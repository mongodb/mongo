// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/unified_write_executor/write_op.h"
#include "mongo/s/write_ops/write_command_ref.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace unified_write_executor {

/**
 * Class which records and updates targeting statistics along with other metrics (such as op
 * counters).
 */
class Stats {
public:
    /**
     * Struct which groups together common targeting statistcs.
     */
    struct TargetingStats {
        int numShardsOwningChunks;
        bool isSharded;
        stdx::unordered_map<WriteType, stdx::unordered_set<ShardId>> targetedShardsByWriteType;
    };

    /**
     * Method to record targeting stats.
     */
    void recordTargetingStats(const std::vector<ShardEndpoint>& targetedShardEndpoints,
                              size_t nsIdx,
                              bool isNsSharded,
                              int numShardsOwningChunks,
                              WriteType writeType);

    /**
     * Method to update CurOp and NumHostsTargetedMetrics.
     */
    void updateMetrics(OperationContext* opCtx, bool updatedShardKey);

    /**
     * Helper to increment query counters for 'op'.
     */
    void incrementOpCounters(OperationContext* opCtx,
                             WriteCommandRef::OpRef op,
                             bool statusOkOrNotWCOS);

    const stdx::unordered_map<size_t, TargetingStats>& getTargetingStatsMap() {
        return _targetingStatsMap;
    }

    const stdx::unordered_set<ShardId>& getTargetedShards() {
        return _targetedShards;
    }

private:
    stdx::unordered_map<size_t, TargetingStats> _targetingStatsMap;
    stdx::unordered_set<ShardId> _targetedShards;
};

}  // namespace unified_write_executor
}  // namespace mongo
