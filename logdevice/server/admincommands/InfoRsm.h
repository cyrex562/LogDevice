/**
 * Copyright (c) 2019-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include "logdevice/common/AdminCommandTable.h"
#include "logdevice/server/admincommands/AdminCommand.h"

namespace facebook { namespace logdevice { namespace commands {

class InfoRsm : public AdminCommand {
  using AdminCommand::AdminCommand;

 private:
  bool json_ = false;
  node_index_t node_idx_{-1};

 public:
  void getOptions(boost::program_options::options_description& opts) override {
    opts.add_options()(
        "node-idx", boost::program_options::value<node_index_t>(&node_idx_))(
        "json", boost::program_options::bool_switch(&json_));
  }

  void getPositionalOptions(
      boost::program_options::positional_options_description& opts) override {
    opts.add("node-idx", 1);
  }

  std::string getUsage() override {
    return "info rsm [node idx] [--json]";
  }

  void run() override {
    printRsmVersions();
  }

  void printRsmVersions() {
    auto fd = server_->getServerProcessor()->failure_detector_.get();
    if (fd == nullptr) {
      out_.printf("Failure detector not present.\r\n");
      return;
    }

    const auto& nodes_configuration =
        server_->getProcessor()->getNodesConfiguration();
    node_index_t lo = 0;
    node_index_t hi = nodes_configuration->getMaxNodeIndex();

    if (node_idx_ != node_index_t(-1)) {
      if (node_idx_ > hi) {
        out_.printf("Node index expected to be in the [0, %u] range\r\n", hi);
        return;
      }
      lo = hi = node_idx_;
    }

    InfoRsmTable table(!json_,
                       "Node ID",
                       "State",
                       "logsconfig in-memory version",
                       "logsconfig durable version",
                       "eventlog in-memory version",
                       "eventlog durable version");

    for (node_index_t idx = lo; idx <= hi; ++idx) {
      if (!nodes_configuration->isNodeInServiceDiscoveryConfig(idx)) {
        continue;
      }
      std::map<logid_t, lsn_t> node_rsm_info;
      fd->getRSMVersionsForNode(idx, table);
    }
    json_ ? table.printJson(out_) : table.print(out_);
  }
};

}}} // namespace facebook::logdevice::commands
