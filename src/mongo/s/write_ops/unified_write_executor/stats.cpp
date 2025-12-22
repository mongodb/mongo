/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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

void Stats::updateMetrics(OperationContext* opCtx) {
    // Record the number of shards targeted by this write.
    // TODO SERVER-114992 increment 'nShards' by 1 if we've targeted shards and updated the shard
    // key.
    CurOp::get(opCtx)->debug().nShards = _targetedShards.size();

    for (const auto& [nsIdx, targetingStats] : _targetingStatsMap) {
        const bool isSharded = targetingStats.isSharded;
        const int nShardsOwningChunks = targetingStats.numShardsOwningChunks;

        for (const auto& [writeType, shards] : targetingStats.targetedShardsByWriteType) {
            const int perWriteNShards = shards.size();

            // TODO SERVER-114992: add one to 'nShards' if updated shard key. This information is
            // returned from WCOS handling, see batch_write_exec.cpp
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

void Stats::incrementOpCounters(OperationContext* opCtx, WriteCommandRef::OpRef op) {
    if (op.isInsertOp()) {
        serviceOpCounters(ClusterRole::RouterServer).gotInsert();

    } else if (op.isUpdateOp()) {
        serviceOpCounters(ClusterRole::RouterServer).gotUpdate();

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
        serviceOpCounters(ClusterRole::RouterServer).gotDelete();
    }
}
}  // namespace unified_write_executor
}  // namespace mongo
