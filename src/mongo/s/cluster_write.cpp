// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/cluster_write.h"

#include "mongo/db/fle_crud.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/router_role/ns_targeter.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/bulk_write_exec.h"
#include "mongo/s/write_ops/fle.h"
#include "mongo/s/write_ops/unified_write_executor/stats.h"
#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"
#include "mongo/util/decorable.h"

#include <memory>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

// Utility to port 'uweStats' to 'batchStats'. This is done to enable re-using the existing logic
// for cluster write statistics.
// TODO SERVER-109104: This can be removed once we delete BatchWriteExec.
void fillOutBatchWriteStats(unified_write_executor::Stats& uweStats,
                            BatchWriteExecStats& batchStats) {
    const auto& map = uweStats.getTargetingStatsMap();
    if (map.empty()) {
        return;
    }
    tassert(11499200,
            "Expected only a single entry in targeting stats map for a batch write",
            map.size() == 1 && map.count(0) != 0);
    const auto& targetingStats = map.at(0);
    batchStats.noteNumShardsOwningChunks(targetingStats.numShardsOwningChunks);
    batchStats.noteTargetedCollectionIsSharded(targetingStats.isSharded);
    for (const auto& shard : uweStats.getTargetedShards()) {
        batchStats.noteTargetedShard(shard);
    }
}
}  // namespace
namespace cluster {

void write(OperationContext* opCtx,
           const BatchedCommandRequest& request,
           NamespaceString* nss,
           BatchWriteExecStats* stats,
           BatchedCommandResponse* response,
           boost::optional<OID> targetEpoch) {
    NotPrimaryErrorTracker::Disabled scopeDisabledTracker(
        &NotPrimaryErrorTracker::get(opCtx->getClient()));

    tassert(11499201, "Should have initialized 'stats' object", stats);
    CollectionRoutingInfoTargeter targeter(opCtx, request.getNS(), targetEpoch);
    if (nss) {
        *nss = targeter.getNS();
    }

    LOGV2_DEBUG_OPTIONS(
        4817400, 2, {logv2::LogComponent::kShardMigrationPerf}, "Starting batch write");

    if (unified_write_executor::isEnabled(opCtx)) {
        unified_write_executor::Stats uweStats;
        *response = unified_write_executor::write(opCtx, request, uweStats, targetEpoch);
        // SERVER-109104 This can be removed once we delete BatchWriteExec.
        fillOutBatchWriteStats(uweStats, *stats);
    } else {
        // Create an RAII object that prints the collection's shard key in the case of a tassert
        // or crash.
        ScopedDebugInfo shardKeyDiagnostics(
            "ShardKeyDiagnostics",
            diagnostic_printers::ShardKeyDiagnosticPrinter{
                targeter.isTargetedCollectionSharded()
                    ? targeter.getRoutingInfo().getChunkManager().getShardKeyPattern().toBSON()
                    : BSONObj()});

        if (request.hasEncryptionInformation()) {
            FLEBatchResult result = processFLEBatch(opCtx, request, response);
            if (result == FLEBatchResult::kProcessed) {
                return;
            }

            // fall through
        }

        BatchWriteExec::executeBatch(opCtx, targeter, request, response, stats);
    }
    LOGV2_DEBUG_OPTIONS(
        4817401, 2, {logv2::LogComponent::kShardMigrationPerf}, "Finished batch write");
}

bulk_write_exec::BulkWriteReplyInfo bulkWrite(
    OperationContext* opCtx,
    const BulkWriteCommandRequest& request,
    const std::vector<std::unique_ptr<NSTargeter>>& targeters,
    bulk_write_exec::BulkWriteExecStats& execStats) {
    if (unified_write_executor::isEnabled(opCtx)) {
        unified_write_executor::Stats uweStats;
        return unified_write_executor::bulkWrite(opCtx, request, uweStats);
    } else {
        if (request.getNsInfo()[0].getEncryptionInformation().has_value()) {
            auto [result, replies] = attemptExecuteFLE(opCtx, request);
            if (result == FLEBatchResult::kProcessed) {
                return replies;
            }  // else fallthrough.
        }
        return bulk_write_exec::execute(opCtx, targeters, request, execStats);
    }
}

}  // namespace cluster
}  // namespace mongo
