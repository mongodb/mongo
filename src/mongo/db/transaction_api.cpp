/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/transaction_api.h"

#include <fmt/format.h>

#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/internal_session_pool.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_time_tracker.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/s/is_mongos.h"
#include "mongo/stdx/future.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

MONGO_FAIL_POINT_DEFINE(overrideTransactionApiMaxRetriesToThree);

namespace mongo {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

namespace txn_api {

SyncTransactionWithRetries::SyncTransactionWithRetries(
    OperationContext* opCtx,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::unique_ptr<ResourceYielder> resourceYielder,
    std::unique_ptr<TransactionClient> txnClient)
    : _resourceYielder(std::move(resourceYielder)),
      _txn(std::make_shared<details::TransactionWithRetries>(
          opCtx,
          executor,
          _source.token(),
          txnClient ? std::move(txnClient)
                    : std::make_unique<details::SEPTransactionClient>(
                          opCtx,
                          executor,
                          std::make_unique<details::DefaultSEPTransactionClientBehaviors>()))) {
    // Callers should always provide a yielder when using the API with a session checked out,
    // otherwise commands run by the API won't be able to check out that session.
    invariant(!OperationContextSession::get(opCtx) || _resourceYielder);
}

StatusWith<CommitResult> SyncTransactionWithRetries::runNoThrow(OperationContext* opCtx,
                                                                Callback callback) noexcept {
    // Pre transaction processing, which must happen inline because it uses the caller's opCtx.
    auto yieldStatus = _resourceYielder ? _resourceYielder->yieldNoThrow(opCtx) : Status::OK();
    if (!yieldStatus.isOK()) {
        return yieldStatus;
    }

    auto txnFuture = _txn->run(std::move(callback));
    auto txnResult = txnFuture.getNoThrow(opCtx);

    // Cancel the source to guarantee the transaction will terminate if our opCtx was interrupted.
    _source.cancel();

    // Wait for transaction to complete before returning so variables referenced by its callback are
    // guaranteed to be in scope even if the API caller's opCtx was interrupted.
    txnFuture.wait();

    // Post transaction processing, which must also happen inline.
    OperationTimeTracker::get(opCtx)->updateOperationTime(_txn->getOperationTime());
    repl::ReplClientInfo::forClient(opCtx->getClient())
        .setLastProxyWriteTimestampForward(_txn->getOperationTime().asTimestamp());
    auto unyieldStatus = _resourceYielder ? _resourceYielder->unyieldNoThrow(opCtx) : Status::OK();

    if (!txnResult.isOK()) {
        return txnResult;
    } else if (!unyieldStatus.isOK()) {
        return unyieldStatus;
    }

    return txnResult;
}

namespace details {

std::string execContextToString(Transaction::ExecutionContext execContext) {
    switch (execContext) {
        case Transaction::ExecutionContext::kOwnSession:
            return "own session";
        case Transaction::ExecutionContext::kClientSession:
            return "client session";
        case Transaction::ExecutionContext::kClientRetryableWrite:
            return "client retryable write";
        case Transaction::ExecutionContext::kClientTransaction:
            return "client transaction";
    }
    MONGO_UNREACHABLE;
}

std::string errorHandlingStepToString(Transaction::ErrorHandlingStep nextStep) {
    switch (nextStep) {
        case Transaction::ErrorHandlingStep::kDoNotRetry:
            return "do not retry";
        case Transaction::ErrorHandlingStep::kAbortAndDoNotRetry:
            return "abort and do not retry";
        case Transaction::ErrorHandlingStep::kRetryTransaction:
            return "retry transaction";
        case Transaction::ErrorHandlingStep::kRetryCommit:
            return "retry commit";
    }
    MONGO_UNREACHABLE;
}

std::string Transaction::_transactionStateToString(TransactionState txnState) const {
    switch (txnState) {
        case TransactionState::kInit:
            return "init";
        case TransactionState::kStarted:
            return "started";
        case TransactionState::kStartedCommit:
            return "started commit";
        case TransactionState::kRetryingCommit:
            return "retrying commit";
        case TransactionState::kStartedAbort:
            return "started abort";
        case TransactionState::kDone:
            return "done";
    }
    MONGO_UNREACHABLE;
}

void logNextStep(Transaction::ErrorHandlingStep nextStep, const BSONObj& txnInfo, int attempts) {
    LOGV2(5918600,
          "Chose internal transaction error handling step",
          "nextStep"_attr = errorHandlingStepToString(nextStep),
          "txnInfo"_attr = txnInfo,
          "attempts"_attr = attempts);
}

SemiFuture<CommitResult> TransactionWithRetries::run(Callback callback) noexcept {
    _internalTxn->setCallback(std::move(callback));

    return AsyncTry([this, bodyAttempts = 0]() mutable {
               bodyAttempts++;
               return _runBodyHandleErrors(bodyAttempts).then([this] {
                   return _runCommitWithRetries();
               });
           })
        .until([](StatusOrStatusWith<CommitResult> txnStatus) {
            // Commit retries should be handled within _runCommitWithRetries().
            invariant(txnStatus != ErrorCodes::TransactionAPIMustRetryCommit);
            return txnStatus.isOK() || txnStatus != ErrorCodes::TransactionAPIMustRetryTransaction;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(_executor, _token)
        // Safe to inline because the continuation only holds state.
        .unsafeToInlineFuture()
        .tapAll([anchor = shared_from_this()](auto&&) {})
        .semi();
}

ExecutorFuture<void> TransactionWithRetries::_runBodyHandleErrors(int bodyAttempts) {
    return _internalTxn->runCallback().thenRunOn(_executor).onError(
        [this, bodyAttempts](Status bodyStatus) {
            auto nextStep = _internalTxn->handleError(bodyStatus, bodyAttempts);
            logNextStep(nextStep, _internalTxn->reportStateForLog(), bodyAttempts);

            if (nextStep == Transaction::ErrorHandlingStep::kDoNotRetry) {
                iassert(bodyStatus);
            } else if (nextStep == Transaction::ErrorHandlingStep::kAbortAndDoNotRetry) {
                return _bestEffortAbort().then([bodyStatus] { return bodyStatus; });
            } else if (nextStep == Transaction::ErrorHandlingStep::kRetryTransaction) {
                return _bestEffortAbort().then([this, bodyStatus] {
                    _internalTxn->primeForTransactionRetry();
                    iassert(Status(ErrorCodes::TransactionAPIMustRetryTransaction,
                                   str::stream() << "Must retry body loop on internal body error: "
                                                 << bodyStatus));
                });
            }
            MONGO_UNREACHABLE;
        });
}

ExecutorFuture<CommitResult> TransactionWithRetries::_runCommitHandleErrors(int commitAttempts) {
    return _internalTxn->commit().thenRunOn(_executor).onCompletion(
        [this, commitAttempts](StatusWith<CommitResult> swCommitResult) {
            if (swCommitResult.isOK() && swCommitResult.getValue().getEffectiveStatus().isOK()) {
                // Commit succeeded so return to the caller.
                return ExecutorFuture<CommitResult>(_executor, swCommitResult);
            }

            auto nextStep = _internalTxn->handleError(swCommitResult, commitAttempts);
            logNextStep(nextStep, _internalTxn->reportStateForLog(), commitAttempts);

            if (nextStep == Transaction::ErrorHandlingStep::kDoNotRetry) {
                return ExecutorFuture<CommitResult>(_executor, swCommitResult);
            } else if (nextStep == Transaction::ErrorHandlingStep::kAbortAndDoNotRetry) {
                MONGO_UNREACHABLE;
            } else if (nextStep == Transaction::ErrorHandlingStep::kRetryTransaction) {
                _internalTxn->primeForTransactionRetry();
                iassert(Status(ErrorCodes::TransactionAPIMustRetryTransaction,
                               str::stream() << "Must retry body loop on commit error: "
                                             << swCommitResult.getStatus()));
            } else if (nextStep == Transaction::ErrorHandlingStep::kRetryCommit) {
                _internalTxn->primeForCommitRetry();
                iassert(Status(ErrorCodes::TransactionAPIMustRetryCommit,
                               str::stream() << "Must retry commit loop on internal commit error: "
                                             << swCommitResult.getStatus()));
            }
            MONGO_UNREACHABLE;
        });
}

ExecutorFuture<CommitResult> TransactionWithRetries::_runCommitWithRetries() {
    return AsyncTry([this, commitAttempts = 0]() mutable {
               commitAttempts++;
               return _runCommitHandleErrors(commitAttempts);
           })
        .until([](StatusWith<CommitResult> swResult) {
            return swResult.isOK() || swResult != ErrorCodes::TransactionAPIMustRetryCommit;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(_executor, _token);
}

ExecutorFuture<void> TransactionWithRetries::_bestEffortAbort() {
    return _internalTxn->abort().thenRunOn(_executor).onError([this](Status abortStatus) {
        LOGV2(5875900,
              "Unable to abort internal transaction",
              "reason"_attr = abortStatus,
              "txnInfo"_attr = _internalTxn->reportStateForLog());
    });
}

// Sets the appropriate options on the given client and operation context for running internal
// commands.
void primeInternalClient(Client* client) {
    auto as = AuthorizationSession::get(client);
    if (as) {
        as->grantInternalAuthorization(client);
    }

    stdx::lock_guard<Client> lk(*client);
    client->setSystemOperationKillableByStepdown(lk);
}

Future<DbResponse> DefaultSEPTransactionClientBehaviors::handleRequest(
    OperationContext* opCtx, const Message& request) const {
    auto serviceEntryPoint = opCtx->getServiceContext()->getServiceEntryPoint();
    return serviceEntryPoint->handleRequest(opCtx, request);
}

SemiFuture<BSONObj> SEPTransactionClient::runCommand(StringData dbName, BSONObj cmdObj) const {
    invariant(_hooks, "Transaction metadata hooks must be injected before a command can be run");

    BSONObjBuilder cmdBuilder(_behaviors->maybeModifyCommand(std::move(cmdObj)));
    _hooks->runRequestHook(&cmdBuilder);

    invariant(!haveClient());
    auto client = _serviceContext->makeClient("SEP-internal-txn-client");
    AlternativeClientRegion clientRegion(client);
    // Note that _token is only cancelled once the caller of the transaction no longer cares about
    // its result, so CancelableOperationContexts only being interrupted by ErrorCodes::Interrupted
    // shouldn't impact any upstream retry logic.
    CancelableOperationContextFactory opCtxFactory(_token, _executor);
    auto cancellableOpCtx = opCtxFactory.makeOperationContext(&cc());
    primeInternalClient(&cc());

    auto opMsgRequest = OpMsgRequest::fromDBAndBody(dbName, cmdBuilder.obj());
    auto requestMessage = opMsgRequest.serialize();
    return _behaviors->handleRequest(cancellableOpCtx.get(), requestMessage)
        .then([this](DbResponse dbResponse) {
            auto reply = rpc::makeReply(&dbResponse.response)->getCommandReply().getOwned();
            _hooks->runReplyHook(reply);
            return reply;
        })
        .semi();
}

SemiFuture<BatchedCommandResponse> SEPTransactionClient::runCRUDOp(
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

    return runCommand(cmd.getNS().db(), cmdBob.obj())
        .thenRunOn(_executor)
        .then([](BSONObj reply) {
            uassertStatusOK(getStatusFromCommandResult(reply));

            BatchedCommandResponse response;
            std::string errmsg;
            if (!response.parseBSON(reply, &errmsg)) {
                uasserted(ErrorCodes::FailedToParse, errmsg);
            }
            return response;
        })
        .semi();
}

SemiFuture<std::vector<BSONObj>> SEPTransactionClient::exhaustiveFind(
    const FindCommandRequest& cmd) const {
    return runCommand(cmd.getDbName(), cmd.toBSON({}))
        .thenRunOn(_executor)
        .then([this, batchSize = cmd.getBatchSize()](BSONObj reply) {
            auto cursorResponse = std::make_shared<CursorResponse>(
                uassertStatusOK(CursorResponse::parseFromBSON(reply)));
            auto response = std::make_shared<std::vector<BSONObj>>();
            return AsyncTry([this,
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
                           cursorResponse->getNSS().coll().toString());
                       getMoreRequest.setBatchSize(batchSize);

                       return runCommand(cursorResponse->getNSS().db(), getMoreRequest.toBSON({}))
                           .thenRunOn(_executor)
                           .then([response, cursorResponse](BSONObj reply) {
                               // We keep the state of cursorResponse to be able to check the
                               // cursorId in the next iteration.
                               *cursorResponse =
                                   uassertStatusOK(CursorResponse::parseFromBSON(reply));
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
                .on(_executor, _token)
                .then([response = std::move(response)] { return std::move(*response); });
        })
        .semi();
}

SemiFuture<CommitResult> Transaction::commit() {
    return _commitOrAbort(NamespaceString::kAdminDb, CommitTransaction::kCommandName)
        .thenRunOn(_executor)
        .then([](BSONObj res) {
            auto wcErrorHolder = getWriteConcernErrorDetailFromBSONObj(res);
            WriteConcernErrorDetail wcError;
            if (wcErrorHolder) {
                wcErrorHolder->cloneTo(&wcError);
            }
            return CommitResult{getStatusFromCommandResult(res), wcError};
        })
        .semi();
}

SemiFuture<void> Transaction::abort() {
    return _commitOrAbort(NamespaceString::kAdminDb, AbortTransaction::kCommandName)
        .thenRunOn(_executor)
        .then([](BSONObj res) {
            uassertStatusOK(getStatusFromCommandResult(res));
            uassertStatusOK(getWriteConcernStatusFromCommandResult(res));
        })
        .semi();
}

SemiFuture<BSONObj> Transaction::_commitOrAbort(StringData dbName, StringData cmdName) {
    {
        stdx::lock_guard<Latch> lg(_mutex);

        if (_state == TransactionState::kInit) {
            LOGV2_DEBUG(
                5875903,
                3,
                "Internal transaction skipping commit or abort because no commands were run",
                "cmdName"_attr = cmdName,
                "txnInfo"_attr = _reportStateForLog(lg));
            return BSON("ok" << 1);
        }
        uassert(5875902,
                "Internal transaction not in progress, state: {}"_format(
                    _transactionStateToString(_state)),
                _state == TransactionState::kStarted ||
                    // Allows retrying commit and the best effort abort after failing to commit.
                    (_isInCommit() &&
                     (cmdName == AbortTransaction::kCommandName ||
                      cmdName == CommitTransaction::kCommandName)));

        if (cmdName == CommitTransaction::kCommandName) {
            invariant(_state != TransactionState::kStartedAbort);
            if (!_isInCommit()) {
                // Only transition if we aren't already retrying commit.
                _state = TransactionState::kStartedCommit;
            }

            if (_execContext == ExecutionContext::kClientTransaction) {
                // Don't commit if we're nested in a client's transaction.
                return SemiFuture<BSONObj>::makeReady(BSON("ok" << 1));
            }
        } else if (cmdName == AbortTransaction::kCommandName) {
            invariant(!_isInCommit());
            _state = TransactionState::kStartedAbort;
            invariant(_execContext != ExecutionContext::kClientTransaction);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(cmdName, 1);
    if (_state == TransactionState::kRetryingCommit) {
        // Per the drivers transaction spec, retrying commitTransaction uses majority write concern
        // to avoid double applying a transaction due to a transient NoSuchTransaction error
        // response.
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                          CommandHelpers::kMajorityWriteConcern.toBSON());
    } else {
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField, _writeConcern);
    }
    auto cmdObj = cmdBuilder.obj();

    return ExecutorFuture<void>(_executor)
        .then([this, dbNameCopy = dbName.toString(), cmdObj = std::move(cmdObj)] {
            return _txnClient->runCommand(dbNameCopy, cmdObj);
        })
        // Safe to inline because the continuation only holds state.
        .unsafeToInlineFuture()
        .tapAll([anchor = shared_from_this()](auto&&) {})
        .semi();
}

SemiFuture<void> Transaction::runCallback() {
    invariant(_callback);
    return ExecutorFuture<void>(_executor)
        .then([this] { return _callback(*_txnClient, _executor); })
        // Safe to inline because the continuation only holds state.
        .unsafeToInlineFuture()
        .tapAll([anchor = shared_from_this()](auto&&) {})
        .semi();
}

int getMaxRetries() {
    // Allow overriding the number of retries so unit tests can exhaust them faster.
    return MONGO_unlikely(overrideTransactionApiMaxRetriesToThree.shouldFail()) ? 3
                                                                                : kTxnRetryLimit;
}

bool isLocalTransactionFatalResult(const StatusWith<CommitResult>& swResult) {
    // If the local node is shutting down all retries would fail and if the node has failed over,
    // retries could eventually succeed on the new primary, but we want to prevent that since
    // whatever command that ran the internal transaction will fail with this error and may be
    // retried itself.
    auto isLocalFatalStatus = [](Status status) -> bool {
        return status.isA<ErrorCategory::NotPrimaryError>() ||
            status.isA<ErrorCategory::ShutdownError>();
    };

    if (!swResult.isOK()) {
        return isLocalFatalStatus(swResult.getStatus());
    }
    return isLocalFatalStatus(swResult.getValue().getEffectiveStatus());
}

// True if the transaction is running entirely against the local node, e.g. a single replica set
// transaction on a mongod. False for remote transactions from a mongod or all transactions from a
// mongos.
bool isRunningLocalTransaction(const TransactionClient& txnClient) {
    return !isMongos() && !txnClient.runsClusterOperations();
}

Transaction::ErrorHandlingStep Transaction::handleError(const StatusWith<CommitResult>& swResult,
                                                        int attemptCounter) const noexcept {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2_DEBUG(5875905,
                3,
                "Internal transaction handling error",
                "error"_attr = swResult.isOK() ? swResult.getValue().getEffectiveStatus()
                                               : swResult.getStatus(),
                "hasTransientTransactionErrorLabel"_attr =
                    _latestResponseHasTransientTransactionErrorLabel,
                "txnInfo"_attr = _reportStateForLog(lg),
                "attempts"_attr = attemptCounter);

    if (_execContext == ExecutionContext::kClientTransaction) {
        // If we're nested in another transaction, let the outer most client decide on errors.
        return ErrorHandlingStep::kDoNotRetry;
    }

    // If we're running locally, some errors mean we should not retry, like a failover or shutdown.
    if (isRunningLocalTransaction(*_txnClient) && isLocalTransactionFatalResult(swResult)) {
        return ErrorHandlingStep::kDoNotRetry;
    }

    // If the op has a deadline, retry until it is reached regardless of the number of attempts.
    if (attemptCounter > getMaxRetries() && !_opDeadline) {
        return _isInCommit() ? ErrorHandlingStep::kDoNotRetry
                             : ErrorHandlingStep::kAbortAndDoNotRetry;
    }

    // The transient transaction error label is always returned in command responses, even for
    // internal clients, so we use it to decide when to retry the transaction instead of inspecting
    // error codes. The only exception is when a network error was received before commit, handled
    // below.
    if (_latestResponseHasTransientTransactionErrorLabel) {
        return ErrorHandlingStep::kRetryTransaction;
    }

    const auto& clientStatus = swResult.getStatus();
    if (!clientStatus.isOK()) {
        if (ErrorCodes::isNetworkError(clientStatus)) {
            // A network error before commit is a transient transaction error, so we can retry the
            // entire transaction. If there is a network error after a commit is sent, we can retry
            // the commit command to either recommit if the operation failed or get the result of
            // the successful commit.
            if (_isInCommit()) {
                return ErrorHandlingStep::kRetryCommit;
            }
            return ErrorHandlingStep::kRetryTransaction;
        }
        return _isInCommit() ? ErrorHandlingStep::kDoNotRetry
                             : ErrorHandlingStep::kAbortAndDoNotRetry;
    }

    if (_isInCommit()) {
        const auto& commitStatus = swResult.getValue().cmdStatus;
        const auto& commitWCStatus = swResult.getValue().wcError.toStatus();

        // The retryable write error label is not returned to internal clients, so we cannot rely on
        // it and instead use error categories to decide when to retry commit, which is treated as a
        // retryable write, per the drivers specification.
        if (ErrorCodes::isRetriableError(commitStatus) ||
            ErrorCodes::isRetriableError(commitWCStatus)) {
            return ErrorHandlingStep::kRetryCommit;
        }

        return ErrorHandlingStep::kDoNotRetry;
    }

    return ErrorHandlingStep::kAbortAndDoNotRetry;
}

void Transaction::prepareRequest(BSONObjBuilder* cmdBuilder) {
    if (isInternalSessionForRetryableWrite(*_sessionInfo.getSessionId())) {
        // Statement ids are meaningful in a transaction spawned on behalf of a retryable write, so
        // every write in the transaction should explicitly specify an id. Either a positive number,
        // which indicates retry history should be saved for the command, or kUninitializedStmtId
        // (aka -1), which indicates retry history should not be saved. If statement ids are not
        // explicitly sent, implicit ids may be inferred, which could lead to bugs if different
        // commands have the same ids inferred.
        dassert(
            !isRetryableWriteCommand(
                cmdBuilder->asTempObj().firstElement().fieldNameStringData()) ||
                (cmdBuilder->hasField(write_ops::WriteCommandRequestBase::kStmtIdsFieldName) ||
                 cmdBuilder->hasField(write_ops::WriteCommandRequestBase::kStmtIdFieldName)),
            str::stream()
                << "In a retryable write transaction every retryable write command should have an "
                   "explicit statement id, command: "
                << redact(cmdBuilder->asTempObj()));
    }

    stdx::lock_guard<Latch> lg(_mutex);

    _sessionInfo.serialize(cmdBuilder);

    if (_state == TransactionState::kInit) {
        _state = TransactionState::kStarted;
        _sessionInfo.setStartTransaction(boost::none);
        cmdBuilder->append(repl::ReadConcernArgs::kReadConcernFieldName, _readConcern);
    }

    // Append the new recalculated maxTimeMS
    if (_opDeadline) {
        uassert(5956600,
                "Command object passed to the transaction api should not contain maxTimeMS field",
                !cmdBuilder->hasField(kMaxTimeMSField));
        auto timeLeftover =
            std::max(Milliseconds(0), *_opDeadline - _service->getFastClockSource()->now());
        cmdBuilder->append(kMaxTimeMSField, durationCount<Milliseconds>(timeLeftover));
    }

    // If the transaction API caller had API parameters, we should forward them in all requests.
    if (_apiParameters.getParamsPassed()) {
        _apiParameters.appendInfo(cmdBuilder);
    }

    _latestResponseHasTransientTransactionErrorLabel = false;
}

void Transaction::processResponse(const BSONObj& reply) {
    stdx::lock_guard<Latch> lg(_mutex);

    if (auto errorLabels = reply[kErrorLabelsFieldName]) {
        for (const auto& label : errorLabels.Array()) {
            if (label.String() == ErrorLabel::kTransientTransaction) {
                _latestResponseHasTransientTransactionErrorLabel = true;
            }
        }
    }

    if (reply.hasField(LogicalTime::kOperationTimeFieldName)) {
        _lastOperationTime = LogicalTime::fromOperationTime(reply);
    }
}

void Transaction::primeForTransactionRetry() noexcept {
    stdx::lock_guard<Latch> lg(_mutex);
    _lastOperationTime = LogicalTime();
    _latestResponseHasTransientTransactionErrorLabel = false;
    switch (_execContext) {
        case ExecutionContext::kOwnSession:
        case ExecutionContext::kClientSession:
        case ExecutionContext::kClientRetryableWrite:
            // Advance txnNumber.
            _sessionInfo.setTxnNumber(*_sessionInfo.getTxnNumber() + 1);
            _sessionInfo.setStartTransaction(true);
            _state = TransactionState::kInit;
            return;
        case ExecutionContext::kClientTransaction:
            // The outermost client handles retries, so we should never reach here.
            MONGO_UNREACHABLE;
    }
}

void Transaction::primeForCommitRetry() noexcept {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_isInCommit());
    _latestResponseHasTransientTransactionErrorLabel = false;
    _state = TransactionState::kRetryingCommit;
}

BSONObj Transaction::reportStateForLog() const {
    stdx::lock_guard<Latch> lg(_mutex);
    return _reportStateForLog(lg);
}

BSONObj Transaction::_reportStateForLog(WithLock) const {
    return BSON("execContext" << execContextToString(_execContext) << "sessionInfo"
                              << _sessionInfo.toBSON() << "state"
                              << _transactionStateToString(_state));
}

void Transaction::_setSessionInfo(WithLock,
                                  LogicalSessionId lsid,
                                  TxnNumber txnNumber,
                                  boost::optional<bool> startTransaction) {
    _sessionInfo.setSessionId(lsid);
    _sessionInfo.setTxnNumber(txnNumber);
    if (startTransaction) {
        invariant(startTransaction == boost::optional<bool>(true));
    }
    _sessionInfo.setStartTransaction(startTransaction);
}

void Transaction::_primeTransaction(OperationContext* opCtx) {
    // The API does not forward shard or database versions from the caller's opCtx, so spawned
    // commands would not obey sharding protocols, like the migration critical section, so it
    // cannot currently be used in an operation with shard versions. This does not apply in the
    // cluster commands configuration because those commands will attach appropriate shard
    // versions.
    uassert(6638800,
            "Transaction API does not currently support use within operations with shard or "
            "database versions without using router commands",
            !OperationShardingState::isComingFromRouter(opCtx) ||
                _txnClient->runsClusterOperations());

    stdx::lock_guard<Latch> lg(_mutex);

    // Extract session options and infer execution context from client's opCtx.
    auto clientSession = opCtx->getLogicalSessionId();
    auto clientTxnNumber = opCtx->getTxnNumber();
    auto clientInMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();

    if (!clientSession) {
        const auto acquiredSession =
            InternalSessionPool::get(opCtx)->acquireStandaloneSession(opCtx);
        _acquiredSessionFromPool = true;
        _setSessionInfo(lg,
                        acquiredSession.getSessionId(),
                        acquiredSession.getTxnNumber(),
                        {true} /* startTransaction */);
        _execContext = ExecutionContext::kOwnSession;
    } else if (!clientTxnNumber) {
        const auto acquiredSession =
            InternalSessionPool::get(opCtx)->acquireChildSession(opCtx, *clientSession);
        _acquiredSessionFromPool = true;
        _setSessionInfo(lg,
                        acquiredSession.getSessionId(),
                        acquiredSession.getTxnNumber(),
                        {true} /* startTransaction */);
        _execContext = ExecutionContext::kClientSession;
    } else if (!clientInMultiDocumentTransaction) {
        uassert(6648100,
                "Cross-shard internal transactions are not supported when run under a retryable "
                "write directly on a shard.",
                !_txnClient->runsClusterOperations() || isMongos());

        _setSessionInfo(lg,
                        makeLogicalSessionIdWithTxnNumberAndUUID(*clientSession, *clientTxnNumber),
                        0 /* txnNumber */,
                        {true} /* startTransaction */);
        _execContext = ExecutionContext::kClientRetryableWrite;
    } else {
        // Note that we don't want to include startTransaction or any first transaction command
        // fields because we assume that if we're in a client transaction the component tracking
        // transactions on the process must have already been started (e.g. TransactionRouter or
        // TransactionParticipant), so when the API sends commands for this transacion that
        // component will attach the correct fields if targeting new participants. This assumes this
        // case always uses a client that runs commands against the local process service entry
        // point, which we verify with this invariant.
        invariant(_txnClient->supportsClientTransactionContext());

        uassert(6648101,
                "Cross-shard internal transactions are not supported when run under a client "
                "transaction directly on a shard.",
                !_txnClient->runsClusterOperations() || isMongos());

        _setSessionInfo(lg, *clientSession, *clientTxnNumber, boost::none /* startTransaction */);
        _execContext = ExecutionContext::kClientTransaction;

        // Skip directly to the started state since we assume the client already started this
        // transaction.
        _state = TransactionState::kStarted;
    }
    _sessionInfo.setAutocommit(false);

    // Extract non-session options. Strip provenance so it can be correctly inferred for the
    // generated commands as if it came from an external client.
    _readConcern = repl::ReadConcernArgs::get(opCtx).toBSONInner().removeField(
        ReadWriteConcernProvenanceBase::kSourceFieldName);
    _writeConcern = opCtx->getWriteConcern().toBSON().removeField(
        ReadWriteConcernProvenanceBase::kSourceFieldName);
    _apiParameters = APIParameters::get(opCtx);

    if (opCtx->hasDeadline()) {
        _opDeadline = opCtx->getDeadline();
    }

    LOGV2_DEBUG(5875901,
                3,
                "Started internal transaction",
                "sessionInfo"_attr = _sessionInfo,
                "readConcern"_attr = _readConcern,
                "writeConcern"_attr = _writeConcern,
                "APIParameters"_attr = _apiParameters,
                "execContext"_attr = execContextToString(_execContext));
}

LogicalTime Transaction::getOperationTime() const {
    stdx::lock_guard<Latch> lg(_mutex);
    return _lastOperationTime;
}

Transaction::~Transaction() {
    if (_acquiredSessionFromPool) {
        InternalSessionPool::get(_service)->release(
            {*_sessionInfo.getSessionId(), *_sessionInfo.getTxnNumber()});
        _acquiredSessionFromPool = false;
    }
}

}  // namespace details
}  // namespace txn_api
}  // namespace mongo
