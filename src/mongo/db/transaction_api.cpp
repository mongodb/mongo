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
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/future.h"
#include "mongo/transport/service_entry_point.h"

namespace mongo::txn_api {

void TransactionWithRetries::runSync(OperationContext* opCtx, TxnCallback func) {
    // TODO SERVER-59566 Add a retry policy.
    while (true) {
        try {
            ExecutorFuture<void>(_executor)
                .then([this, anchor = shared_from_this(), &func] {
                    return func(_internalTxn->getClient(), _executor);
                })
                .get(opCtx);
        } catch (const DBException& e) {
            auto nextStep = _internalTxn->handleError(e.toStatus());
            switch (nextStep) {
                case details::Transaction::ErrorHandlingStep::kDoNotRetry:
                    _bestEffortAbort(opCtx);
                    throw;
                case details::Transaction::ErrorHandlingStep::kRetryTransaction:
                    continue;
                case details::Transaction::ErrorHandlingStep::kRetryCommit:
                    MONGO_UNREACHABLE;
            }
        }

        while (true) {
            try {
                ExecutorFuture<void>(_executor)
                    .then([this, anchor = shared_from_this()] { return _internalTxn->commit(); })
                    .get(opCtx);
                return;
            } catch (const DBException& e) {
                auto nextStep = _internalTxn->handleError(e.toStatus());
                switch (nextStep) {
                    case details::Transaction::ErrorHandlingStep::kDoNotRetry:
                        _bestEffortAbort(opCtx);
                        throw;
                    case details::Transaction::ErrorHandlingStep::kRetryTransaction:
                        break;
                    case details::Transaction::ErrorHandlingStep::kRetryCommit:
                        continue;
                }
            }
        }
    }
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

SemiFuture<void> Transaction::commit() {
    return _commitOrAbort(NamespaceString::kAdminDb, CommitTransaction::kCommandName);
}

SemiFuture<void> Transaction::abort() {
    return _commitOrAbort(NamespaceString::kAdminDb, AbortTransaction::kCommandName);
}

SemiFuture<void> Transaction::_commitOrAbort(StringData dbName, StringData cmdName) {
    uassert(5875904, "Internal transaction already completed", _state != TransactionState::kDone);

    if (_state == TransactionState::kInit) {
        LOGV2_DEBUG(5875903,
                    0,  // TODO SERVER-61781: Raise verbosity.
                    "Internal transaction skipping commit or abort because no commands were run",
                    "cmdName"_attr = cmdName,
                    "sessionInfo"_attr = _sessionInfo,
                    "execContext"_attr = _execContextToString(_execContext));
        return SemiFuture<void>::makeReady();
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
    cmdBuilder.append(WriteConcernOptions::kWriteConcernField, WriteConcernOptions::Majority);
    auto cmdObj = cmdBuilder.obj();

    return _txnClient->runCommand(dbName, cmdObj)
        .thenRunOn(_executor)
        .then([this](BSONObj res) {
            uassertStatusOK(getStatusFromCommandResult(res));
            uassertStatusOK(getWriteConcernStatusFromCommandResult(res));
            _state = TransactionState::kDone;
        })
        .semi();
}

Transaction::ErrorHandlingStep Transaction::handleError(Status clientStatus) {
    LOGV2_DEBUG(5875905,
                0,  // TODO SERVER-61781: Raise verbosity.
                "Internal transaction handling error",
                "clientStatus"_attr = clientStatus,
                "latestResponseStatus"_attr = _latestResponseStatus,
                "latestResponseWCStatus"_attr = _latestResponseWCStatus,
                "txnInfo"_attr = reportStateForLog());

    if (_execContext == ExecutionContext::kClientTransaction) {
        // If we're nested in another transaction, let the outer most client decide on errors.
        return ErrorHandlingStep::kDoNotRetry;
    }

    auto hasStartedCommit = _state == TransactionState::kStartedCommit;
    auto clientReceivedNetworkError = ErrorCodes::isNetworkError(clientStatus);
    if (_latestResponseHasTransientTransactionErrorLabel ||
        // A network error before commit is a transient transaction error.
        (!hasStartedCommit && clientReceivedNetworkError)) {
        _primeForTransactionRetry();
        return ErrorHandlingStep::kRetryTransaction;
    }

    bool latestResponseErrorWasRetryable = ErrorCodes::isRetriableError(_latestResponseStatus) ||
        ErrorCodes::isRetriableError(_latestResponseWCStatus);
    if (hasStartedCommit && latestResponseErrorWasRetryable) {
        // TODO SERVER-59566: Handle timeouts and max retry attempts. Note commit might be retried
        // within the command itself, e.g. ClusterCommitTransaction uses an idempotent retry policy,
        // so we may want a timeout policy instead of number of retries.
        _primeForCommitRetry();
        return ErrorHandlingStep::kRetryCommit;
    }

    return ErrorHandlingStep::kDoNotRetry;
}

void Transaction::prepareRequest(BSONObjBuilder* cmdBuilder) {
    // TODO SERVER-61780 Determine if apiVersion should be set.

    _sessionInfo.serialize(cmdBuilder);

    if (_state == TransactionState::kInit) {
        _state = TransactionState::kStarted;
        _sessionInfo.setStartTransaction(boost::none);
    }

    _latestResponseStatus = Status::OK();
    _latestResponseWCStatus = Status::OK();
    _latestResponseHasTransientTransactionErrorLabel = false;
}

void Transaction::processResponse(const BSONObj& reply) {
    _latestResponseStatus = getStatusFromCommandResult(reply);
    _latestResponseWCStatus = getWriteConcernStatusFromCommandResult(reply);

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

void Transaction::_primeForTransactionRetry() {
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

void Transaction::_primeForCommitRetry() {
    invariant(_state == TransactionState::kStartedCommit);
    _state = TransactionState::kStarted;
}

std::string Transaction::_execContextToString(ExecutionContext execContext) {
    switch (execContext) {
        case ExecutionContext::kOwnSession:
            return "own session";
        case ExecutionContext::kClientSession:
            return "client session";
        case ExecutionContext::kClientRetryableWrite:
            return "client retryable write";
        case ExecutionContext::kClientTransaction:
            return "client transaction";
    }
    MONGO_UNREACHABLE;
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

    LOGV2_DEBUG(5875901,
                0,  // TODO SERVER-61781: Raise verbosity.
                "Started internal transaction",
                "sessionInfo"_attr = _sessionInfo,
                "execContext"_attr = _execContextToString(_execContext));
}

}  // namespace details
}  // namespace mongo::txn_api
