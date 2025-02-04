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

#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/resource_yielders.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/transaction_router_resource_yielder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"

namespace mongo {

namespace transaction_request_sender_details {
namespace {
void processReplyMetadata(OperationContext* opCtx,
                          const ShardId& shardId,
                          const BSONObj& responseBson,
                          bool forAsyncGetMore = false) {
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter) {
        return;
    }

    txnRouter.processParticipantResponse(opCtx, shardId, responseBson, forAsyncGetMore);
}
}  // namespace

std::vector<AsyncRequestsSender::Request> attachTxnDetails(
    OperationContext* opCtx, const std::vector<AsyncRequestsSender::Request>& requests) {
    bool activeTxnParticipantAddParticipants =
        opCtx->isActiveTransactionParticipant() && opCtx->inMultiDocumentTransaction();

    auto txnRouter = TransactionRouter::get(opCtx);

    if (!txnRouter) {
        return requests;
    }

    if (activeTxnParticipantAddParticipants) {
        auto opCtxTxnNum = opCtx->getTxnNumber();
        invariant(opCtxTxnNum);
        txnRouter.beginOrContinueTxn(
            opCtx, *opCtxTxnNum, TransactionRouter::TransactionActions::kStartOrContinue);
    }

    std::vector<AsyncRequestsSender::Request> newRequests;
    newRequests.reserve(requests.size());

    for (const auto& request : requests) {
        newRequests.emplace_back(
            request.shardId,
            txnRouter.attachTxnFieldsIfNeeded(opCtx, request.shardId, request.cmdObj));
    }

    return newRequests;
}

void processReplyMetadata(OperationContext* opCtx,
                          const AsyncRequestsSender::Response& response,
                          bool forAsyncGetMore) {
    if (!response.swResponse.isOK()) {
        return;
    }

    processReplyMetadata(
        opCtx, response.shardId, response.swResponse.getValue().data, forAsyncGetMore);
}

void processReplyMetadataForAsyncGetMore(OperationContext* opCtx,
                                         const ShardId& shardId,
                                         const BSONObj& responseBson) {
    processReplyMetadata(opCtx, shardId, responseBson, true /* forAsyncGetMore */);
}

}  // namespace transaction_request_sender_details

MultiStatementTransactionRequestsSender::MultiStatementTransactionRequestsSender(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const DatabaseName& dbName,
    const std::vector<AsyncRequestsSender::Request>& requests,
    const ReadPreferenceSetting& readPreference,
    Shard::RetryPolicy retryPolicy,
    AsyncRequestsSender::ShardHostMap designatedHostsMap)
    : _opCtx(opCtx),
      _ars(std::make_unique<AsyncRequestsSender>(
          opCtx,
          std::move(executor),
          dbName,
          transaction_request_sender_details::attachTxnDetails(opCtx, requests),
          readPreference,
          retryPolicy,
          ResourceYielderFactory::get(*opCtx->getService()).make(opCtx, "request-sender"),
          designatedHostsMap)) {}

MultiStatementTransactionRequestsSender::~MultiStatementTransactionRequestsSender() {
    invariant(_opCtx);
    auto baton = _opCtx->getBaton();
    invariant(baton);
    // Delegate the destruction of `_ars` to the `_opCtx` baton to potentially move the cost off of
    // the critical path. The assumption is that postponing the destruction is safe so long as the
    // `_opCtx` that corresponds to `_ars` remains alive.
    baton->schedule([ars = std::move(_ars)](Status) mutable { ars.reset(); });
}

bool MultiStatementTransactionRequestsSender::done() const {
    return _ars->done();
}

AsyncRequestsSender::Response MultiStatementTransactionRequestsSender::next(bool forMergeCursors) {
    auto response = nextResponse();
    validateResponse(response, forMergeCursors);
    return response;
}

AsyncRequestsSender::Response MultiStatementTransactionRequestsSender::nextResponse() {
    return _ars->next();
}

void MultiStatementTransactionRequestsSender::validateResponse(
    const AsyncRequestsSender::Response& response, bool forMergeCursors) const {
    transaction_request_sender_details::processReplyMetadata(_opCtx, response, forMergeCursors);
}

void MultiStatementTransactionRequestsSender::stopRetrying() {
    _ars->stopRetrying();
}

}  // namespace mongo
