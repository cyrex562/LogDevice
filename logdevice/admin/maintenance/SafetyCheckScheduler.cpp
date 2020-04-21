/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/admin/maintenance/SafetyCheckScheduler.h"

#include "logdevice/admin/maintenance/SequencerWorkflow.h"
#include "logdevice/admin/maintenance/ShardWorkflow.h"
#include "logdevice/admin/safety/SafetyChecker.h"
#include "logdevice/common/ClusterState.h"
#include "logdevice/common/Processor.h"

namespace facebook { namespace logdevice { namespace maintenance {

folly::SemiFuture<folly::Expected<SafetyCheckScheduler::Result, Status>>
SafetyCheckScheduler::schedule(
    const ClusterMaintenanceWrapper& maintenance_state,
    const ShardAuthoritativeStatusMap& status_map,
    const std::shared_ptr<const configuration::nodes::NodesConfiguration>&
        nodes_config,
    const std::vector<const ShardWorkflow*>& shard_wf,
    const std::vector<const SequencerWorkflow*>& seq_wf,
    const ShardSet& safe_shards,
    NodeLocationScope biggest_replication_scope) const {
  // Build an empty Result object
  Result result;
  // Build Plan.
  auto plan = buildExecutionPlan(maintenance_state,
                                 shard_wf,
                                 seq_wf,
                                 nodes_config,
                                 biggest_replication_scope);
  if (plan.size() == 0) {
    ld_warning(
        "We are producing an empty safety-check execution plan. This should "
        "only happen if the maintenance workflows supplied are empty!");
    return folly::makeUnexpected(E::INVALID_PARAM);
  }
  // Execute Progressively.
  ExecutionState state;
  state.result.safe_shards.insert(safe_shards.begin(), safe_shards.end());
  state.plan = std::move(plan);
  return executePlan(state, status_map, nodes_config);
}

folly::SemiFuture<folly::Expected<SafetyCheckScheduler::Result, Status>>
SafetyCheckScheduler::executePlan(
    ExecutionState state,
    ShardAuthoritativeStatusMap status_map,
    std::shared_ptr<const configuration::nodes::NodesConfiguration>
        nodes_config) const {
  // now, do we have more shards in plan?
  updateResult(state);

  if (state.plan.size() == 0) {
    return std::move(state.result);
  }

  auto safety_check_job = state.plan.front();
  auto shards_sequencers_to_check = safety_check_job.shards_and_seqs;
  // remove it from the plan.
  state.plan.pop_front();
  // We have a plan to execute.
  folly::SemiFuture<folly::Expected<Impact, Status>> safety_future =
      // arguments as passed as copies.
      performSafetyCheck(state.result.safe_shards,
                         state.result.safe_sequencers,
                         status_map,
                         nodes_config,
                         shards_sequencers_to_check.first,
                         shards_sequencers_to_check.second,
                         safety_check_job.safety_margin);

  folly::SemiFuture<folly::Expected<Result, Status>> result_fut =
      std::move(safety_future)
          // We switch into unsafe future because we know that it's safe to do
          // so. The execution will happen on the worker of the safety checker.
          // This will be executed inline in the worker executing the
          // underlying safety check.
          .toUnsafeFuture()
          .thenValue(
              [this,
               status_map{std::move(status_map)},
               nodes_config{std::move(nodes_config)},
               state{std::move(state)},
               group_id{std::move(safety_check_job.group_id)},
               sequencers_to_check{
                   std::move(shards_sequencers_to_check.second)},
               shards_to_check{std::move(shards_sequencers_to_check.first)}](
                  folly::Expected<Impact, Status> expected_impact) mutable
              -> folly::SemiFuture<folly::Expected<Result, Status>> {
                if (expected_impact.hasError()) {
                  ld_error("SafetyChecker cannot execute because: %s",
                           error_name(expected_impact.error()));
                  // We don't want to continue execution if safety checker is
                  // failing. Instead, we fail the entire execution stack.
                  return folly::makeUnexpected(expected_impact.error());
                }

                if (expected_impact->result != 0) {
                  ld_info(
                      "Safety check didn't pass for maintenance %s, impact: %s",
                      group_id.c_str(),
                      expected_impact->toString().c_str());
                } else {
                  ld_info("Safety check passed for maintenance %s",
                          group_id.c_str());
                }
                state.last_check = ExecutionState::LastCheck{
                    .group_id = group_id,
                    .sequencers = sequencers_to_check,
                    .shards = shards_to_check,
                    .impact = std::move(*expected_impact)};
                // Will schedule another parts of the plan via the processor.
                // (async)
                return executePlan(std::move(state), status_map, nodes_config);
              });
  return result_fut;
}

void SafetyCheckScheduler::updateResult(ExecutionState& state) const {
  // We will update the impact of the last_checked sequencers and shards only
  // if:
  //  - We do not have previous results for them.
  //  - Or it's not safe and previous we recorded that it's unsafe.
  //
  if (state.last_check) {
    if (state.last_check->impact.result == Impact::ImpactResult::NONE) {
      // The operation was safe. Add shards and sequencers to the safe list.
      state.result.safe_shards.insert(
          state.last_check->shards.begin(), state.last_check->shards.end());

      state.result.safe_sequencers.insert(state.last_check->sequencers.begin(),
                                          state.last_check->sequencers.end());
    } else {
      // The operation was unsafe. Add the impact to the group-id results.
      state.result.unsafe_groups[state.last_check->group_id] =
          state.last_check->impact;
    }
  }
}

folly::SemiFuture<folly::Expected<Impact, Status>>
SafetyCheckScheduler::performSafetyCheck(
    ShardSet disabled_shards,
    NodeIndexSet disabled_sequencers,
    ShardAuthoritativeStatusMap status_map,
    std::shared_ptr<const configuration::nodes::NodesConfiguration>
        nodes_config,
    ShardSet shards,
    NodeIndexSet sequencers,
    SafetyMargin safety_margin) const {
  // We must have nodes configuration to operate.
  ld_assert(nodes_config != nullptr);
  ld_assert(processor_);
  ld_assert(safety_checker_);

  ld_info("Performing safety check for disabling shards %s while assuming that "
          "%s are already disabled. And disabling sequencers %s while assuming "
          "that sequencers %s are already disabled.",
          toString(shards).c_str(),
          toString(disabled_shards).c_str(),
          toString(sequencers).c_str(),
          toString(disabled_sequencers).c_str());

  // Combine the shards into a single input list to safety checker.
  shards.insert(disabled_shards.begin(), disabled_shards.end());
  sequencers.insert(disabled_sequencers.begin(), disabled_sequencers.end());

  STAT_INCR(processor_->stats_, admin_server.mm_safety_checker_runs);
  return safety_checker_->checkImpact(
      status_map,
      shards,
      sequencers,
      configuration::StorageState::DISABLED, // We always assume that nodes may
                                             // die.
      safety_margin,
      /* check_metadata_logs= */ true,
      /* check_internal_logs= */ true,
      /* check_capacity= */ true,
      settings_->max_unavailable_storage_capacity_pct,
      settings_->max_unavailable_sequencing_capacity_pct,
      /* logids_to_check= */ folly::none);
}

std::deque<SafetyCheckScheduler::SafetyCheckJob>
SafetyCheckScheduler::buildExecutionPlan(
    const ClusterMaintenanceWrapper& maintenance_state,
    const std::vector<const ShardWorkflow*>& shard_wf,
    const std::vector<const SequencerWorkflow*>& seq_wf,
    const std::shared_ptr<const configuration::nodes::NodesConfiguration>&
        nodes_config,
    NodeLocationScope biggest_replication_scope) const {
  // We will iteratively create the plan by building a plan per periority from
  // high to low. We only care about HIGH, MEDIUM, LOW here since IMMINENT will
  // not trigger safety check at all.
  //
  std::deque<SafetyCheckJob> plan;

  SafetyMargin medium_safety_margin;
  // We don't want to set the safety margin if the value is zero for better
  // performance. Safety checking with margin is significatly more expensive
  // (see SafetyCheckerUtils.cpp for the safety_margin.empty() check)
  if (settings_->safety_margin_medium_pri_scopes > 0) {
    medium_safety_margin[biggest_replication_scope] =
        settings_->safety_margin_medium_pri_scopes;
  }
  SafetyMargin low_safety_margin;
  if (settings_->safety_margin_low_pri_scopes > 0) {
    low_safety_margin[biggest_replication_scope] =
        settings_->safety_margin_low_pri_scopes;
  }
  auto high_pri = buildExecutionPlanPerPriority(maintenance_state,
                                                shard_wf,
                                                seq_wf,
                                                nodes_config,
                                                biggest_replication_scope,
                                                MaintenancePriority::HIGH,
                                                // No safety margin for HIGH
                                                // priority maintenances
                                                SafetyMargin());

  auto medium_pri = buildExecutionPlanPerPriority(maintenance_state,
                                                  shard_wf,
                                                  seq_wf,
                                                  nodes_config,
                                                  biggest_replication_scope,
                                                  MaintenancePriority::MEDIUM,
                                                  medium_safety_margin);

  auto low_pri = buildExecutionPlanPerPriority(maintenance_state,
                                               shard_wf,
                                               seq_wf,
                                               nodes_config,
                                               biggest_replication_scope,
                                               MaintenancePriority::LOW,
                                               low_safety_margin);

  // Combining results
  plan.insert(plan.end(), high_pri.begin(), high_pri.end());
  plan.insert(plan.end(), medium_pri.begin(), medium_pri.end());
  plan.insert(plan.end(), low_pri.begin(), low_pri.end());
  return plan;
}

std::deque<SafetyCheckScheduler::SafetyCheckJob>
SafetyCheckScheduler::buildExecutionPlanPerPriority(
    const ClusterMaintenanceWrapper& maintenance_state,
    const std::vector<const ShardWorkflow*>& shard_wf,
    const std::vector<const SequencerWorkflow*>& seq_wf,
    const std::shared_ptr<const configuration::nodes::NodesConfiguration>&
        nodes_config,
    NodeLocationScope biggest_replication_scope,
    MaintenancePriority priority,
    SafetyMargin safety_margin) const {
  //
  // We want to sort the shard and sequencer maintenances so that we process
  // maintenances for shards that are in the same (biggest) replication
  // location. Maintenances that span multiple locations (grouped) are processed
  // next.
  //
  // We assume that all the workflows passed to us require safety checks.
  // We also assume that all these shards already exist in the
  // ClusterMaintenanceWrapper as well.
  std::deque<SafetyCheckJob> plan;
  // Stage 1: Group the shards into their corresponding groups.
  std::vector<ShardID> all_shards;
  for (const auto* workflow : shard_wf) {
    ld_assert(workflow != nullptr);
    all_shards.push_back(workflow->getShardID());
  }

  std::vector<node_index_t> all_sequencers;
  for (const auto* workflow : seq_wf) {
    ld_assert(workflow != nullptr);
    all_sequencers.push_back(workflow->getNodeIndex());
  }

  // A mapping from a group ID to the set of shards.
  folly::F14FastMap<GroupID, ShardSet> group_to_shards =
      maintenance_state.groupShardsByGroupID(all_shards, priority);

  // A mapping from a group ID to the set of sequencer node ids.
  folly::F14FastMap<GroupID, NodeIndexSet> group_to_sequencers =
      maintenance_state.groupSequencersByGroupID(all_sequencers, priority);
  // Stage 2: Order the shard sets as following:
  //   1. Group all MAY_DISAPPEAR groups first as we know that these are
  //   short-term maintenances.
  //   2. The MAY_DISAPPEAR maintenances should be sorted where the maintenances
  //   that have a single scope come first. The maintenances that have the same
  //   maintenance scope should be next to each other.
  //   3. For the MAY_DISAPPEAR groups, order by creation_time within the
  //   location scope boundry. First-come should be allowed to be served first.
  //   3. DRAINED targets are grouped afterwards with the same sub-ordering
  //   strategy.
  std::vector<MaintenanceManifest> may_disappear;
  std::vector<MaintenanceManifest> drained;

  // Finding all may_disappear groups that have shards in the workflows, if
  for (const auto& it : group_to_shards) {
    const GroupID& group_id = it.first;
    auto* maintenance = maintenance_state.getMaintenanceByGroupID(group_id);
    ld_check(maintenance != nullptr);
    MaintenanceManifest manifest{};
    manifest.group_id = group_id;
    manifest.timestamp = maintenance->created_on_ref().value_or(0);
    // Do we have sequencer disable requests for this group?
    NodeIndexSet sequencers_to_disable;
    auto seq_it = group_to_sequencers.find(group_id);
    if (seq_it != group_to_sequencers.end()) {
      // We have sequencers to disable here.
      sequencers_to_disable = seq_it->second;
      // go over these sequencer nodes, and find the location for the
      // replication scope (biggest_replication_scope)
      for (node_index_t seq_id : sequencers_to_disable) {
        const auto* sd = nodes_config->getNodeServiceDiscovery(seq_id);
        if (sd != nullptr && sd->location) {
          // Add the location to the locations set for this maintenance.
          manifest.locations.insert(
              sd->location->getDomain(biggest_replication_scope, seq_id));
        }
      }
      // remove it from the groups, as we have picked that up already.
      group_to_sequencers.erase(seq_it);
    }

    manifest.shards_and_seqs = std::make_pair(it.second, sequencers_to_disable);

    // Go over the shards and collect all the locations for the
    // scope (biggest_replication_scope)
    for (const ShardID& shard : manifest.shards_and_seqs.first) {
      const auto* sd = nodes_config->getNodeServiceDiscovery(shard.node());
      if (sd != nullptr && sd->location) {
        // Add the location to the locations set for this maintenance.
        manifest.locations.insert(
            sd->location->getDomain(biggest_replication_scope, shard.node()));
      }
    }

    if (maintenance->shard_target_state ==
        ShardOperationalState::MAY_DISAPPEAR) {
      may_disappear.push_back(std::move(manifest));
    } else {
      ld_check(maintenance->shard_target_state ==
               ShardOperationalState::DRAINED);
      drained.push_back(std::move(manifest));
    }
  }
  may_disappear = sortAndGroupMaintenances(std::move(may_disappear));
  drained = sortAndGroupMaintenances(std::move(drained));

  // Build the plan, we start by the MAY_DISAPPEAR maintenances
  std::for_each(may_disappear.begin(),
                may_disappear.end(),
                [&](const MaintenanceManifest& v) {
                  plan.push_back(SafetyCheckJob{
                      .group_id = v.group_id,
                      .shards_and_seqs = v.shards_and_seqs,
                      .safety_margin = safety_margin,
                  });
                });

  std::for_each(
      drained.begin(), drained.end(), [&](const MaintenanceManifest& v) {
        plan.push_back(SafetyCheckJob{
            .group_id = v.group_id,
            .shards_and_seqs = v.shards_and_seqs,
            .safety_margin = safety_margin,
        });
      });

  // Do we have sequencer-only maintenances still left?
  if (group_to_sequencers.size() > 0) {
    for (const auto& it : group_to_sequencers) {
      ShardSet empty;
      plan.push_back(SafetyCheckJob{
          .group_id = it.first,
          .shards_and_seqs = std::make_pair(empty, it.second),
          .safety_margin = safety_margin,
      });
    }
  }

  ld_check(plan.size() ==
           may_disappear.size() + drained.size() + group_to_sequencers.size());
  return plan;
}

std::vector<SafetyCheckScheduler::MaintenanceManifest>
SafetyCheckScheduler::sortAndGroupMaintenances(
    std::vector<SafetyCheckScheduler::MaintenanceManifest>&& input) const {
  // We pay special attention to maintenances that do not span across multiple
  // replication scopes, we try to group them together and sort each by
  // timestamp.
  //
  // Maintenance groups that span multiple failure domains we process next since
  // their chance is lower in passing safety check and we might hit the capacity
  // checker limits before achieving them.
  //
  // For each of the list that we will generate, we want to sort by timestamp.
  // - We we want to process the oldest single-failure-domain maintenances
  // first.
  // - Then the oldest multi-failure-domain maintenances after that.
  //
  std::vector<MaintenanceManifest> output;
  // Maps location-string => [MaintenanceManifest]
  folly::F14FastMap<std::string, std::vector<MaintenanceManifest>>
      maintenances_with_single_fd;

  // Remove the maintenances that have one location scope and move to the map.
  folly::F14FastMap<std::string, int64_t> location_to_oldest_timestamp;
  auto it = input.begin();
  while (it != input.end()) {
    if (it->locations.size() == 1) {
      auto loc = *it->locations.begin();
      // Let's insert this into the sorted vector value.
      auto& vec = maintenances_with_single_fd[loc];
      // Using binary-search to keep this vector sorted by timestamp.
      vec.insert(std::upper_bound(vec.begin(),
                                  vec.end(),
                                  *it,
                                  [](const auto& i, const auto& j) {
                                    return i.timestamp < j.timestamp;
                                  }),
                 *it);

      // We use this map to index what is the oldest maintenance in this
      // location scope.
      if (location_to_oldest_timestamp.count(loc) > 0) {
        if (it->timestamp < location_to_oldest_timestamp[loc]) {
          // Only replace the value if this maintenance is older than the one we
          // have seen previously.
          location_to_oldest_timestamp[loc] = it->timestamp;
        }
      } else {
        // first time to see this location, store the timestamp.
        location_to_oldest_timestamp[loc] = it->timestamp;
      }
      it = input.erase(it);
    } else {
      ++it;
    }
  }

  // For the rest of the maintenances, let's sort by timestamp too.
  std::sort(input.begin(),
            input.end(),
            [](const MaintenanceManifest& i, const MaintenanceManifest& j) {
              return i.timestamp < j.timestamp;
            });

  // This looks into the different location scopes and pick the scope that has
  // the oldest maintenance to be poped.
  while (!location_to_oldest_timestamp.empty()) {
    auto it = std::min_element(
        location_to_oldest_timestamp.begin(),
        location_to_oldest_timestamp.end(),
        [](const auto& i, const auto& j) { return i.second < j.second; });
    const auto& maintenances = maintenances_with_single_fd[it->first];
    std::copy(
        maintenances.begin(), maintenances.end(), std::back_inserter(output));
    // Remove it from maintenances and move to the next oldest.
    maintenances_with_single_fd.erase(it->first);
    location_to_oldest_timestamp.erase(it->first);
  }
  // Everything we have left is now part of the output;
  std::copy(input.begin(), input.end(), std::back_inserter(output));
  return output;
}
}}} // namespace facebook::logdevice::maintenance
