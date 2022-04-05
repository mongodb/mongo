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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

#include "mongo/db/transaction_api.h"

#include <fmt/format.h>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
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
#include "mongo/db/session_catalog.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/future.h"
#include "mongo/transport/service_entry_point.h"

namespace mongo::txn_api {

namespace {

/**
 * Accepts a callback that returns a future. Yields the ResourceYielder before constructing and
 * waiting on the future and then unyields before returning the ready future's result. Unyield will
 * always be called if yield is successful. Errors from the callback are returned if it and unyield
 * return errors.
 *
 * Notably, the futures are constructed after yielding so a future made with an inline executor
 * will still run after the yield.
 */
template <typename Callback>
StatusOrStatusWith<typename std::invoke_result_t<Callback>::value_type> getWithYields(
    OperationContext* opCtx,
    Callback&& cb,
    const std::unique_ptr<ResourceYielder>& resourceYielder) {
    auto yieldStatus = resourceYielder ? resourceYielder->yieldNoThrow(opCtx) : Status::OK();
    if (!yieldStatus.isOK()) {
        return yieldStatus;
    }

    auto fut = std::forward<Callback>(cb)();
    auto futureStatus = fut.getNoThrow(opCtx);

    auto unyieldStatus = resourceYielder ? resourceYielder->unyieldNoThrow(opCtx) : Status::OK();

    if (!futureStatus.isOK()) {
        return futureStatus;
    } else if (!unyieldStatus.isOK()) {
        return unyieldStatus;
    }

    return futureStatus;
}

}  // namespace

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

std::string transactionStateToString(Transaction::TransactionState txnState) {
    switch (txnState) {
        case Transaction::TransactionState::kInit:
            return "init";
        case Transaction::TransactionState::kStarted:
            return "started";
        case Transaction::TransactionState::kStartedCommit:
            return "started commit";
        case Transaction::TransactionState::kStartedAbort:
            return "started abort";
        case Transaction::TransactionState::kDone:
            return "done";
    }
    MONGO_UNREACHABLE;
}

void logNextStep(Transaction::ErrorHandlingStep nextStep, const BSONObj& txnInfo) {
    LOGV2(5918600,
          "Chose internal transaction error handling step",
          "nextStep"_attr = errorHandlingStepToString(nextStep),
          "txnInfo"_attr = txnInfo);
}

}  // namespace details

TransactionWithRetries::TransactionWithRetries(OperationContext* opCtx,
                                               ExecutorPtr executor,
                                               std::unique_ptr<ResourceYielder> resourceYielder)
    : _internalTxn(std::make_shared<details::Transaction>(opCtx, executor)),
      _resourceYielder(std::move(resourceYielder)) {
    // Callers should always provide a yielder when using the API with a session checked out,
    // otherwise commands run by the API won't be able to check out that session.
    invariant(!OperationContextSession::get(opCtx) || _resourceYielder);
}

StatusWith<CommitResult> TransactionWithRetries::runSyncNoThrow(OperationContext* opCtx,
                                                                Callback callback) noexcept {
    ON_BLOCK_EXIT([opCtx, this] {
        OperationTimeTracker::get(opCtx)->updateOperationTime(_internalTxn->getOperationTime());
    });

    // TODO SERVER-59566 Add a retry policy.
    _internalTxn->setCallback(std::move(callback));
    while (true) {
        {
            auto bodyStatus =
                getWithYields(opCtx, [&] { return _internalTxn->runCallback(); }, _resourceYielder);

            if (!bodyStatus.isOK()) {
                auto nextStep = _internalTxn->handleError(bodyStatus);
                logNextStep(nextStep, _internalTxn->reportStateForLog());

                if (nextStep == details::Transaction::ErrorHandlingStep::kDoNotRetry) {
                    return bodyStatus;
                } else if (nextStep ==
                           details::Transaction::ErrorHandlingStep::kAbortAndDoNotRetry) {
                    _bestEffortAbort(opCtx);
                    return bodyStatus;
                } else if (nextStep == details::Transaction::ErrorHandlingStep::kRetryTransaction) {
                    _bestEffortAbort(opCtx);
                    _internalTxn->primeForTransactionRetry();
                    continue;
                } else {
                    MONGO_UNREACHABLE;
                }
            }
        }

        while (true) {
            auto swResult =
                getWithYields(opCtx, [&] { return _internalTxn->commit(); }, _resourceYielder);

            if (swResult.isOK() && swResult.getValue().getEffectiveStatus().isOK()) {
                // Commit succeeded so return to the caller.
                return swResult;
            }

            auto nextStep = _internalTxn->handleError(swResult);
            logNextStep(nextStep, _internalTxn->reportStateForLog());

            if (nextStep == details::Transaction::ErrorHandlingStep::kDoNotRetry) {
                return swResult;
            } else if (nextStep == details::Transaction::ErrorHandlingStep::kAbortAndDoNotRetry) {
                _bestEffortAbort(opCtx);
                return swResult;
            } else if (nextStep == details::Transaction::ErrorHandlingStep::kRetryTransaction) {
                _bestEffortAbort(opCtx);
                _internalTxn->primeForTransactionRetry();
                break;
            } else if (nextStep == details::Transaction::ErrorHandlingStep::kRetryCommit) {
                _internalTxn->primeForCommitRetry();
                continue;
            } else {
                MONGO_UNREACHABLE;
            }
        }
    }
    MONGO_UNREACHABLE;
}

void TransactionWithRetries::_bestEffortAbort(OperationContext* opCtx) {
    try {
        uassertStatusOK(
            getWithYields(opCtx, [&] { return _internalTxn->abort(); }, _resourceYielder));
    } catch (const DBException& e) {
        LOGV2(5875900,
              "Unable to abort internal transaction",
              "reason"_attr = e.toStatus(),
              "txnInfo"_attr = _internalTxn->reportStateForLog());
    }
}

namespace details {

// Sets the appropriate options on the given client and operation context for running internal
// commands.
void primeInternalClientAndOpCtx(Client* client, OperationContext* opCtx) {
    auto as = AuthorizationSession::get(client);
    if (as) {
        as->grantInternalAuthorization(client);
    }
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
    auto cancellableOpCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    primeInternalClientAndOpCtx(&cc(), cancellableOpCtx.get());

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
    // TODO SERVER-64793: Make exhaustiveFind asynchronous
    return runCommand(cmd.getDbName(), cmd.toBSON({}))
        .thenRunOn(_executor)
        .then([this, batchSize = cmd.getBatchSize()](BSONObj reply) {
            std::vector<BSONObj> response;
            auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(reply));
            while (true) {
                auto releasedBatch = cursorResponse.releaseBatch();
                response.insert(response.end(), releasedBatch.begin(), releasedBatch.end());

                // We keep issuing getMores until the cursorId signifies that there are no more
                // documents to fetch.
                if (!cursorResponse.getCursorId()) {
                    break;
                }

                GetMoreCommandRequest getMoreRequest(cursorResponse.getCursorId(),
                                                     cursorResponse.getNSS().coll().toString());
                getMoreRequest.setBatchSize(batchSize);

                // We block until we get the response back from runCommand().
                cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(
                    runCommand(cursorResponse.getNSS().db(), getMoreRequest.toBSON({})).get()));
            }
            return response;
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
                "Internal transaction not in progress",
                _state == TransactionState::kStarted ||
                    // Allows the best effort abort to run.
                    (_state == TransactionState::kStartedCommit &&
                     cmdName == AbortTransaction::kCommandName));

        if (cmdName == CommitTransaction::kCommandName) {
            _state = TransactionState::kStartedCommit;
            if (_execContext == ExecutionContext::kClientTransaction) {
                // Don't commit if we're nested in a client's transaction.
                return SemiFuture<BSONObj>::makeReady(BSON("ok" << 1));
            }
        } else if (cmdName == AbortTransaction::kCommandName) {
            _state = TransactionState::kStartedAbort;
            invariant(_execContext != ExecutionContext::kClientTransaction);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(cmdName, 1);
    cmdBuilder.append(WriteConcernOptions::kWriteConcernField, _writeConcern);
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

Transaction::ErrorHandlingStep Transaction::handleError(
    const StatusWith<CommitResult>& swResult) const {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2_DEBUG(5875905,
                3,
                "Internal transaction handling error",
                "error"_attr = swResult.isOK() ? swResult.getValue().getEffectiveStatus()
                                               : swResult.getStatus(),
                "hasTransientTransactionErrorLabel"_attr =
                    _latestResponseHasTransientTransactionErrorLabel,
                "txnInfo"_attr = _reportStateForLog(lg));

    if (_execContext == ExecutionContext::kClientTransaction) {
        // If we're nested in another transaction, let the outer most client decide on errors.
        return ErrorHandlingStep::kDoNotRetry;
    }

    // The transient transaction error label is always returned in command responses, even for
    // internal clients, so we use it to decide when to retry the transaction instead of inspecting
    // error codes. The only exception is when a network error was received before commit, handled
    // below.
    if (_latestResponseHasTransientTransactionErrorLabel) {
        return ErrorHandlingStep::kRetryTransaction;
    }

    auto hasStartedCommit = _state == TransactionState::kStartedCommit;

    const auto& clientStatus = swResult.getStatus();
    if (!clientStatus.isOK()) {
        if (ErrorCodes::isNetworkError(clientStatus)) {
            // A network error before commit is a transient transaction error, so we can retry the
            // entire transaction. If there is a network error after a commit is sent, we can retry
            // the commit command to either recommit if the operation failed or get the result of
            // the successful commit.
            if (hasStartedCommit) {
                return ErrorHandlingStep::kRetryCommit;
            }
            return ErrorHandlingStep::kRetryTransaction;
        }
        return ErrorHandlingStep::kAbortAndDoNotRetry;
    }

    if (hasStartedCommit) {
        const auto& commitStatus = swResult.getValue().cmdStatus;
        const auto& commitWCStatus = swResult.getValue().wcError.toStatus();

        // The retryable write error label is not returned to internal clients, so we cannot rely on
        // it and instead use error categories to decide when to retry commit, which is treated as a
        // retryable write, per the drivers specification.
        if (ErrorCodes::isRetriableError(commitStatus) ||
            ErrorCodes::isRetriableError(commitWCStatus)) {
            // TODO SERVER-59566: Handle timeouts and max retry attempts. Note commit might be
            // retried within the command itself, e.g. ClusterCommitTransaction uses an idempotent
            // retry policy, so we may want a timeout policy instead of number of retries.
            return ErrorHandlingStep::kRetryCommit;
        }
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

void Transaction::primeForTransactionRetry() {
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

void Transaction::primeForCommitRetry() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_state == TransactionState::kStartedCommit);
    _latestResponseHasTransientTransactionErrorLabel = false;
    _state = TransactionState::kStarted;
}

BSONObj Transaction::reportStateForLog() const {
    stdx::lock_guard<Latch> lg(_mutex);
    return _reportStateForLog(lg);
}

BSONObj Transaction::_reportStateForLog(WithLock) const {
    return BSON("execContext" << execContextToString(_execContext) << "sessionInfo"
                              << _sessionInfo.toBSON() << "state"
                              << transactionStateToString(_state));
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

    LOGV2_DEBUG(5875901,
                3,
                "Started internal transaction",
                "sessionInfo"_attr = _sessionInfo,
                "readConcern"_attr = _readConcern,
                "writeConcern"_attr = _writeConcern,
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
}  // namespace mongo::txn_api
