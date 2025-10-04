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


#include "mongo/db/transaction/transaction_api.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/api_parameters_gen.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/error_labels.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/read_write_concern_provenance_base_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_validation.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

MONGO_FAIL_POINT_DEFINE(overrideTransactionApiMaxRetriesToThree);
MONGO_FAIL_POINT_DEFINE(pauseAfterTxnStarted);

namespace mongo {
namespace txn_api {
namespace details {

void Transaction::TransactionState::transitionTo(StateFlag newState) {
    invariant(_isLegalTransition(_state, newState),
              str::stream() << "Current state: " << toString(_state)
                            << ", Illegal attempted next state: " << toString(newState));
    _state = newState;
}

std::string Transaction::TransactionState::toString(StateFlag state) {
    switch (state) {
        case StateFlag::kInit:
            return "init";
        case StateFlag::kStarted:
            return "started";
        case StateFlag::kStartedCommit:
            return "started commit";
        case StateFlag::kRetryingCommit:
            return "retrying commit";
        case StateFlag::kStartedAbort:
            return "started abort";
        case StateFlag::kNeedsCleanup:
            return "needs cleanup";
    }
    MONGO_UNREACHABLE;
}

bool Transaction::TransactionState::_isLegalTransition(StateFlag oldState, StateFlag newState) {
    switch (oldState) {
        case kInit:
            switch (newState) {
                case kStarted:
                case kNeedsCleanup:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kStarted:
            switch (newState) {
                case kInit:
                case kStartedCommit:
                case kStartedAbort:
                case kNeedsCleanup:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kStartedCommit:
            switch (newState) {
                case kInit:
                case kRetryingCommit:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kRetryingCommit:
            switch (newState) {
                case kInit:
                case kRetryingCommit:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kStartedAbort:
            switch (newState) {
                case kInit:
                    return true;
                default:
                    return false;
            }
            MONGO_UNREACHABLE;
        case kNeedsCleanup:
            return false;
    }
    MONGO_UNREACHABLE;
}

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


SemiFuture<CommitResult> Transaction::commit() {
    return _commitOrAbort(DatabaseName::kAdmin, CommitTransaction::kCommandName)
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

SemiFuture<AbortResult> Transaction::abort() {
    return _commitOrAbort(DatabaseName::kAdmin, AbortTransaction::kCommandName)
        .thenRunOn(_executor)
        .then([](BSONObj res) {
            auto wcErrorHolder = getWriteConcernErrorDetailFromBSONObj(res);
            WriteConcernErrorDetail wcError;
            if (wcErrorHolder) {
                wcErrorHolder->cloneTo(&wcError);
            }
            return AbortResult{getStatusFromCommandResult(res), wcError};
        })
        .semi();
}

SemiFuture<BSONObj> Transaction::_commitOrAbort(const DatabaseName& dbName, StringData cmdName) {
    BSONObjBuilder cmdBuilder;
    cmdBuilder.append(cmdName, 1);

    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

        if (_state.is(TransactionState::kInit)) {
            LOGV2_DEBUG(
                5875903,
                3,
                "Internal transaction skipping commit or abort because no commands were run",
                "cmdName"_attr = cmdName,
                "txnInfo"_attr = _reportStateForLog(lg));
            return BSON("ok" << 1);
        }
        uassert(5875902,
                fmt::format("Internal transaction not in progress, state: {}", _state.toString()),
                _state.is(TransactionState::kStarted) ||
                    // Allows retrying commit.
                    (_state.isInCommit() && cmdName == CommitTransaction::kCommandName) ||
                    // Allows best effort abort to clean up after giving up.
                    (_state.is(TransactionState::kNeedsCleanup) &&
                     cmdName == AbortTransaction::kCommandName));

        if (cmdName == CommitTransaction::kCommandName) {
            if (!_state.isInCommit()) {
                // Only transition if we aren't already retrying commit.
                _state.transitionTo(TransactionState::kStartedCommit);
            }

            if (_execContext == ExecutionContext::kClientTransaction) {
                // Don't commit if we're nested in a client's transaction.
                return SemiFuture<BSONObj>::makeReady(BSON("ok" << 1));
            }
        } else if (cmdName == AbortTransaction::kCommandName) {
            if (!_state.is(TransactionState::kNeedsCleanup)) {
                _state.transitionTo(TransactionState::kStartedAbort);
            }
            invariant(_execContext != ExecutionContext::kClientTransaction);
        } else {
            MONGO_UNREACHABLE;
        }

        if (_state.is(TransactionState::kRetryingCommit)) {
            // Per the drivers transaction spec, retrying commitTransaction uses majority write
            // concern to avoid double applying a transaction due to a transient NoSuchTransaction
            // error response.
            cmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                              defaultMajorityWriteConcernDoNotUse().toBSON());
        } else {
            cmdBuilder.append(WriteConcernOptions::kWriteConcernField, _writeConcern);
        }
    }

    return ExecutorFuture<void>(_executor)
        .then([this, dbNameCopy = dbName, cmdObj = cmdBuilder.obj()] {
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
    return !serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer) &&
        !txnClient.runsClusterOperations();
}

Transaction::ErrorHandlingStep Transaction::handleError(const StatusWith<CommitResult>& swResult,
                                                        int attemptCounter) const noexcept {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    // Errors aborting are always ignored.
    invariant(!_state.is(TransactionState::kNeedsCleanup));
    invariant(!_state.is(TransactionState::kStartedAbort));

    LOGV2_DEBUG(5875905,
                3,
                "Internal transaction handling error",
                "error"_attr = swResult.isOK() ? redact(swResult.getValue().getEffectiveStatus())
                                               : redact(swResult.getStatus()),
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
        return _state.isInCommit() ? ErrorHandlingStep::kDoNotRetry
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
            if (_state.isInCommit()) {
                return ErrorHandlingStep::kRetryCommit;
            }
            return ErrorHandlingStep::kRetryTransaction;
        }
        return _state.isInCommit() ? ErrorHandlingStep::kDoNotRetry
                                   : ErrorHandlingStep::kAbortAndDoNotRetry;
    }

    if (_state.isInCommit()) {
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
                _service, cmdBuilder->asTempObj().firstElement().fieldNameStringData()) ||
                (cmdBuilder->hasField(write_ops::WriteCommandRequestBase::kStmtIdsFieldName) ||
                 cmdBuilder->hasField(write_ops::WriteCommandRequestBase::kStmtIdFieldName)) ||
                (cmdBuilder->hasField(BulkWriteCommandRequest::kStmtIdFieldName) ||
                 cmdBuilder->hasField(BulkWriteCommandRequest::kStmtIdsFieldName)),
            str::stream()
                << "In a retryable write transaction every retryable write command should have an "
                   "explicit statement id, command: "
                << redact(cmdBuilder->asTempObj()));
    }

    auto assertDoesNotHaveField = [&cmdBuilder](StringData fieldName) {
        iassert(8579100,
                fmt::format("Command object passed to the internal transaction API should not "
                            "contain the '{}' field",
                            fieldName),
                !cmdBuilder->hasField(fieldName));
    };

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    for (auto fieldName : OperationSessionInfo::fieldNames) {
        assertDoesNotHaveField(fieldName);
    }
    _sessionInfo.serialize(cmdBuilder);

    if (_state.is(TransactionState::kInit)) {
        _state.transitionTo(TransactionState::kStarted);
        _sessionInfo.setStartTransaction(boost::none);

        assertDoesNotHaveField(GenericArguments::kReadConcernFieldName);
        cmdBuilder->append(GenericArguments::kReadConcernFieldName, _readConcern);
    }

    // Append the new recalculated maxTimeMS
    if (_opDeadline) {
        assertDoesNotHaveField(GenericArguments::kMaxTimeMSFieldName);
        auto now = _service->getServiceContext()->getFastClockSource()->now();
        auto timeLeftover = std::max(Milliseconds(0), *_opDeadline - now);
        cmdBuilder->append(GenericArguments::kMaxTimeMSFieldName,
                           durationCount<Milliseconds>(timeLeftover));
    }

    // If the transaction API caller had API parameters, we should forward them in all requests.
    if (_apiParameters.getParamsPassed()) {
        for (auto fieldName : APIParametersFromClient::fieldNames) {
            assertDoesNotHaveField(fieldName);
        }
        _apiParameters.appendInfo(cmdBuilder);
    }

    _latestResponseHasTransientTransactionErrorLabel = false;
}

void Transaction::processResponse(const BSONObj& reply) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

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
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _lastOperationTime = LogicalTime();
    _latestResponseHasTransientTransactionErrorLabel = false;
    switch (_execContext) {
        case ExecutionContext::kOwnSession:
        case ExecutionContext::kClientSession:
        case ExecutionContext::kClientRetryableWrite:
            // Advance txnNumber.
            _sessionInfo.setTxnNumber(*_sessionInfo.getTxnNumber() + 1);
            _sessionInfo.setStartTransaction(true);
            _state.transitionTo(TransactionState::kInit);
            return;
        case ExecutionContext::kClientTransaction:
            // The outermost client handles retries, so we should never reach here.
            MONGO_UNREACHABLE;
    }
}

void Transaction::primeForCommitRetry() noexcept {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _latestResponseHasTransientTransactionErrorLabel = false;
    _state.transitionTo(TransactionState::kRetryingCommit);
}

void Transaction::primeForCleanup() noexcept {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (!_state.is(TransactionState::kInit)) {
        // Only cleanup if we've sent at least one command.
        _state.transitionTo(TransactionState::kNeedsCleanup);
    }
}

bool Transaction::needsCleanup() const noexcept {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _state.is(TransactionState::kNeedsCleanup);
}

CancellationToken Transaction::getTokenForCommand() const {
    if (needsCleanup()) {
        // Use an uncancelable token when cleaning up so we can still do so after the transaction
        // was cancelled. Note callers will never wait for an operation using this token.
        return CancellationToken::uncancelable();
    }
    return _token;
}

BSONObj Transaction::reportStateForLog() const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _reportStateForLog(lg);
}

BSONObj Transaction::_reportStateForLog(WithLock) const {
    return BSON("execContext" << execContextToString(_execContext) << "sessionInfo"
                              << _sessionInfo.toBSON() << "state" << _state.toString()
                              << "lastOperationTime" << _lastOperationTime.toString()
                              << "latestResponseHasTransientTransactionErrorLabel"
                              << _latestResponseHasTransientTransactionErrorLabel << "deadline"
                              << (_opDeadline ? _opDeadline->toString() : "none") << "writeConcern"
                              << _writeConcern << "readConcern" << _readConcern << "APIParameters"
                              << _apiParameters.toBSON() << "canceled" << _token.isCanceled());
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
    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);

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
            _setSessionInfo(
                lg,
                makeLogicalSessionIdWithTxnNumberAndUUID(*clientSession, *clientTxnNumber),
                0 /* txnNumber */,
                {true} /* startTransaction */);
            _execContext = ExecutionContext::kClientRetryableWrite;
        } else {
            // Note that we don't want to include startTransaction or any first transaction command
            // fields because we assume that if we're in a client transaction the component tracking
            // transactions on the process must have already been started (e.g. TransactionRouter or
            // TransactionParticipant), so when the API sends commands for this transacion that
            // component will attach the correct fields if targeting new participants. This assumes
            // this case always uses a client that runs commands against the local process service
            // entry point, which we verify with this invariant.
            invariant(_txnClient->supportsClientTransactionContext());

            uassert(6648101,
                    "Cross-shard internal transactions are not supported when run under a client "
                    "transaction directly on a shard.",
                    !_txnClient->runsClusterOperations() ||
                        serverGlobalParams.clusterRole.hasExclusively(ClusterRole::RouterServer));

            _setSessionInfo(
                lg, *clientSession, *clientTxnNumber, boost::none /* startTransaction */);
            _execContext = ExecutionContext::kClientTransaction;

            // Skip directly to the started state since we assume the client already started this
            // transaction.
            _state.transitionTo(TransactionState::kStarted);
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
    }
    LOGV2_DEBUG(5875901,
                3,
                "Started internal transaction",
                "sessionInfo"_attr = _sessionInfo,
                "readConcern"_attr = _readConcern,
                "writeConcern"_attr = _writeConcern,
                "APIParameters"_attr = _apiParameters,
                "execContext"_attr = execContextToString(_execContext));
    if (MONGO_unlikely(pauseAfterTxnStarted.shouldFail())) {
        pauseAfterTxnStarted.pauseWhileSet();
    }
}

LogicalTime Transaction::getOperationTime() const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _lastOperationTime;
}

Transaction::~Transaction() {
    if (_acquiredSessionFromPool) {
        InternalSessionPool::get(_service->getServiceContext())
            ->release({*_sessionInfo.getSessionId(), *_sessionInfo.getTxnNumber()});
        _acquiredSessionFromPool = false;
    }
}

}  // namespace details
}  // namespace txn_api
}  // namespace mongo
