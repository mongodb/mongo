/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/s/cluster_write.h"

#include "mongo/db/fle_crud.h"
#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"
#include "mongo/db/global_catalog/router_role_api/ns_targeter.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/bulk_write_exec.h"
#include "mongo/s/write_ops/fle.h"
#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"
#include "mongo/util/decorable.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace cluster {

void write(OperationContext* opCtx,
           const BatchedCommandRequest& request,
           NamespaceString* nss,
           BatchWriteExecStats* stats,
           BatchedCommandResponse* response,
           boost::optional<OID> targetEpoch) {
    NotPrimaryErrorTracker::Disabled scopeDisabledTracker(
        &NotPrimaryErrorTracker::get(opCtx->getClient()));

    CollectionRoutingInfoTargeter targeter(opCtx, request.getNS(), targetEpoch);

    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics(
        "ShardKeyDiagnostics",
        diagnostic_printers::ShardKeyDiagnosticPrinter{
            targeter.isTargetedCollectionSharded()
                ? targeter.getRoutingInfo().getChunkManager().getShardKeyPattern().toBSON()
                : BSONObj()});

    if (nss) {
        *nss = targeter.getNS();
    }

    LOGV2_DEBUG_OPTIONS(
        4817400, 2, {logv2::LogComponent::kShardMigrationPerf}, "Starting batch write");

    // TODO SERVER-104145: Enable insert/update/delete commands from internal clients.
    if (unified_write_executor::isEnabled(opCtx)) {
        *response = unified_write_executor::write(opCtx, request);
        // SERVER-109104 This can be removed once we delete BatchWriteExec.
        stats->markIgnore();
    } else {
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
    if (request.getNsInfo()[0].getEncryptionInformation().has_value()) {
        auto [result, replies] = attemptExecuteFLE(opCtx, request);
        if (result == FLEBatchResult::kProcessed) {
            return replies;
        }  // else fallthrough.
    }

    return bulk_write_exec::execute(opCtx, targeters, request, execStats);
}

}  // namespace cluster
}  // namespace mongo
