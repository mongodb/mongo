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

#include "mongo/platform/basic.h"

#include "mongo/s/multi_statement_transaction_requests_sender.h"

#include "mongo/s/transaction_router.h"

namespace mongo {

namespace {

std::vector<AsyncRequestsSender::Request> attachTxnDetails(
    OperationContext* opCtx, const std::vector<AsyncRequestsSender::Request>& requests) {
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter) {
        return requests;
    }

    std::vector<AsyncRequestsSender::Request> newRequests;
    newRequests.reserve(requests.size());

    for (auto request : requests) {
        newRequests.emplace_back(
            request.shardId,
            txnRouter.attachTxnFieldsIfNeeded(opCtx, request.shardId, request.cmdObj));
    }

    return newRequests;
}

void processReplyMetadata(OperationContext* opCtx, const AsyncRequestsSender::Response& response) {
    auto txnRouter = TransactionRouter::get(opCtx);
    if (!txnRouter) {
        return;
    }

    if (!response.swResponse.isOK()) {
        return;
    }

    txnRouter.processParticipantResponse(
        opCtx, response.shardId, response.swResponse.getValue().data);
}

}  // unnamed namespace

MultiStatementTransactionRequestsSender::MultiStatementTransactionRequestsSender(
    OperationContext* opCtx,
    executor::TaskExecutor* executor,
    StringData dbName,
    const std::vector<AsyncRequestsSender::Request>& requests,
    const ReadPreferenceSetting& readPreference,
    Shard::RetryPolicy retryPolicy)
    : _opCtx(opCtx),
      _ars(
          opCtx, executor, dbName, attachTxnDetails(opCtx, requests), readPreference, retryPolicy) {
}

bool MultiStatementTransactionRequestsSender::done() {
    return _ars.done();
}

AsyncRequestsSender::Response MultiStatementTransactionRequestsSender::next() {
    const auto response = _ars.next();
    processReplyMetadata(_opCtx, response);
    return response;
}

void MultiStatementTransactionRequestsSender::stopRetrying() {
    _ars.stopRetrying();
}

}  // namespace mongo
