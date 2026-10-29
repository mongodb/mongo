// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/write_ops/timeseries_write_ops.h"

#include "mongo/db/curop.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"
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
        std::lock_guard<Client> lk(*opCtx->getClient());
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
        curOp.debug().getAdditiveMetrics().incrementNinserted(0);
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

    curOp->debug().getAdditiveMetrics().ninserted = baseReply.getN();
    globalOpCounters().gotInserts(baseReply.getN());
    ServerWriteConcernMetrics::get(opCtx)->recordWriteConcernForInserts(opCtx->getWriteConcern(),
                                                                        baseReply.getN());

    return insertReply;
}

}  // namespace mongo::timeseries::write_ops
