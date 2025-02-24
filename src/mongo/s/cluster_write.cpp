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


#include <boost/move/utility_core.hpp>
#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/db/fle_crud.h"
#include "mongo/db/not_primary_error_tracker.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_write.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/s/ns_targeter.h"
#include "mongo/s/write_ops/bulk_write_exec.h"
#include "mongo/util/decorable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace cluster {

void write(OperationContext* opCtx,
           const BatchedCommandRequest& request,
           NamespaceString* nss,
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

    if (nss) {
        *nss = targeter.getNS();
    }

    LOGV2_DEBUG_OPTIONS(
        4817400, 2, {logv2::LogComponent::kShardMigrationPerf}, "Starting batch write");

    BatchWriteExec::executeBatch(opCtx, targeter, request, response, stats);

    LOGV2_DEBUG_OPTIONS(
        4817401, 2, {logv2::LogComponent::kShardMigrationPerf}, "Finished batch write");
}

bulk_write_exec::BulkWriteReplyInfo bulkWrite(
    OperationContext* opCtx,
    const BulkWriteCommandRequest& request,
    const std::vector<std::unique_ptr<NSTargeter>>& targeters) {
    if (request.getNsInfo().size() > 1) {
        for (const auto& nsInfo : request.getNsInfo()) {
            uassert(ErrorCodes::BadValue,
                    "BulkWrite with Queryable Encryption supports only a single namespace.",
                    !nsInfo.getEncryptionInformation().has_value());
        }
    } else if (request.getNsInfo()[0].getEncryptionInformation().has_value()) {
        auto [result, replies] = bulk_write_exec::attemptExecuteFLE(opCtx, request);
        if (result == FLEBatchResult::kProcessed) {
            return replies;
        }  // else fallthrough.
    }

    return bulk_write_exec::execute(opCtx, targeters, request);
}

}  // namespace cluster
}  // namespace mongo
