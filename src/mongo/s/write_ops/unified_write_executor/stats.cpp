// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/write_ops/unified_write_executor/stats.h"

#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/sharding_environment/client/num_hosts_targeted_metrics.h"
#include "mongo/db/stats/counters.h"

namespace mongo {
namespace unified_write_executor {

void Stats::recordTargetingStats(const std::vector<ShardEndpoint>& targetedShardEndpoints,
                                 size_t nsIdx,
                                 bool isNsSharded,
                                 int numShardsOwningChunks,
                                 WriteType writeType) {
    stdx::unordered_set<ShardId> shards;
    for (const auto& endpoint : targetedShardEndpoints) {
        shards.insert(endpoint.shardName);
    }

    if (!_targetingStatsMap.contains(nsIdx)) {
        _targetingStatsMap[nsIdx] = TargetingStats();
    }
    TargetingStats& targetingStats = _targetingStatsMap.at(nsIdx);
    targetingStats.numShardsOwningChunks = numShardsOwningChunks;
    targetingStats.isSharded = isNsSharded;
    auto& nsWriteTypeShardSet = targetingStats.targetedShardsByWriteType[writeType];
    for (const auto& shardId : shards) {
        _targetedShards.insert(shardId);
        nsWriteTypeShardSet.insert(shardId);
    }
}

void Stats::updateMetrics(OperationContext* opCtx, bool updatedShardKey) {
    // Record the number of shards targeted by this write.
    CurOp::get(opCtx)->debug().nShards = _targetedShards.size();

    for (const auto& [nsIdx, targetingStats] : _targetingStatsMap) {
        const bool isSharded = targetingStats.isSharded;
        const int nShardsOwningChunks = targetingStats.numShardsOwningChunks;

        if (nShardsOwningChunks == 0) {
            continue;
        }

        for (const auto& [writeType, shards] : targetingStats.targetedShardsByWriteType) {
            int perWriteNShards = shards.size();

            // If we have no information on the shards targeted, ignore updatedShardKey,
            // updateHostsTargetedMetrics will report this as TargetType::kManyShards.
            if (perWriteNShards != 0 && updatedShardKey) {
                perWriteNShards += 1;
            }

            NumHostsTargetedMetrics::QueryType metricsWriteType;
            switch (writeType) {
                case WriteType::kInsert:
                    metricsWriteType = NumHostsTargetedMetrics::QueryType::kInsertCmd;
                    break;
                case WriteType::kUpdate:
                    metricsWriteType = NumHostsTargetedMetrics::QueryType::kUpdateCmd;
                    break;
                case WriteType::kDelete:
                    metricsWriteType = NumHostsTargetedMetrics::QueryType::kDeleteCmd;
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
            auto targetType = NumHostsTargetedMetrics::get(opCtx).parseTargetType(
                opCtx, perWriteNShards, nShardsOwningChunks, isSharded);
            NumHostsTargetedMetrics::get(opCtx).addNumHostsTargeted(metricsWriteType, targetType);
        }
    }
}

void Stats::incrementOpCounters(OperationContext* opCtx,
                                WriteCommandRef::OpRef op,
                                bool statusOkOrNotWCOS) {
    if (op.isInsertOp()) {
        globalOpCounters().gotInsert();

    } else if (op.isUpdateOp()) {
        if (statusOkOrNotWCOS) {
            globalOpCounters().gotUpdate();
        }

        auto updateRef = op.getUpdateOp();
        // 'isMulti' is set to false as the metrics for multi updates were registered
        // for each operation individually.
        bulk_write_common::incrementBulkWriteUpdateMetrics(getQueryCounters(opCtx),
                                                           ClusterRole::RouterServer,
                                                           // update
                                                           updateRef.getUpdateMods(),
                                                           op.getNss(),
                                                           updateRef.getArrayFilters(),
                                                           false /* isMulti */);
    } else if (op.isDeleteOp()) {
        globalOpCounters().gotDelete();
    }
}
}  // namespace unified_write_executor
}  // namespace mongo
