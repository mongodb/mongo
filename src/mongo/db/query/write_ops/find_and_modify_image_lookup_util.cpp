/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/db/query/write_ops/find_and_modify_image_lookup_util.h"

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/local_executor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/server_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace {

/**
 * Extracts the _id filter for the findAndModify operation with the given oplog entry.
 */
BSONObj extractFindAndModifyIdFilter(const repl::OplogEntry& oplogEntry) {
    auto idField = [&] {
        switch (oplogEntry.getOpType()) {
            case repl::OpTypeEnum::kUpdate:
                return oplogEntry.getObject2().get_value_or({})["_id"];
            case repl::OpTypeEnum::kDelete:
                return oplogEntry.getObject()["_id"];
            default:
                uasserted(11730900,
                          str::stream()
                              << "Found a findAndModify oplog entry with an unexpected op type "
                              << OpType_serializer(oplogEntry.getOpType()));
        }
    }();
    uassert(11730901,
            str::stream() << "Expected a findAndModify oplog entry to have an '_id' field "
                          << redact(oplogEntry.toBSONForLogging()),
            !idField.eoo());
    return idField.wrap();
}
}  // namespace

boost::optional<BSONObj> fetchPreOrPostImageFromSnapshot(OperationContext* opCtx,
                                                         const repl::OplogEntry& oplogEntry) {
    invariant(oplogEntry.getNeedsRetryImage());

    auto idFilter = extractFindAndModifyIdFilter(oplogEntry);
    auto opTimestamp = oplogEntry.getCommitTransactionTimestamp()
        ? *oplogEntry.getCommitTransactionTimestamp()
        : oplogEntry.getTimestamp();

    // Set up a separate OperationContext since waiting for read concern is not supported running a
    // transaction and the caller may be handling a retry in a retryable internal transaction.
    auto newClient = opCtx->getService()->makeClient("fetchPreOrPostImageFromSnapshot");
    auto executor = getLocalExecutor(opCtx);
    AlternativeClientRegion acr(newClient);
    CancelableOperationContext newOpCtx(
        cc().makeOperationContext(), opCtx->getCancellationToken(), executor);

    repl::ReadConcernArgs snapshotReadConcern(repl::ReadConcernLevel::kSnapshotReadConcern);
    snapshotReadConcern.setArgsAtClusterTimeForSnapshot(
        oplogEntry.getNeedsRetryImage() == repl::RetryImageEnum::kPostImage ? opTimestamp
                                                                            : opTimestamp - 1);

    repl::ReadConcernArgs::get(newOpCtx.get()) = snapshotReadConcern;
    DBDirectClient client(newOpCtx.get());
    try {
        FindCommandRequest findRequest(oplogEntry.getNss());
        findRequest.setFilter(idFilter);
        if (gFeatureFlagAllBinariesSupportRawDataOperations.isEnabledUseLatestFCVWhenUninitialized(
                VersionContext::getDecoration(newOpCtx.get()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            // This must be set for the request to work against a timeseries collection.
            findRequest.setRawData(true);
        }
        auto cursor = client.find(findRequest);
        tassert(11730902,
                str::stream() << "Could not find the document that the findAndModify operation "
                                 "wrote to in the snapshot for "
                              << oplogEntry.getTimestamp(),
                cursor->more());
        auto doc = cursor->next();
        tassert(
            11730903,
            str::stream() << "Found multiple documents with _id that the findAndModify operation "
                             "wrote to in the snapshot for "
                          << oplogEntry.getTimestamp(),
            !cursor->more());
        return doc;
    } catch (const ExceptionFor<ErrorCategory::SnapshotError>&) {
        return boost::none;
    }
}

}  // namespace mongo
