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


#include "mongo/logv2/log.h"

#include "mongo/platform/basic.h"

#include "mongo/s/cluster_write.h"

#include "mongo/db/fle_crud.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace cluster {

void write(OperationContext* opCtx,
           const BatchedCommandRequest& request,
           BatchWriteExecStats* stats,
           BatchedCommandResponse* response,
           boost::optional<OID> targetEpoch) {
    if (request.hasEncryptionInformation()) {
        FLEBatchResult result = processFLEBatch(opCtx, request, stats, response, targetEpoch);
        if (result == FLEBatchResult::kProcessed) {
            return;
        }

        // fall through
    }

    NotPrimaryErrorTracker::Disabled scopeDisabledTracker(
        &NotPrimaryErrorTracker::get(opCtx->getClient()));

    CollectionRoutingInfoTargeter targeter(opCtx, request.getNS(), targetEpoch);

    LOGV2_DEBUG_OPTIONS(
        4817400, 2, {logv2::LogComponent::kShardMigrationPerf}, "Starting batch write");

    BatchWriteExec::executeBatch(opCtx, targeter, request, response, stats);

    LOGV2_DEBUG_OPTIONS(
        4817401, 2, {logv2::LogComponent::kShardMigrationPerf}, "Finished batch write");
}

std::vector<BulkWriteReplyItem> bulkWrite(OperationContext* opCtx,
                                          const BulkWriteCommandRequest& request) {
    std::vector<std::unique_ptr<NSTargeter>> targeters;
    targeters.reserve(request.getNsInfo().size());
    for (const auto& nsInfo : request.getNsInfo()) {
        targeters.push_back(std::make_unique<CollectionRoutingInfoTargeter>(opCtx, nsInfo.getNs()));
    }

    return bulk_write_exec::execute(opCtx, targeters, request);
}

}  // namespace cluster
}  // namespace mongo
