// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
/**
 * Execution metrics for update type commands - update, findAndModify and their mongoS variants.
 * Exposes the metrics to the output of the serverStatus command.
 */
class UpdateMetrics {
public:
    /**
     * Construct metrics for a command identified by commandName.
     */
    UpdateMetrics(std::string_view commandName, ClusterRole role);

    /**
     * Increment counter for how many times this command has executed with an aggregation
     * pipeline-style update parameter.
     */
    void incrementExecutedWithAggregationPipeline();

    /**
     * Increment counter for how many times this command has executed with the arrayFilters option.
     */
    void incrementExecutedWithArrayFilters();

    /**
     * Collect update related metrics from the command object for update type commands. Update type
     * commands may have update and arrayFilters fields. Accepts an unparsed command object to
     * support use cases when the command object is not fully parsed by the command.
     */
    void collectMetrics(const BSONObj& cmdObj);

    /**
     * Increments update metrics corresponding to the supplied parameters.
     */
    void collectMetrics(const write_ops::FindAndModifyCommandRequest& cmd);

private:
    // A counter for how many times this command has been executed with an aggregation
    // pipeline-style update parameter.
    Counter64* _commandsWithAggregationPipeline;

    // A counter for how many times this command has been executed with the arrayFilters option.
    Counter64* _commandsWithArrayFilters;
};
}  // namespace mongo
