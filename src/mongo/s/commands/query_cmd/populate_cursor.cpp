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

#include "mongo/s/commands/query_cmd/populate_cursor.h"

#include "mongo/db/commands/query_cmd/bulk_write_common.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"
#include "mongo/s/query/exec/cluster_client_cursor_guard.h"
#include "mongo/s/query/exec/cluster_client_cursor_impl.h"
#include "mongo/s/query/exec/cluster_client_cursor_params.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/query/exec/router_exec_stage.h"
#include "mongo/s/query/exec/router_stage_queued_data.h"

namespace mongo {

BulkWriteCommandReply populateCursorReply(OperationContext* opCtx,
                                          const BulkWriteCommandRequest& req,
                                          const BSONObj& reqObj,
                                          bulk_write_exec::BulkWriteReplyInfo replyInfo) {
    auto& [replyItems, summaryFields, wcErrors, retriedStmtIds] = replyInfo;
    const NamespaceString cursorNss = NamespaceString::makeBulkWriteNSS(req.getDbName().tenantId());

    if (bulk_write_common::isUnacknowledgedBulkWrite(opCtx)) {
        // Skip cursor creation and return the simplest reply.
        return BulkWriteCommandReply(
            BulkWriteCommandResponseCursor(0 /* cursorId */, {} /* firstBatch */, cursorNss),
            summaryFields.nErrors,
            summaryFields.nInserted,
            summaryFields.nMatched,
            summaryFields.nModified,
            summaryFields.nUpserted,
            summaryFields.nDeleted);
    }

    ClusterClientCursorParams params(
        cursorNss,
        APIParameters::get(opCtx),
        ReadPreferenceSetting::get(opCtx),
        repl::ReadConcernArgs::get(opCtx),
        [&] {
            if (!opCtx->getLogicalSessionId())
                return OperationSessionInfoFromClient();
            // TODO (SERVER-80525): This code path does not
            // clear the setAutocommit field on the presence of
            // TransactionRouter::get
            return OperationSessionInfoFromClient(
                *opCtx->getLogicalSessionId(),
                // Retryable writes will have a txnNumber we do not want to associate with
                // the cursor. We only want to set this field for transactions.
                opCtx->inMultiDocumentTransaction() ? opCtx->getTxnNumber() : boost::none);
        }());

    long long batchSize = std::numeric_limits<long long>::max();
    if (req.getCursor() && req.getCursor()->getBatchSize()) {
        params.batchSize = req.getCursor()->getBatchSize();
        batchSize = *req.getCursor()->getBatchSize();
    }

    if (!reqObj.isEmpty()) {
        params.originatingCommandObj = reqObj.getOwned();
        params.originatingPrivileges = bulk_write_common::getPrivileges(req);
    }

    auto queuedDataStage = std::make_unique<RouterStageQueuedData>(opCtx);
    BulkWriteCommandReply reply;
    reply.setNErrors(summaryFields.nErrors);
    reply.setNInserted(summaryFields.nInserted);
    reply.setNDeleted(summaryFields.nDeleted);
    reply.setNMatched(summaryFields.nMatched);
    reply.setNModified(summaryFields.nModified);
    reply.setNUpserted(summaryFields.nUpserted);
    reply.setWriteConcernError(wcErrors);
    reply.setRetriedStmtIds(retriedStmtIds);

    for (auto& replyItem : replyItems) {
        queuedDataStage->queueResult(replyItem.toBSON());
    }

    auto ccc = ClusterClientCursorImpl::make(opCtx, std::move(queuedDataStage), std::move(params));

    size_t numRepliesInFirstBatch = 0;
    FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
    for (long long objCount = 0; objCount < batchSize; objCount++) {
        auto next = uassertStatusOK(ccc->next());

        if (next.isEOF()) {
            break;
        }

        auto nextObj = *next.getResult();
        if (!responseSizeTracker.haveSpaceForNext(nextObj)) {
            ccc->queueResult(nextObj);
            break;
        }

        numRepliesInFirstBatch++;
        responseSizeTracker.add(nextObj);
    }
    if (numRepliesInFirstBatch == replyItems.size()) {
        replyItems.resize(numRepliesInFirstBatch);
        reply.setCursor(BulkWriteCommandResponseCursor(
            0, std::vector<BulkWriteReplyItem>(std::move(replyItems)), cursorNss));
        return reply;
    }

    ccc->detachFromOperationContext();
    ccc->incNBatches();

    auto authUser = AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName();
    auto cursorId = uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
        opCtx,
        ccc.releaseCursor(),
        cursorNss,
        ClusterCursorManager::CursorType::QueuedData,
        ClusterCursorManager::CursorLifetime::Mortal,
        authUser));

    // Record the cursorID in CurOp.
    CurOp::get(opCtx)->debug().cursorid = cursorId;

    replyItems.resize(numRepliesInFirstBatch);
    reply.setCursor(BulkWriteCommandResponseCursor(
        cursorId, std::vector<BulkWriteReplyItem>(std::move(replyItems)), cursorNss));
    return reply;
}
}  // namespace mongo
