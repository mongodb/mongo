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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/rpc/factory.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future_util.h"


namespace mongo {
namespace txn_api {
namespace {

void runFutureInline(executor::InlineExecutor* inlineExecutor, Notification<void>& mayReturn) {
    inlineExecutor->run([&]() { return !!mayReturn; });
}

}  // namespace

SyncTransactionWithRetries::SyncTransactionWithRetries(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> sleepAndCleanupExecutor,
    std::unique_ptr<ResourceYielder> resourceYielder,
    std::shared_ptr<executor::InlineExecutor> inlineExecutor,
    std::unique_ptr<TransactionClient> txnClient)
    : _resourceYielder(std::move(resourceYielder)),
      _inlineExecutor(inlineExecutor),
      _sleepExec(inlineExecutor->getSleepableExecutor(sleepAndCleanupExecutor)),
      _cleanupExecutor(sleepAndCleanupExecutor),
      _txn(std::make_shared<details::TransactionWithRetries>(
          opCtx,
          _sleepExec,
          opCtx->getCancellationToken(),
          txnClient ? std::move(txnClient)
                    : std::make_unique<details::SEPTransactionClient>(
                          opCtx,
                          inlineExecutor,
                          _sleepExec,
                          _cleanupExecutor,
                          std::make_unique<details::DefaultSEPTransactionClientBehaviors>()))) {
    // Callers should always provide a yielder when using the API with a session checked out,
    // otherwise commands run by the API won't be able to check out that session.
    invariant(!OperationContextSession::get(opCtx) || _resourceYielder,
              str::stream() << "session is not checked out by the opCtx: "
                            << !OperationContextSession::get(opCtx)
                            << ", yielder is not provided: " << !_resourceYielder);
}

StatusWith<CommitResult> SyncTransactionWithRetries::runNoThrow(OperationContext* opCtx,
                                                                Callback callback) noexcept {
    // Pre transaction processing, which must happen inline because it uses the caller's opCtx.
    auto yieldStatus = _resourceYielder ? _resourceYielder->yieldNoThrow(opCtx) : Status::OK();
    if (!yieldStatus.isOK()) {
        return yieldStatus;
    }

    Notification<void> mayReturn;
    auto txnFuture = _txn->run(std::move(callback))
                         .unsafeToInlineFuture()
                         .tapAll([&](auto&&) { mayReturn.set(); })
                         .semi();

    runFutureInline(_inlineExecutor.get(), mayReturn);

    auto txnResult = txnFuture.getNoThrow(opCtx);

    // Post transaction processing, which must also happen inline.
    OperationTimeTracker::get(opCtx)->updateOperationTime(_txn->getOperationTime());
    repl::ReplClientInfo::forClient(opCtx->getClient())
        .setLastProxyWriteTimestampForward(_txn->getOperationTime().asTimestamp());

    boost::optional<AbortResult> cleanupAbortResult;
    if (_txn->needsCleanup()) {
        // Schedule cleanup on an out of line executor so it runs even if the transaction was
        // cancelled. Attempt to wait for cleanup so it appears synchronous for most callers, but
        // allow interruptions so we return immediately if the opCtx has been cancelled.
        //
        // Also schedule after getting the transaction's operation time so the best effort abort
        // can't unnecessarily advance it.
        auto abortResult = ExecutorFuture<void>(_cleanupExecutor)
                               .then([txn = _txn, inlineExecutor = _inlineExecutor]() mutable {
                                   Notification<void> mayReturnFromCleanup;
                                   auto cleanUpFuture =
                                       txn->cleanUp().unsafeToInlineFuture().tapAll(
                                           [&](auto&&) { mayReturnFromCleanup.set(); });
                                   runFutureInline(inlineExecutor.get(), mayReturnFromCleanup);
                                   return cleanUpFuture;
                               })
                               .getNoThrow(opCtx);
        if (abortResult.isOK()) {
            cleanupAbortResult = abortResult.getValue();
        }
    }

    auto unyieldStatus = _resourceYielder ? _resourceYielder->unyieldNoThrow(opCtx) : Status::OK();

    if (!txnResult.isOK()) {
        if (auto interruptStatus = opCtx->checkForInterruptNoAssert(); !interruptStatus.isOK()) {
            // The caller was interrupted during the transaction, so if the transaction failed,
            // return the caller's interruption code instead. The transaction uses a
            // CancelableOperationContext inherited from the caller's opCtx, but that type can only
            // kill with an Interrupted error, so this is meant as a workaround to preserve the
            // presumably more meaningful error the caller was interrupted with.
            return interruptStatus;
        }

        // TODO SERVER-99035: Use a more general TxnResult struct instead of CommitResult.
        //
        // We include the write concern error from the cleanup abort to ensure that the client
        // is informed of any replication issues with the speculative snapshot that the internal
        // transaction operated on, even if the transaction has failed and the commit
        // (which would normally provide a write concern error) did not occur.
        return StatusWith(CommitResult{txnResult.getStatus(),
                                       cleanupAbortResult ? cleanupAbortResult->wcError
                                                          : WriteConcernErrorDetail()});
    } else if (!unyieldStatus.isOK()) {
        return unyieldStatus;
    }

    return txnResult;
}

namespace details {

// Sets the appropriate options on the given client and operation context for running internal
// commands.
void primeInternalClient(Client* client) {
    auto as = AuthorizationSession::get(client);
    if (as) {
        as->grantInternalAuthorization();
    }
}

Future<DbResponse> DefaultSEPTransactionClientBehaviors::handleRequest(OperationContext* opCtx,
                                                                       const Message& request,
                                                                       Date_t started) const {
    auto serviceEntryPoint = opCtx->getService()->getServiceEntryPoint();
    return serviceEntryPoint->handleRequest(opCtx, request, started);
}

ExecutorFuture<BSONObj> SEPTransactionClient::_runCommand(const DatabaseName& dbName,
                                                          BSONObj cmdObj) const {
    invariant(_hooks, "Transaction metadata hooks must be injected before a command can be run");

    Date_t started = _serviceContext->getFastClockSource()->now();

    BSONObjBuilder cmdBuilder(_behaviors->maybeModifyCommand(std::move(cmdObj)));
    _hooks->runRequestHook(&cmdBuilder);

    auto client = _serviceContext->getService()->makeClient("SEP-internal-txn-client");

    AlternativeClientRegion clientRegion(client);

    // Note that _token is only cancelled once the caller of the transaction no longer cares about
    // its result, so CancelableOperationContexts only being interrupted by ErrorCodes::Interrupted
    // shouldn't impact any upstream retry logic.
    auto opCtxFactory =
        CancelableOperationContextFactory(_hooks->getTokenForCommand(), _cancelExecutor);

    auto cancellableOpCtx = opCtxFactory.makeOperationContext(&cc());

    primeInternalClient(&cc());

    auto vts = [&]() {
        auto tenantId = dbName.tenantId();
        return tenantId
            ? auth::ValidatedTenancyScopeFactory::create(
                  *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{})
            : auth::ValidatedTenancyScope::kNotRequired;
    }();
    auto opMsgRequest = OpMsgRequestBuilder::create(vts, dbName, cmdBuilder.obj());
    auto requestMessage = opMsgRequest.serialize();
    return _behaviors->handleRequest(cancellableOpCtx.get(), requestMessage, started)
        .thenRunOn(_executor)
        .then([this](DbResponse dbResponse) {
            // NOTE: The API uses this method to run commit and abort, so be careful about adding
            // new logic here to ensure it cannot interfere with error handling for either command.
            auto reply = rpc::makeReply(&dbResponse.response)->getCommandReply().getOwned();
            _hooks->runReplyHook(reply);
            return reply;
        });
}

BSONObj SEPTransactionClient::runCommandSync(const DatabaseName& dbName, BSONObj cmdObj) const {
    Notification<void> mayReturn;

    auto result =
        _runCommand(dbName, cmdObj).unsafeToInlineFuture().tapAll([&](auto&&) { mayReturn.set(); });

    runFutureInline(_inlineExecutor.get(), mayReturn);

    return std::move(result).get();
}

SemiFuture<BSONObj> SEPTransactionClient::runCommand(const DatabaseName& dbName,
                                                     BSONObj cmdObj) const {
    return _runCommand(dbName, cmdObj).semi();
}

ExecutorFuture<BSONObj> SEPTransactionClient::_runCommandChecked(const DatabaseName& dbName,
                                                                 BSONObj cmdObj) const {
    return _runCommand(dbName, cmdObj).then([](BSONObj reply) {
        uassertStatusOK(getStatusFromCommandResult(reply));
        return reply;
    });
}

SemiFuture<BSONObj> SEPTransactionClient::runCommandChecked(const DatabaseName& dbName,
                                                            BSONObj cmdObj) const {
    return _runCommandChecked(dbName, cmdObj).semi();
}

BSONObj SEPTransactionClient::runCommandCheckedSync(const DatabaseName& dbName,
                                                    BSONObj cmdObj) const {
    Notification<void> mayReturn;
    auto result = _runCommandChecked(dbName, cmdObj).unsafeToInlineFuture().tapAll([&](auto&&) {
        mayReturn.set();
    });
    runFutureInline(_inlineExecutor.get(), mayReturn);

    return std::move(result).get();
}

ExecutorFuture<BatchedCommandResponse> SEPTransactionClient::_runCRUDOp(
    const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const {
    invariant(!stmtIds.size() || (cmd.sizeWriteOps() == stmtIds.size()),
              fmt::format("If stmtIds are specified, they must match the number of write ops. "
                          "Found {} stmtId(s) and {} write op(s).",
                          stmtIds.size(),
                          cmd.sizeWriteOps()));

    BSONObjBuilder cmdBob(cmd.toBSON());
    if (stmtIds.size()) {
        cmdBob.append(write_ops::WriteCommandRequestBase::kStmtIdsFieldName, stmtIds);
    }

    return runCommand(cmd.getNS().dbName(), cmdBob.obj())
        .thenRunOn(_executor)
        .then([](BSONObj reply) {
            uassertStatusOK(getStatusFromWriteCommandReply(reply));

            BatchedCommandResponse response;
            std::string errmsg;
            if (!response.parseBSON(reply, &errmsg)) {
                uasserted(ErrorCodes::FailedToParse, errmsg);
            }
            return response;
        });
}

SemiFuture<BatchedCommandResponse> SEPTransactionClient::runCRUDOp(
    const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const {
    return _runCRUDOp(cmd, stmtIds).semi();
}

BatchedCommandResponse SEPTransactionClient::runCRUDOpSync(const BatchedCommandRequest& cmd,
                                                           std::vector<StmtId> stmtIds) const {
    Notification<void> mayReturn;

    auto result =
        _runCRUDOp(cmd, stmtIds)
            .unsafeToInlineFuture()
            // Use tap and tapError instead of tapAll since tapAll is not move-only type friendly
            .tap([&](auto&&) { mayReturn.set(); })
            .tapError([&](auto&&) { mayReturn.set(); });

    runFutureInline(_inlineExecutor.get(), mayReturn);

    return std::move(result).get();
}

ExecutorFuture<BulkWriteCommandReply> SEPTransactionClient::_runCRUDOp(
    const BulkWriteCommandRequest& cmd) const {
    BSONObjBuilder cmdBob(cmd.toBSON());
    // BulkWrite can only execute on admin DB.
    return runCommand(DatabaseName::kAdmin, cmdBob.obj())
        .thenRunOn(_executor)
        .then([](BSONObj reply) {
            uassertStatusOK(getStatusFromCommandResult(reply));

            IDLParserContext ctx("BulkWriteCommandReply");
            auto response = BulkWriteCommandReply::parse(reply, ctx);

            // TODO (SERVER-80794): Support iterating through the cursor for internal transactions.
            uassert(7934200,
                    "bulkWrite requires multiple batches to fetch all responses but it is "
                    "currently not supported in internal transactions",
                    response.getCursor().getId() == 0);
            for (auto&& replyItem : response.getCursor().getFirstBatch()) {
                uassertStatusOK(replyItem.getStatus());
            }

            uassertStatusOK(getWriteConcernStatusFromCommandResult(reply));

            return response;
        });
}

SemiFuture<BulkWriteCommandReply> SEPTransactionClient::runCRUDOp(
    const BulkWriteCommandRequest& cmd) const {
    return _runCRUDOp(cmd).semi();
}

BulkWriteCommandReply SEPTransactionClient::runCRUDOpSync(
    const BulkWriteCommandRequest& cmd) const {

    Notification<void> mayReturn;

    auto result =
        _runCRUDOp(cmd)
            .unsafeToInlineFuture()
            // Use tap and tapError instead of tapAll since tapAll is not move-only type friendly
            .tap([&](auto&&) { mayReturn.set(); })
            .tapError([&](auto&&) { mayReturn.set(); });

    runFutureInline(_inlineExecutor.get(), mayReturn);

    return std::move(result).get();
}

ExecutorFuture<std::vector<BSONObj>> SEPTransactionClient::_exhaustiveFind(
    const FindCommandRequest& cmd) const {
    return runCommand(cmd.getDbName(), cmd.toBSON())
        .thenRunOn(_executor)
        .then([this, batchSize = cmd.getBatchSize(), tenantId = cmd.getDbName().tenantId()](
                  BSONObj reply) {
            auto cursorResponse = std::make_shared<CursorResponse>(
                uassertStatusOK(CursorResponse::parseFromBSON(reply, nullptr, tenantId)));
            auto response = std::make_shared<std::vector<BSONObj>>();
            return AsyncTry([this,
                             tenantId,
                             batchSize = batchSize,
                             cursorResponse = std::move(cursorResponse),
                             response]() mutable {
                       auto releasedBatch = cursorResponse->releaseBatch();
                       response->insert(
                           response->end(), releasedBatch.begin(), releasedBatch.end());

                       // If we've fetched all the documents, we can return the response vector
                       // wrapped in an OK status.
                       if (!cursorResponse->getCursorId()) {
                           return SemiFuture<void>(Status::OK());
                       }

                       GetMoreCommandRequest getMoreRequest(
                           cursorResponse->getCursorId(),
                           std::string{cursorResponse->getNSS().coll()});
                       getMoreRequest.setBatchSize(batchSize);

                       return runCommand(cursorResponse->getNSS().dbName(), getMoreRequest.toBSON())
                           .thenRunOn(_executor)
                           .then([response, cursorResponse, tenantId](BSONObj reply) {
                               // We keep the state of cursorResponse to be able to check the
                               // cursorId in the next iteration.
                               *cursorResponse = uassertStatusOK(
                                   CursorResponse::parseFromBSON(reply, nullptr, tenantId));
                               uasserted(ErrorCodes::InternalTransactionsExhaustiveFindHasMore,
                                         "More documents to fetch");
                           })
                           .semi();
                   })
                .until([&](Status result) {
                    // We stop execution if there is either no more documents to fetch or there was
                    // an error upon fetching more documents.
                    return result != ErrorCodes::InternalTransactionsExhaustiveFindHasMore;
                })
                // It's fine to use an uncancelable token here because the getMore commands in the
                // AsyncTry will inherit the real token.
                .on(_executor, CancellationToken::uncancelable())
                .then([response = std::move(response)] { return std::move(*response); });
        });
}

SemiFuture<std::vector<BSONObj>> SEPTransactionClient::exhaustiveFind(
    const FindCommandRequest& cmd) const {
    return _exhaustiveFind(cmd).semi();
}

std::vector<BSONObj> SEPTransactionClient::exhaustiveFindSync(const FindCommandRequest& cmd) const {
    Notification<void> mayReturn;

    auto result =
        _exhaustiveFind(cmd).unsafeToInlineFuture().tapAll([&](auto&&) { mayReturn.set(); });

    runFutureInline(_inlineExecutor.get(), mayReturn);

    return std::move(result).get();
}

}  // namespace details
}  // namespace txn_api
}  // namespace mongo
