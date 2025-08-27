/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"

#include "mongo/db/curop.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/timeseries/write_ops/internal/timeseries_write_ops_internal.h"
#include "mongo/db/transaction/retryable_writes_stats.h"

namespace mongo::timeseries::write_ops {

mongo::write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const timeseries::CollectionPreConditions& preConditions) {

    auto& curOp = *CurOp::get(opCtx);
    ON_BLOCK_EXIT([&] {
        // This is the only part of finishCurOp we need to do for inserts because they reuse
        // the top-level curOp. The rest is handled by the top-level entrypoint.
        curOp.done();
        Top::getDecoration(opCtx).record(opCtx,
                                         internal::ns(request),
                                         LogicalOp::opInsert,
                                         Top::LockType::WriteLocked,
                                         curOp.elapsedTimeExcludingPauses(),
                                         curOp.isCommand(),
                                         curOp.getReadWriteType());
    });

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        auto requestNs = internal::ns(request);
        // TODO SERVER-101784: Remove the following translation once 9.0 is LTS and viewful
        // time-series collections no longe rexist.
        curOp.setNS(lk,
                    requestNs.isTimeseriesBucketsCollection()
                        ? requestNs.getTimeseriesViewNamespace()
                        : requestNs);
        curOp.setLogicalOp(lk, LogicalOp::opInsert);
        curOp.ensureStarted();
        // Initialize 'ninserted' for the operation if is not yet.
        curOp.debug().additiveMetrics.incrementNinserted(0);
    }

    return performTimeseriesWrites(opCtx, request, preConditions, &curOp);
}

mongo::write_ops::InsertCommandReply performTimeseriesWrites(
    OperationContext* opCtx,
    const mongo::write_ops::InsertCommandRequest& request,
    const timeseries::CollectionPreConditions& preConditions,
    CurOp* curOp) {

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot insert into a time-series collection in a multi-document "
                             "transaction: "
                          << internal::ns(request).toStringForErrorMsg(),
            !opCtx->inMultiDocumentTransaction());

    std::vector<mongo::write_ops::WriteError> errors;
    boost::optional<repl::OpTime> opTime;
    boost::optional<OID> electionId;
    bool containsRetry = false;

    mongo::write_ops::InsertCommandReply insertReply;
    auto& baseReply = insertReply.getWriteCommandReplyBase();

    if (request.getOrdered()) {
        baseReply.setN(internal::performOrderedTimeseriesWrites(
            opCtx, request, preConditions, &errors, &opTime, &electionId, &containsRetry));
    } else {

        internal::performUnorderedTimeseriesWritesWithRetries(
            opCtx,
            request,
            preConditions,
            0,
            request.getDocuments().size(),
            bucket_catalog::AllowQueryBasedReopening::kAllow,
            &errors,
            &opTime,
            &electionId,
            &containsRetry);
        baseReply.setN(request.getDocuments().size() - errors.size());
    }

    if (!errors.empty()) {
        baseReply.setWriteErrors(std::move(errors));
    }
    if (opTime) {
        baseReply.setOpTime(*opTime);
    }
    if (electionId) {
        baseReply.setElectionId(*electionId);
    }
    if (containsRetry) {
        RetryableWritesStats::get(opCtx)->incrementRetriedCommandsCount();
    }

    curOp->debug().additiveMetrics.ninserted = baseReply.getN();
    serviceOpCounters(opCtx).gotInserts(baseReply.getN());
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInserts(opCtx->getWriteConcern(),
                                                                        baseReply.getN());

    return insertReply;
}

}  // namespace mongo::timeseries::write_ops
