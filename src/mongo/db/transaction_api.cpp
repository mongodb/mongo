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

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_api.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/future.h"
#include "mongo/transport/service_entry_point.h"

namespace mongo::txn_api {

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

StatusWith<CommitResult> TransactionWithRetries::runSyncNoThrow(OperationContext* opCtx,
                                                                TxnCallback func) noexcept {
    // TODO SERVER-59566 Add a retry policy.
    while (true) {
        {
            auto bodyStatus = ExecutorFuture<void>(_executor)
                                  .then([this, anchor = shared_from_this(), &func] {
                                      return func(_internalTxn->getClient(), _executor);
                                  })
                                  .getNoThrow(opCtx);

            if (!bodyStatus.isOK()) {
                auto nextStep = _internalTxn->handleError(bodyStatus);
                logNextStep(nextStep, _internalTxn->reportStateForLog());

                if (nextStep == details::Transaction::ErrorHandlingStep::kDoNotRetry) {
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
                ExecutorFuture<void>(_executor)
                    .then([this, anchor = shared_from_this()] { return _internalTxn->commit(); })
                    .getNoThrow(opCtx);

            if (swResult.isOK() && swResult.getValue().getEffectiveStatus().isOK()) {
                // Commit succeeded so return to the caller.
                return swResult;
            }

            auto nextStep = _internalTxn->handleError(swResult);
            logNextStep(nextStep, _internalTxn->reportStateForLog());

            if (nextStep == details::Transaction::ErrorHandlingStep::kDoNotRetry) {
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
        ExecutorFuture<void>(_executor)
            .then([this, anchor = shared_from_this()] { return _internalTxn->abort(); })
            .get(opCtx);
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

SemiFuture<BSONObj> SEPTransactionClient::runCommand(StringData dbName, BSONObj cmdObj) const {
    BSONObjBuilder cmdBuilder(std::move(cmdObj));

    invariant(_hooks, "Transaction metadata hooks must be injected before a command can be run");
    _hooks->runRequestHook(&cmdBuilder);

    invariant(!haveClient());
    auto client = getGlobalServiceContext()->makeClient("SEP-internal-txn-client");
    AlternativeClientRegion clientRegion(client);
    auto cancellableOpCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    primeInternalClientAndOpCtx(&cc(), cancellableOpCtx.get());

    auto sep = cc().getServiceContext()->getServiceEntryPoint();
    auto opMsgRequest = OpMsgRequest::fromDBAndBody(dbName, cmdBuilder.obj());
    auto requestMessage = opMsgRequest.serialize();
    return sep->handleRequest(cancellableOpCtx.get(), requestMessage)
        .then([this](DbResponse dbResponse) {
            auto reply = rpc::makeReply(&dbResponse.response)->getCommandReply().getOwned();
            _hooks->runReplyHook(reply);
            return reply;
        })
        .semi();
}

SemiFuture<BatchedCommandResponse> SEPTransactionClient::runCRUDOp(
    const BatchedCommandRequest& cmd, std::vector<StmtId> stmtIds) const {
    return runCommand(cmd.getNS().db(), cmd.toBSON())
        .thenRunOn(_executor)
        .then([this](BSONObj reply) {
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
    // TODO SERVER-61779 Support cursors.
    uassert(ErrorCodes::IllegalOperation,
            "Only single batch finds are supported",
            cmd.getSingleBatch());
    return runCommand(cmd.getDbName(), cmd.toBSON({}))
        .thenRunOn(_executor)
        .then([this](BSONObj reply) {
            // Will throw if the response has a non OK top level status.
            auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(reply));
            return cursorResponse.releaseBatch();
        })
        .semi();
}

SemiFuture<CommitResult> Transaction::commit() {
    return _commitOrAbort(NamespaceString::kAdminDb, CommitTransaction::kCommandName)
        .thenRunOn(_executor)
        .then([this](BSONObj res) {
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
        .then([this](BSONObj res) {
            uassertStatusOK(getStatusFromCommandResult(res));
            uassertStatusOK(getWriteConcernStatusFromCommandResult(res));
        })
        .semi();
}

SemiFuture<BSONObj> Transaction::_commitOrAbort(StringData dbName, StringData cmdName) {
    if (_state == TransactionState::kInit) {
        LOGV2_DEBUG(5875903,
                    0,  // TODO SERVER-61781: Raise verbosity.
                    "Internal transaction skipping commit or abort because no commands were run",
                    "cmdName"_attr = cmdName,
                    "txnInfo"_attr = reportStateForLog());
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
    } else if (cmdName == AbortTransaction::kCommandName) {
        _state = TransactionState::kStartedAbort;
    } else {
        MONGO_UNREACHABLE;
    }

    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(cmdName, 1);
    cmdBuilder.append(WriteConcernOptions::kWriteConcernField, _writeConcern.toBSON());
    auto cmdObj = cmdBuilder.obj();

    return _txnClient->runCommand(dbName, cmdObj).semi();
}

Transaction::ErrorHandlingStep Transaction::handleError(
    const StatusWith<CommitResult>& swResult) const {
    LOGV2_DEBUG(5875905,
                0,  // TODO SERVER-61781: Raise verbosity.
                "Internal transaction handling error",
                "error"_attr = swResult.isOK() ? swResult.getValue().getEffectiveStatus()
                                               : swResult.getStatus(),
                "hasTransientTransactionErrorLabel"_attr =
                    _latestResponseHasTransientTransactionErrorLabel,
                "txnInfo"_attr = reportStateForLog());

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
        // A network error before commit is a transient transaction error.
        if (!hasStartedCommit && ErrorCodes::isNetworkError(clientStatus)) {
            return ErrorHandlingStep::kRetryTransaction;
        }
        return ErrorHandlingStep::kDoNotRetry;
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

    return ErrorHandlingStep::kDoNotRetry;
}

void Transaction::prepareRequest(BSONObjBuilder* cmdBuilder) {
    // TODO SERVER-61780 Determine if apiVersion should be set.

    _sessionInfo.serialize(cmdBuilder);

    if (_state == TransactionState::kInit) {
        _state = TransactionState::kStarted;
        _sessionInfo.setStartTransaction(boost::none);
        cmdBuilder->append(_readConcern.toBSON().firstElement());
    }

    _latestResponseHasTransientTransactionErrorLabel = false;
}

void Transaction::processResponse(const BSONObj& reply) {
    if (auto errorLabels = reply[kErrorLabelsFieldName]) {
        for (const auto& label : errorLabels.Array()) {
            if (label.String() == ErrorLabel::kTransientTransaction) {
                _latestResponseHasTransientTransactionErrorLabel = true;
            }
        }
    }
}

void Transaction::_setSessionInfo(LogicalSessionId lsid,
                                  TxnNumber txnNumber,
                                  boost::optional<TxnRetryCounter> txnRetryCounter) {
    _sessionInfo.setSessionId(lsid);
    _sessionInfo.setTxnNumber(txnNumber);
    _sessionInfo.setTxnRetryCounter(txnRetryCounter ? *txnRetryCounter : 0);
}

void Transaction::primeForTransactionRetry() {
    _latestResponseHasTransientTransactionErrorLabel = false;
    switch (_execContext) {
        case ExecutionContext::kOwnSession:
            // Advance txnNumber.
            _sessionInfo.setTxnNumber(*_sessionInfo.getTxnNumber() + 1);
            _sessionInfo.setStartTransaction(true);
            _state = TransactionState::kInit;
            return;
        case ExecutionContext::kClientSession:
            // Advance txnRetryCounter.
            _sessionInfo.setTxnRetryCounter(*_sessionInfo.getTxnRetryCounter() + 1);
            _sessionInfo.setStartTransaction(true);
            _state = TransactionState::kInit;
            return;
        case ExecutionContext::kClientRetryableWrite:
            // Advance txnRetryCounter.
            _sessionInfo.setTxnRetryCounter(*_sessionInfo.getTxnRetryCounter() + 1);
            _sessionInfo.setStartTransaction(true);
            _state = TransactionState::kInit;
            return;
        case ExecutionContext::kClientTransaction:
            // The outermost client handles retries.
            MONGO_UNREACHABLE;
    }
}

void Transaction::primeForCommitRetry() {
    invariant(_state == TransactionState::kStartedCommit);
    _latestResponseHasTransientTransactionErrorLabel = false;
    _state = TransactionState::kStarted;
}

BSONObj Transaction::reportStateForLog() const {
    return BSON("execContext" << execContextToString(_execContext) << "sessionInfo"
                              << _sessionInfo.toBSON() << "state"
                              << transactionStateToString(_state));
}

void Transaction::_primeTransaction(OperationContext* opCtx) {
    // Extract session options and infer execution context from client's opCtx.
    auto clientSession = opCtx->getLogicalSessionId();
    auto clientTxnNumber = opCtx->getTxnNumber();
    auto clientInMultiDocumentTransaction = opCtx->inMultiDocumentTransaction();
    auto clientTxnRetryCounter = opCtx->getTxnRetryCounter();

    if (!clientSession) {
        // TODO SERVER-61783: Integrate session pool.
        _setSessionInfo(makeLogicalSessionId(opCtx), 0, 0);
        _execContext = ExecutionContext::kOwnSession;
    } else if (!clientTxnNumber) {
        _setSessionInfo(makeLogicalSessionIdWithTxnUUID(*clientSession), 0, 0);
        _execContext = ExecutionContext::kClientSession;

        // TODO SERVER-59186: Handle client session case.
        MONGO_UNREACHABLE;
    } else if (!clientInMultiDocumentTransaction) {
        _setSessionInfo(
            makeLogicalSessionIdWithTxnNumberAndUUID(*clientSession, *clientTxnNumber), 0, 0);
        _execContext = ExecutionContext::kClientRetryableWrite;

        // TODO SERVER-59186: Handle client retryable write case.
        MONGO_UNREACHABLE;
    } else {
        _setSessionInfo(*clientSession, *clientTxnNumber, clientTxnRetryCounter);
        _execContext = ExecutionContext::kClientTransaction;

        // TODO SERVER-59186: Handle client transaction case.
        MONGO_UNREACHABLE;
    }
    _sessionInfo.setStartTransaction(true);
    _sessionInfo.setAutocommit(false);

    // Extract non-session options.
    _readConcern = repl::ReadConcernArgs::get(opCtx);
    _writeConcern = opCtx->getWriteConcern();

    LOGV2_DEBUG(5875901,
                0,  // TODO SERVER-61781: Raise verbosity.
                "Started internal transaction",
                "sessionInfo"_attr = _sessionInfo,
                "readConcern"_attr = _readConcern,
                "writeConcern"_attr = _writeConcern,
                "execContext"_attr = execContextToString(_execContext));
}

}  // namespace details
}  // namespace mongo::txn_api
