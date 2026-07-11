// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/multi_statement_transaction_requests_sender.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/resource_yielders.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/assert_util.h"

#include <utility>

namespace mongo {

namespace transaction_request_sender_details {
namespace {

void processReplyMetadata(
    OperationContext* opCtx,
    const ShardId& shardId,
    const TransactionRouter::ParsedParticipantResponseMetadata& parsedResponse,
    bool forAsyncGetMore = false) {
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter) {
        return;
    }

    txnRouter.processParticipantResponse(opCtx, shardId, parsedResponse, forAsyncGetMore);
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
            request.shardRef,
            txnRouter.attachTxnFieldsIfNeeded(opCtx, request.shardRef, request.cmdObj));
    }

    return newRequests;
}

void processReplyMetadata(OperationContext* opCtx,
                          const AsyncRequestsSender::Response& response,
                          bool forAsyncGetMore) {
    if (!response.swResponse.isOK()) {
        return;
    }

    processReplyMetadata(opCtx,
                         response.shardId,
                         TransactionRouter::Router::parseParticipantResponseMetadata(
                             response.swResponse.getValue().data),
                         forAsyncGetMore);
}

void processReplyMetadataForAsyncGetMore(
    OperationContext* opCtx,
    const ShardId& shardId,
    const TransactionRouter::ParsedParticipantResponseMetadata& parsedResponse) {
    processReplyMetadata(opCtx, shardId, parsedResponse, true /* forAsyncGetMore */);
}

}  // namespace transaction_request_sender_details

MultiStatementTransactionRequestsSender::MultiStatementTransactionRequestsSender(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    const DatabaseName& dbName,
    const std::vector<AsyncRequestsSender::Request>& requests,
    const ReadPreferenceSetting& readPreference,
    Shard::RetryPolicy retryPolicy,
    const AsyncRequestsSender::ShardHostMap& designatedHostsMap)
    : _opCtx(opCtx),
      _ars(std::make_unique<AsyncRequestsSender>(
          opCtx,
          std::move(executor),
          dbName,
          transaction_request_sender_details::attachTxnDetails(opCtx, requests),
          readPreference,
          retryPolicy,
          ResourceYielderFactory::get(*opCtx->getService())
              ? ResourceYielderFactory::get(*opCtx->getService())->make(opCtx, "request-sender")
              : nullptr,
          designatedHostsMap)) {}

MultiStatementTransactionRequestsSender::~MultiStatementTransactionRequestsSender() {
    invariant(_opCtx);
    auto baton = _opCtx->getBaton();
    invariant(baton);

    // Cancel any scheduled retry requests here, in case we missed cancelling them in the
    // AsyncRequestsSender before. The retries will be canceled in the AsyncRequestsSender as well
    // as a last resort, but in case we got here without a previous cancellation, we can avoid some
    // unnecessary work by cancelling the callbacks here.
    _ars->stopRetrying();

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
