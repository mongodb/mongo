// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/query_cmd/update_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/query_cmd/cmd_specific_metric_helpers.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"

#include <string_view>

namespace mongo {
UpdateMetrics::UpdateMetrics(std::string_view commandName, ClusterRole role)
    : _commandsWithAggregationPipeline(
          getSingletonMetricPtrWithinCmd(commandName, "pipeline", role)),
      _commandsWithArrayFilters(getSingletonMetricPtrWithinCmd(commandName, "arrayFilters", role)) {
}

void UpdateMetrics::incrementExecutedWithAggregationPipeline() {
    _commandsWithAggregationPipeline->increment();
}

void UpdateMetrics::incrementExecutedWithArrayFilters() {
    _commandsWithArrayFilters->increment();
}

void UpdateMetrics::collectMetrics(const BSONObj& cmdObj) {
    // If this command is a pipeline-style update, record that it was used.
    if (cmdObj.hasField("update") && (cmdObj.getField("update").type() == BSONType::array)) {
        _commandsWithAggregationPipeline->increment();
    }

    // If this command had arrayFilters option, record that it was used.
    if (cmdObj.hasField("arrayFilters")) {
        _commandsWithArrayFilters->increment();
    }
}

void UpdateMetrics::collectMetrics(const write_ops::FindAndModifyCommandRequest& cmd) {
    if (auto update = cmd.getUpdate()) {
        if (update->type() == write_ops::UpdateModification::Type::kPipeline) {
            _commandsWithAggregationPipeline->increment();
        }
    }

    if (cmd.getArrayFilters()) {
        _commandsWithArrayFilters->increment();
    }
}

}  // namespace mongo
