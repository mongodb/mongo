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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/s/transaction_router.h"

#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// TODO SERVER-39704: Remove this fail point once the router can safely retry within a transaction
// on stale version and snapshot errors.
MONGO_FAIL_POINT_DEFINE(enableStaleVersionAndSnapshotRetriesWithinTransactions);

const char kCoordinatorField[] = "coordinator";
const char kReadConcernLevelSnapshotName[] = "snapshot";

const auto getTransactionRouter = Session::declareDecoration<TransactionRouter>();

bool isTransactionCommand(const BSONObj& cmd) {
    auto cmdName = cmd.firstElement().fieldNameStringData();
    return cmdName == "abortTransaction" || cmdName == "commitTransaction" ||
        cmdName == "prepareTransaction";
}

/**
 * Attaches the given atClusterTime to the readConcern object in the given command object, removing
 * afterClusterTime if present. Assumes the given command object has a readConcern field and has
 * readConcern level snapshot.
 */
BSONObj appendAtClusterTimeToReadConcern(BSONObj cmdObj, LogicalTime atClusterTime) {
    dassert(cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

    BSONObjBuilder cmdAtClusterTimeBob;
    for (auto&& elem : cmdObj) {
        if (elem.fieldNameStringData() == repl::ReadConcernArgs::kReadConcernFieldName) {
            BSONObjBuilder readConcernBob =
                cmdAtClusterTimeBob.subobjStart(repl::ReadConcernArgs::kReadConcernFieldName);
            for (auto&& rcElem : elem.Obj()) {
                // afterClusterTime cannot be specified with atClusterTime.
                if (rcElem.fieldNameStringData() !=
                    repl::ReadConcernArgs::kAfterClusterTimeFieldName) {
                    readConcernBob.append(rcElem);
                }
            }

            dassert(readConcernBob.hasField(repl::ReadConcernArgs::kLevelFieldName) &&
                    readConcernBob.asTempObj()[repl::ReadConcernArgs::kLevelFieldName].String() ==
                        kReadConcernLevelSnapshotName);

            readConcernBob.append(repl::ReadConcernArgs::kAtClusterTimeFieldName,
                                  atClusterTime.asTimestamp());
        } else {
            cmdAtClusterTimeBob.append(elem);
        }
    }

    return cmdAtClusterTimeBob.obj();
}

BSONObj appendReadConcernForTxn(BSONObj cmd,
                                repl::ReadConcernArgs readConcernArgs,
                                boost::optional<LogicalTime> atClusterTime) {
    // Check for an existing read concern. The first statement in a transaction may already have
    // one, in which case its level should always match the level of the transaction's readConcern.
    if (cmd.hasField(repl::ReadConcernArgs::kReadConcernFieldName)) {
        repl::ReadConcernArgs existingReadConcernArgs;
        dassert(existingReadConcernArgs.initialize(cmd));
        dassert(existingReadConcernArgs.getLevel() == readConcernArgs.getLevel());

        return atClusterTime ? appendAtClusterTimeToReadConcern(std::move(cmd), *atClusterTime)
                             : cmd;
    }

    BSONObjBuilder bob(std::move(cmd));
    readConcernArgs.appendInfo(&bob);

    return atClusterTime ? appendAtClusterTimeToReadConcern(bob.asTempObj(), *atClusterTime)
                         : bob.obj();
}

BSONObjBuilder appendFieldsForStartTransaction(BSONObj cmd,
                                               repl::ReadConcernArgs readConcernArgs,
                                               boost::optional<LogicalTime> atClusterTime,
                                               bool doAppendStartTransaction) {
    auto cmdWithReadConcern = !readConcernArgs.isEmpty()
        ? appendReadConcernForTxn(std::move(cmd), readConcernArgs, atClusterTime)
        : std::move(cmd);

    BSONObjBuilder bob(std::move(cmdWithReadConcern));

    if (doAppendStartTransaction) {
        bob.append(OperationSessionInfoFromClient::kStartTransactionFieldName, true);
    }

    return bob;
}

// Commands that are idempotent in a transaction context and can be blindly retried in the middle of
// a transaction. Aggregate with $out is disallowed in a transaction, so aggregates must be read
// operations. Note: aggregate and find do have the side-effect of creating cursors, but any
// established during an unsuccessful attempt are best-effort killed.
const StringMap<int> alwaysRetryableCmds = {
    {"aggregate", 1}, {"distinct", 1}, {"find", 1}, {"getMore", 1}, {"killCursors", 1}};

bool isReadConcernLevelAllowedInTransaction(repl::ReadConcernLevel readConcernLevel) {
    return readConcernLevel == repl::ReadConcernLevel::kSnapshotReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kMajorityReadConcern ||
        readConcernLevel == repl::ReadConcernLevel::kLocalReadConcern;
}

// Returns if the error code would be considered a retryable error for a retryable write.
bool isRetryableWritesError(ErrorCodes::Error code) {
    return std::find(RemoteCommandRetryScheduler::kAllRetriableErrors.begin(),
                     RemoteCommandRetryScheduler::kAllRetriableErrors.end(),
                     code) != RemoteCommandRetryScheduler::kAllRetriableErrors.end();
}

// Returns if a transaction's commit result is unknown based on the given statuses. A result is
// considered unknown if it would be given the "UnknownTransactionCommitResult" as defined by the
// driver transactions specification or fails with one of the errors for invalid write concern that
// are specifically not given the "UnknownTransactionCommitResult" label. Additionally,
// TransactionTooOld is considered unknown because a command that fails with it could not have done
// meaningful work.
//
// The "UnknownTransactionCommitResult" specification:
// https://github.com/mongodb/specifications/blob/master/source/transactions/transactions.rst#unknowntransactioncommitresult.
bool isCommitResultUnknown(const Status& commitStatus, const Status& commitWCStatus) {
    if (!commitStatus.isOK()) {
        return isRetryableWritesError(commitStatus.code()) ||
            ErrorCodes::isExceededTimeLimitError(commitStatus.code()) ||
            commitStatus.code() == ErrorCodes::TransactionTooOld;
    }

    if (!commitWCStatus.isOK()) {
        return true;
    }

    return false;
}

BSONObj sendCommitDirectlyToShards(OperationContext* opCtx, const std::vector<ShardId>& shardIds) {
    // Assemble requests.
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& shardId : shardIds) {
        CommitTransaction commitCmd;
        commitCmd.setDbName(NamespaceString::kAdminDb);
        const auto commitCmdObj = commitCmd.toBSON(
            BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));
        requests.emplace_back(shardId, commitCmdObj);
    }

    // Send the requests.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        NamespaceString::kAdminDb,
        requests,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    BSONObj lastResult;

    // Receive the responses.
    while (!ars.done()) {
        auto response = ars.next();

        uassertStatusOK(response.swResponse);
        lastResult = response.swResponse.getValue().data;

        // If any shard returned an error, return the error immediately.
        const auto commandStatus = getStatusFromCommandResult(lastResult);
        if (!commandStatus.isOK()) {
            return lastResult;
        }

        // If any participant had a writeConcern error, return the participant's writeConcern
        // error immediately.
        const auto writeConcernStatus = getWriteConcernStatusFromCommandResult(lastResult);
        if (!writeConcernStatus.isOK()) {
            return lastResult;
        }
    }

    // If all the responses were ok, return the last response.
    return lastResult;
}

}  // unnamed namespace

TransactionRouter::Participant::Participant(bool inIsCoordinator,
                                            StmtId inStmtIdCreatedAt,
                                            SharedTransactionOptions inSharedOptions)
    : isCoordinator(inIsCoordinator),
      stmtIdCreatedAt(inStmtIdCreatedAt),
      sharedOptions(std::move(inSharedOptions)) {}

BSONObj TransactionRouter::Participant::attachTxnFieldsIfNeeded(
    BSONObj cmd, bool isFirstStatementInThisParticipant) const {
    bool hasStartTxn = false;
    bool hasAutoCommit = false;
    bool hasTxnNum = false;

    BSONObjIterator iter(cmd);
    while (iter.more()) {
        auto elem = iter.next();

        if (OperationSessionInfoFromClient::kStartTransactionFieldName ==
            elem.fieldNameStringData()) {
            hasStartTxn = true;
        } else if (OperationSessionInfoFromClient::kAutocommitFieldName ==
                   elem.fieldNameStringData()) {
            hasAutoCommit = true;
        } else if (OperationSessionInfo::kTxnNumberFieldName == elem.fieldNameStringData()) {
            hasTxnNum = true;
        }
    }

    // TODO: SERVER-37045 assert when attaching startTransaction to killCursors command.

    // The first command sent to a participant must start a transaction, unless it is a transaction
    // command, which don't support the options that start transactions, i.e. startTransaction and
    // readConcern. Otherwise the command must not have a read concern.
    bool mustStartTransaction = isFirstStatementInThisParticipant && !isTransactionCommand(cmd);

    if (!mustStartTransaction) {
        dassert(!cmd.hasField(repl::ReadConcernArgs::kReadConcernFieldName));
    }

    BSONObjBuilder newCmd = mustStartTransaction
        ? appendFieldsForStartTransaction(std::move(cmd),
                                          sharedOptions.readConcernArgs,
                                          sharedOptions.atClusterTime,
                                          !hasStartTxn)
        : BSONObjBuilder(std::move(cmd));

    if (isCoordinator) {
        newCmd.append(kCoordinatorField, true);
    }

    if (!hasAutoCommit) {
        newCmd.append(OperationSessionInfoFromClient::kAutocommitFieldName, false);
    }

    if (!hasTxnNum) {
        newCmd.append(OperationSessionInfo::kTxnNumberFieldName, sharedOptions.txnNumber);
    } else {
        auto osi =
            OperationSessionInfoFromClient::parse("OperationSessionInfo"_sd, newCmd.asTempObj());
        invariant(sharedOptions.txnNumber == *osi.getTxnNumber());
    }

    return newCmd.obj();
}

void TransactionRouter::processParticipantResponse(const ShardId& shardId,
                                                   const BSONObj& responseObj) {
    auto participant = getParticipant(shardId);
    invariant(participant, "Participant should exist if processing participant response");

    if (_commitType != CommitType::kNotInitiated) {
        // Do not update a participant's transaction metadata after commit has been initiated, since
        // a participant's state is partially reset on commit.
        return;
    }

    auto commandStatus = getStatusFromCommandResult(responseObj);
    if (!commandStatus.isOK()) {
        return;
    }

    if (participant->stmtIdCreatedAt != _latestStmtId) {
        uassert(
            51112,
            str::stream() << "readOnly field for participant " << shardId
                          << " should have been set on the participant's first successful response",
            participant->readOnly != Participant::ReadOnly::kUnset);
    }

    auto txnResponseMetadata =
        TxnResponseMetadata::parse("processParticipantResponse"_sd, responseObj);

    if (txnResponseMetadata.getReadOnly()) {
        if (participant->readOnly == Participant::ReadOnly::kUnset) {
            LOG(3) << txnIdToString() << " Marking " << shardId << " as read-only";
            participant->readOnly = Participant::ReadOnly::kReadOnly;
            return;
        }

        uassert(51113,
                str::stream() << "Participant shard " << shardId
                              << " claimed to be read-only for a transaction after previously "
                                 "claiming to have done a write for the transaction",
                participant->readOnly == Participant::ReadOnly::kReadOnly);
        return;
    }

    // The shard reported readOnly:false on this statement.

    if (participant->readOnly != Participant::ReadOnly::kNotReadOnly) {
        LOG(3) << txnIdToString() << " Marking " << shardId << " as having done a write";
        participant->readOnly = Participant::ReadOnly::kNotReadOnly;

        if (!_recoveryShardId) {
            LOG(3) << txnIdToString() << " Choosing " << shardId << " as recovery shard";
            _recoveryShardId = shardId;
        }
    }
}

LogicalTime TransactionRouter::AtClusterTime::getTime() const {
    invariant(_atClusterTime != LogicalTime::kUninitialized);
    invariant(_stmtIdSelectedAt);
    return _atClusterTime;
}

void TransactionRouter::AtClusterTime::setTime(LogicalTime atClusterTime, StmtId currentStmtId) {
    invariant(atClusterTime != LogicalTime::kUninitialized);
    _atClusterTime = atClusterTime;
    _stmtIdSelectedAt = currentStmtId;
}

bool TransactionRouter::AtClusterTime::canChange(StmtId currentStmtId) const {
    return !_stmtIdSelectedAt || *_stmtIdSelectedAt == currentStmtId;
}

TransactionRouter* TransactionRouter::get(OperationContext* opCtx) {
    const auto session = OperationContextSession::get(opCtx);
    if (session) {
        return &getTransactionRouter(session);
    }

    return nullptr;
}

TransactionRouter* get(const ObservableSession& osession) {
    return &getTransactionRouter(osession.get());
}

TransactionRouter::TransactionRouter() = default;

TransactionRouter::~TransactionRouter() = default;

const boost::optional<TransactionRouter::AtClusterTime>& TransactionRouter::getAtClusterTime()
    const {
    return _atClusterTime;
}

const boost::optional<ShardId>& TransactionRouter::getCoordinatorId() const {
    return _coordinatorId;
}

const boost::optional<ShardId>& TransactionRouter::getRecoveryShardId() const {
    return _recoveryShardId;
}

BSONObj TransactionRouter::attachTxnFieldsIfNeeded(const ShardId& shardId, const BSONObj& cmdObj) {
    if (auto txnPart = getParticipant(shardId)) {
        LOG(4) << txnIdToString()
               << " Sending transaction fields to existing participant: " << shardId;
        return txnPart->attachTxnFieldsIfNeeded(cmdObj, false);
    }

    auto txnPart = _createParticipant(shardId);
    LOG(4) << txnIdToString() << " Sending transaction fields to new participant: " << shardId;
    return txnPart.attachTxnFieldsIfNeeded(cmdObj, true);
}

void TransactionRouter::_verifyParticipantAtClusterTime(const Participant& participant) {
    const auto& participantAtClusterTime = participant.sharedOptions.atClusterTime;
    invariant(participantAtClusterTime);
    invariant(*participantAtClusterTime == _atClusterTime->getTime());
}

TransactionRouter::Participant* TransactionRouter::getParticipant(const ShardId& shard) {
    const auto iter = _participants.find(shard.toString());
    if (iter == _participants.end())
        return nullptr;

    if (_atClusterTime) {
        _verifyParticipantAtClusterTime(iter->second);
    }

    return &iter->second;
}

TransactionRouter::Participant& TransactionRouter::_createParticipant(const ShardId& shard) {
    // The first participant is chosen as the coordinator.
    auto isFirstParticipant = _participants.empty();
    if (isFirstParticipant) {
        invariant(!_coordinatorId);
        _coordinatorId = shard.toString();
    }

    SharedTransactionOptions sharedOptions = {
        _txnNumber,
        _readConcernArgs,
        _atClusterTime ? boost::optional<LogicalTime>(_atClusterTime->getTime()) : boost::none};

    auto resultPair =
        _participants.try_emplace(shard.toString(),
                                  TransactionRouter::Participant(
                                      isFirstParticipant, _latestStmtId, std::move(sharedOptions)));

    return resultPair.first->second;
}

void TransactionRouter::_assertAbortStatusIsOkOrNoSuchTransaction(
    const AsyncRequestsSender::Response& response) const {
    auto shardResponse = uassertStatusOKWithContext(
        std::move(response.swResponse),
        str::stream() << "Failed to send abort to shard " << response.shardId
                      << " between retries of statement "
                      << _latestStmtId);

    auto status = getStatusFromCommandResult(shardResponse.data);
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << txnIdToString() << "Transaction aborted between retries of statement "
                          << _latestStmtId
                          << " due to error: "
                          << status
                          << " from shard: "
                          << response.shardId,
            status.isOK() || status.code() == ErrorCodes::NoSuchTransaction);

    // abortTransaction is sent with no write concern, so there's no need to check for a write
    // concern error.
}

std::vector<ShardId> TransactionRouter::_getPendingParticipants() const {
    std::vector<ShardId> pendingParticipants;
    for (const auto& participant : _participants) {
        if (participant.second.stmtIdCreatedAt == _latestStmtId) {
            pendingParticipants.emplace_back(ShardId(participant.first));
        }
    }
    return pendingParticipants;
}

void TransactionRouter::_clearPendingParticipants(OperationContext* opCtx) {
    const auto pendingParticipants = _getPendingParticipants();

    // Send abort to each pending participant. This resets their transaction state and guarantees no
    // transactions will be left open if the retry does not re-target any of these shards.
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participant : pendingParticipants) {
        abortRequests.emplace_back(participant, BSON("abortTransaction" << 1));
    }
    auto responses = gatherResponses(opCtx,
                                     NamespaceString::kAdminDb,
                                     ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                     Shard::RetryPolicy::kIdempotent,
                                     abortRequests);

    // Verify each abort succeeded or failed with NoSuchTransaction, which may happen if the
    // transaction was already implicitly aborted on the shard.
    for (const auto& response : responses) {
        _assertAbortStatusIsOkOrNoSuchTransaction(response);
    }

    // Remove each aborted participant from the participant list. Remove after sending abort, so
    // they are not added back to the participant list by the transaction tracking inside the ARS.
    for (const auto& participant : pendingParticipants) {
        // If the participant being removed was chosen as the recovery shard, reset the recovery
        // shard. This is safe because this participant is a pending participant, meaning it
        // cannot have been returned in the recoveryToken on an earlier statement.
        if (_recoveryShardId && *_recoveryShardId == participant) {
            _recoveryShardId.reset();
        }
        invariant(_participants.erase(participant));
    }

    // If there are no more participants, also clear the coordinator id because a new one must be
    // chosen by the retry.
    if (_participants.empty()) {
        _coordinatorId.reset();
        return;
    }

    // If participants were created by an earlier command, the coordinator must be one of them.
    invariant(_coordinatorId);
    invariant(_participants.count(*_coordinatorId) == 1);
}

bool TransactionRouter::canContinueOnStaleShardOrDbError(StringData cmdName) const {
    if (MONGO_FAIL_POINT(enableStaleVersionAndSnapshotRetriesWithinTransactions)) {
        // We can always retry on the first overall statement because all targeted participants must
        // be pending, so the retry will restart the local transaction on each one, overwriting any
        // effects from the first attempt.
        if (_latestStmtId == _firstStmtId) {
            return true;
        }

        // Only idempotent operations can be retried if the error came from a later statement
        // because non-pending participants targeted by the statement may receive the same statement
        // id more than once, and currently statement ids are not tracked by participants so the
        // operation would be applied each time.
        //
        // Note that the retry will fail if any non-pending participants returned a stale version
        // error during the latest statement, because the error will abort their local transactions
        // but the router's retry will expect them to be in-progress.
        if (alwaysRetryableCmds.count(cmdName)) {
            return true;
        }
    }

    return false;
}

void TransactionRouter::onStaleShardOrDbError(OperationContext* opCtx,
                                              StringData cmdName,
                                              const Status& errorStatus) {
    invariant(canContinueOnStaleShardOrDbError(cmdName));

    LOG(3) << txnIdToString()
           << " Clearing pending participants after stale version error: " << errorStatus;

    // Remove participants created during the current statement so they are sent the correct options
    // if they are targeted again by the retry.
    _clearPendingParticipants(opCtx);
}

void TransactionRouter::onViewResolutionError(OperationContext* opCtx, const NamespaceString& nss) {
    // The router can always retry on a view resolution error.

    LOG(3) << txnIdToString()
           << " Clearing pending participants after view resolution error on namespace: " << nss;

    // Requests against views are always routed to the primary shard for its database, but the retry
    // on the resolved namespace does not have to re-target the primary, so pending participants
    // should be cleared.
    _clearPendingParticipants(opCtx);
}

bool TransactionRouter::canContinueOnSnapshotError() const {
    if (MONGO_FAIL_POINT(enableStaleVersionAndSnapshotRetriesWithinTransactions)) {
        return _atClusterTime && _atClusterTime->canChange(_latestStmtId);
    }

    return false;
}

void TransactionRouter::onSnapshotError(OperationContext* opCtx, const Status& errorStatus) {
    invariant(canContinueOnSnapshotError());

    LOG(3) << txnIdToString() << " Clearing pending participants and resetting global snapshot "
                                 "timestamp after snapshot error: "
           << errorStatus << ", previous timestamp: " << _atClusterTime->getTime();

    // The transaction must be restarted on all participants because a new read timestamp will be
    // selected, so clear all pending participants. Snapshot errors are only retryable on the first
    // client statement, so all participants should be cleared, including the coordinator.
    _clearPendingParticipants(opCtx);
    invariant(_participants.empty());
    invariant(!_coordinatorId);

    // Reset the global snapshot timestamp so the retry will select a new one.
    _atClusterTime.reset();
    _atClusterTime.emplace();
}

void TransactionRouter::setDefaultAtClusterTime(OperationContext* opCtx) {
    if (!_atClusterTime || !_atClusterTime->canChange(_latestStmtId)) {
        return;
    }

    auto defaultTime = LogicalClock::get(opCtx)->getClusterTime();
    _setAtClusterTime(repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime(), defaultTime);
}

void TransactionRouter::_setAtClusterTime(const boost::optional<LogicalTime>& afterClusterTime,
                                          LogicalTime candidateTime) {
    // If the user passed afterClusterTime, the chosen time must be greater than or equal to it.
    if (afterClusterTime && *afterClusterTime > candidateTime) {
        _atClusterTime->setTime(*afterClusterTime, _latestStmtId);
        return;
    }

    LOG(2) << txnIdToString() << " Setting global snapshot timestamp to " << candidateTime
           << " on statement " << _latestStmtId;

    _atClusterTime->setTime(candidateTime, _latestStmtId);
}

void TransactionRouter::beginOrContinueTxn(OperationContext* opCtx,
                                           TxnNumber txnNumber,
                                           TransactionActions action) {
    if (txnNumber < _txnNumber) {
        // This transaction is older than the transaction currently in progress, so throw an error.
        uasserted(ErrorCodes::TransactionTooOld,
                  str::stream() << "txnNumber " << txnNumber << " is less than last txnNumber "
                                << _txnNumber
                                << " seen in session "
                                << _sessionId());
    } else if (txnNumber == _txnNumber) {
        // This is the same transaction as the one in progress.
        switch (action) {
            case TransactionActions::kStart: {
                uasserted(ErrorCodes::ConflictingOperationInProgress,
                          str::stream() << "txnNumber " << _txnNumber << " for session "
                                        << _sessionId()
                                        << " already started");
            }
            case TransactionActions::kContinue: {
                uassert(ErrorCodes::InvalidOptions,
                        "Only the first command in a transaction may specify a readConcern",
                        repl::ReadConcernArgs::get(opCtx).isEmpty());

                repl::ReadConcernArgs::get(opCtx) = _readConcernArgs;
                ++_latestStmtId;
                return;
            }
            case TransactionActions::kCommit:
                ++_latestStmtId;
                return;
        }
    } else if (txnNumber > _txnNumber) {
        // This is a newer transaction.
        switch (action) {
            case TransactionActions::kStart: {
                auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
                uassert(
                    ErrorCodes::InvalidOptions,
                    "The first command in a transaction cannot specify a readConcern level other "
                    "than local, majority, or snapshot",
                    !readConcernArgs.hasLevel() ||
                        isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel()));

                _resetRouterState(txnNumber);

                _readConcernArgs = readConcernArgs;

                if (_readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
                    _atClusterTime.emplace();
                }

                _onNewTransaction(opCtx);
                LOG(3) << txnIdToString() << " New transaction started";
                return;
            }
            case TransactionActions::kContinue: {
                uasserted(ErrorCodes::NoSuchTransaction,
                          str::stream() << "cannot continue txnId " << _txnNumber << " for session "
                                        << _sessionId()
                                        << " with txnId "
                                        << txnNumber);
            }
            case TransactionActions::kCommit: {
                _resetRouterState(txnNumber);
                // If the first action seen by the router for this transaction is to commit, that
                // means that the client is attempting to recover a commit decision.
                _isRecoveringCommit = true;

                _onBeginRecoveringDecision(opCtx);
                LOG(3) << txnIdToString() << " Commit recovery started";
                return;
            }
        };
    }
    MONGO_UNREACHABLE;
}

const LogicalSessionId& TransactionRouter::_sessionId() const {
    const auto* owningSession = getTransactionRouter.owner(this);
    return owningSession->getSessionId();
}

BSONObj TransactionRouter::_handOffCommitToCoordinator(OperationContext* opCtx) {
    invariant(_coordinatorId);
    auto coordinatorIter = _participants.find(*_coordinatorId);
    invariant(coordinatorIter != _participants.end());

    std::vector<CommitParticipant> participantList;
    for (const auto& participantEntry : _participants) {
        CommitParticipant commitParticipant;
        commitParticipant.setShardId(participantEntry.first);
        participantList.push_back(std::move(commitParticipant));
    }

    CoordinateCommitTransaction coordinateCommitCmd;
    coordinateCommitCmd.setDbName("admin");
    coordinateCommitCmd.setParticipants(participantList);
    const auto coordinateCommitCmdObj = coordinateCommitCmd.toBSON(
        BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));

    _commitType = CommitType::kTwoPhaseCommit;

    LOG(3) << txnIdToString()
           << " Committing using two-phase commit, coordinator: " << *_coordinatorId;

    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        NamespaceString::kAdminDb,
        {{*_coordinatorId, coordinateCommitCmdObj}},
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    auto response = ars.next();
    invariant(ars.done());
    uassertStatusOK(response.swResponse);

    return response.swResponse.getValue().data;
}

BSONObj TransactionRouter::commitTransaction(
    OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken) {
    _onStartCommit(opCtx);

    auto commitRes = _commitTransaction(opCtx, recoveryToken);

    auto commitStatus = getStatusFromCommandResult(commitRes);
    auto commitWCStatus = getWriteConcernStatusFromCommandResult(commitRes);

    if (isCommitResultUnknown(commitStatus, commitWCStatus)) {
        // Don't update stats if we don't know the result of the transaction. The client may choose
        // to retry commit, which will update stats if the result is determined.
        //
        // Note that we also don't end the transaction if _commitTransaction() throws, which it
        // should only do on failure to send a request, in which case the commit result is unknown.
        return commitRes;
    }

    if (commitStatus.isOK()) {
        _onSuccessfulCommit(opCtx);
    } else {
        // Note that write concern errors are never considered a fatal commit error because they
        // should be retryable, so it is fine to only pass the top-level status.
        _onNonRetryableCommitError(opCtx, commitStatus);
    }

    return commitRes;
}

BSONObj TransactionRouter::_commitTransaction(
    OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken) {
    if (_isRecoveringCommit) {
        uassert(50940,
                "Cannot recover the transaction decision without a recoveryToken",
                recoveryToken);
        _commitType = CommitType::kRecoverWithToken;
        return _commitWithRecoveryToken(opCtx, *recoveryToken);
    }

    if (_participants.empty()) {
        // The participants list can be empty if a transaction was began on mongos, but it never
        // ended up targeting any hosts. Such cases are legal for example if a find is issued
        // against a non-existent database.
        uassert(ErrorCodes::IllegalOperation,
                "Cannot commit without participants",
                _txnNumber != kUninitializedTxnNumber);
        _commitType = CommitType::kNoShards;
        return BSON("ok" << 1);
    }

    std::vector<ShardId> readOnlyShards;
    std::vector<ShardId> writeShards;
    for (const auto& participant : _participants) {
        switch (participant.second.readOnly) {
            case Participant::ReadOnly::kUnset:
                uasserted(ErrorCodes::NoSuchTransaction,
                          str::stream() << txnIdToString() << " Failed to commit transaction "
                                        << "because a previous statement on the transaction "
                                        << "participant "
                                        << participant.first
                                        << " was unsuccessful.");
            case Participant::ReadOnly::kReadOnly:
                readOnlyShards.push_back(participant.first);
                break;
            case Participant::ReadOnly::kNotReadOnly:
                writeShards.push_back(participant.first);
                break;
        }
    }

    if (_participants.size() == 1) {
        ShardId shardId = _participants.cbegin()->first;
        LOG(3) << txnIdToString()
               << " Committing single-shard transaction, single participant: " << shardId;
        _commitType = CommitType::kSingleShard;
        return sendCommitDirectlyToShards(opCtx, {shardId});
    }

    if (writeShards.size() == 0) {
        LOG(3) << txnIdToString() << " Committing read-only transaction on "
               << readOnlyShards.size() << " shards";
        _commitType = CommitType::kReadOnly;
        return sendCommitDirectlyToShards(opCtx, readOnlyShards);
    }

    if (writeShards.size() == 1) {
        LOG(3) << txnIdToString() << " Committing single-write-shard transaction with "
               << readOnlyShards.size()
               << " read-only shards, write shard: " << writeShards.front();
        _commitType = CommitType::kSingleWriteShard;
        const auto readOnlyShardsResponse = sendCommitDirectlyToShards(opCtx, readOnlyShards);

        if (!getStatusFromCommandResult(readOnlyShardsResponse).isOK() ||
            !getWriteConcernStatusFromCommandResult(readOnlyShardsResponse).isOK()) {
            return readOnlyShardsResponse;
        }
        return sendCommitDirectlyToShards(opCtx, writeShards);
    }

    return _handOffCommitToCoordinator(opCtx);
}

BSONObj TransactionRouter::abortTransaction(OperationContext* opCtx) {
    _onExplicitAbort(opCtx);

    // The router has yet to send any commands to a remote shard for this transaction.
    // Return the same error that would have been returned by a shard.
    uassert(ErrorCodes::NoSuchTransaction,
            "no known command has been sent by this router for this transaction",
            !_participants.empty());

    auto abortCmd = BSON("abortTransaction" << 1);
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participantEntry : _participants) {
        abortRequests.emplace_back(ShardId(participantEntry.first), abortCmd);
    }

    LOG(3) << txnIdToString() << " Aborting transaction on " << _participants.size() << " shard(s)";

    const auto responses = gatherResponses(opCtx,
                                           NamespaceString::kAdminDb,
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           Shard::RetryPolicy::kIdempotent,
                                           abortRequests);

    BSONObj lastResult;
    for (const auto& response : responses) {
        uassertStatusOK(response.swResponse);

        lastResult = response.swResponse.getValue().data;

        // If any shard returned an error, return the error immediately.
        const auto commandStatus = getStatusFromCommandResult(lastResult);
        if (!commandStatus.isOK()) {
            return lastResult;
        }

        // If any participant had a writeConcern error, return the participant's writeConcern
        // error immediately.
        const auto writeConcernStatus = getWriteConcernStatusFromCommandResult(lastResult);
        if (!writeConcernStatus.isOK()) {
            return lastResult;
        }
    }

    // If all the responses were ok, return the last response.
    return lastResult;
}

void TransactionRouter::implicitlyAbortTransaction(OperationContext* opCtx,
                                                   const Status& errorStatus) {
    if (_commitType == CommitType::kTwoPhaseCommit ||
        _commitType == CommitType::kRecoverWithToken) {
        LOG(3) << txnIdToString() << " Router not sending implicit abortTransaction because commit "
                                     "may have been handed off to the coordinator";
        return;
    }

    _onImplicitAbort(opCtx, errorStatus);

    if (_participants.empty()) {
        return;
    }

    auto abortCmd = BSON("abortTransaction" << 1);
    std::vector<AsyncRequestsSender::Request> abortRequests;
    for (const auto& participantEntry : _participants) {
        abortRequests.emplace_back(ShardId(participantEntry.first), abortCmd);
    }

    LOG(3) << txnIdToString() << " Implicitly aborting transaction on " << _participants.size()
           << " shard(s) due to error: " << errorStatus;

    try {
        // Ignore the responses.
        gatherResponses(opCtx,
                        NamespaceString::kAdminDb,
                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                        Shard::RetryPolicy::kIdempotent,
                        abortRequests);
    } catch (const DBException& ex) {
        LOG(3) << txnIdToString() << " Implicitly aborting transaction failed "
               << causedBy(ex.toStatus());
        // Ignore any exceptions.
    }
}

std::string TransactionRouter::txnIdToString() const {
    return str::stream() << _sessionId().getId() << ":" << _txnNumber;
}

void TransactionRouter::appendRecoveryToken(BSONObjBuilder* builder) const {
    BSONObjBuilder recoveryTokenBuilder(
        builder->subobjStart(CommitTransaction::kRecoveryTokenFieldName));
    TxnRecoveryToken recoveryToken;

    // The recovery shard is chosen on the first statement that did a write (transactions that only
    // did reads do not need to be recovered; they can just be retried).
    if (_recoveryShardId) {
        invariant(_participants.find(*_recoveryShardId)->second.readOnly ==
                  Participant::ReadOnly::kNotReadOnly);
        recoveryToken.setRecoveryShardId(*_recoveryShardId);
    }

    recoveryToken.serialize(&recoveryTokenBuilder);
    recoveryTokenBuilder.doneFast();
}

void TransactionRouter::_resetRouterState(const TxnNumber& txnNumber) {
    _txnNumber = txnNumber;
    _commitType = CommitType::kNotInitiated;
    _isRecoveringCommit = false;
    _participants.clear();
    _coordinatorId.reset();
    _recoveryShardId.reset();
    _readConcernArgs = {};
    _atClusterTime.reset();
    _abortCause = std::string();
    _timingStats = TimingStats();

    // TODO SERVER-37115: Parse statement ids from the client and remember the statement id
    // of the command that started the transaction, if one was included.
    _latestStmtId = kDefaultFirstStmtId;
    _firstStmtId = kDefaultFirstStmtId;
};

BSONObj TransactionRouter::_commitWithRecoveryToken(OperationContext* opCtx,
                                                    const TxnRecoveryToken& recoveryToken) {
    uassert(ErrorCodes::NoSuchTransaction,
            "Recovery token is empty, meaning the transaction only performed reads and can be "
            "safely retried",
            recoveryToken.getRecoveryShardId());
    const auto& recoveryShardId = *recoveryToken.getRecoveryShardId();

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    auto coordinateCommitCmd = [&] {
        CoordinateCommitTransaction coordinateCommitCmd;
        coordinateCommitCmd.setDbName("admin");
        coordinateCommitCmd.setParticipants({});

        auto rawCoordinateCommit = coordinateCommitCmd.toBSON(
            BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));

        auto existingParticipant = getParticipant(recoveryShardId);
        auto recoveryParticipant =
            existingParticipant ? existingParticipant : &_createParticipant(recoveryShardId);
        return recoveryParticipant->attachTxnFieldsIfNeeded(rawCoordinateCommit, false);
    }();

    auto recoveryShard = uassertStatusOK(shardRegistry->getShard(opCtx, recoveryShardId));
    return uassertStatusOK(recoveryShard->runCommandWithFixedRetryAttempts(
                               opCtx,
                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                               "admin",
                               coordinateCommitCmd,
                               Shard::RetryPolicy::kIdempotent))
        .response;
}

void TransactionRouter::_logSlowTransaction(OperationContext* opCtx,
                                            TerminationCause terminationCause) const {
    log() << "transaction " << _transactionInfoForLog(opCtx, terminationCause);
}

std::string TransactionRouter::_transactionInfoForLog(OperationContext* opCtx,
                                                      TerminationCause terminationCause) const {
    StringBuilder sb;

    BSONObjBuilder parametersBuilder;

    BSONObjBuilder lsidBuilder(parametersBuilder.subobjStart("lsid"));
    _sessionId().serialize(&lsidBuilder);
    lsidBuilder.doneFast();

    parametersBuilder.append("txnNumber", _txnNumber);
    parametersBuilder.append("autocommit", false);
    _readConcernArgs.appendInfo(&parametersBuilder);

    sb << "parameters:" << parametersBuilder.obj().toString() << ",";

    if (_atClusterTime) {
        sb << " globalReadTimestamp:" << _atClusterTime->getTime().toString() << ",";
    }

    if (_commitType != CommitType::kRecoverWithToken) {
        // We don't know the participants if we're recovering the commit.
        sb << " numParticipants:" << _participants.size() << ",";
    }

    if (_commitType == CommitType::kTwoPhaseCommit) {
        invariant(_coordinatorId);
        sb << " coordinator:" << *_coordinatorId << ",";
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    if (terminationCause == TerminationCause::kCommitted) {
        sb << " terminationCause:committed,";

        invariant(_commitType != CommitType::kNotInitiated);
        invariant(_abortCause.empty());
    } else {
        sb << " terminationCause:aborted,";

        invariant(!_abortCause.empty());
        sb << " abortCause:" << _abortCause << ",";

        // TODO SERVER-40985: Log abortSource
    }

    if (_commitType != CommitType::kNotInitiated) {
        sb << " commitType:" << _commitTypeToString(_commitType) << ",";

        sb << " commitDurationMicros:"
           << durationCount<Microseconds>(_timingStats.getCommitDuration(tickSource, curTicks))
           << ",";
    }

    // TODO SERVER-40985: Log timeActiveMicros

    // TODO SERVER-40985: Log timeInactiveMicros

    // Total duration of the transaction. Logged at the end of the line for consistency with slow
    // command logging.
    sb << " " << duration_cast<Milliseconds>(_timingStats.getDuration(tickSource, curTicks));

    return sb.str();
}

void TransactionRouter::_onNewTransaction(OperationContext* opCtx) {
    auto tickSource = opCtx->getServiceContext()->getTickSource();
    _timingStats.startTime = tickSource->getTicks();
}

void TransactionRouter::_onBeginRecoveringDecision(OperationContext* opCtx) {
    auto tickSource = opCtx->getServiceContext()->getTickSource();
    _timingStats.startTime = tickSource->getTicks();
}

void TransactionRouter::_onImplicitAbort(OperationContext* opCtx, const Status& errorStatus) {
    if (_commitType != CommitType::kNotInitiated && _timingStats.endTime == 0) {
        // If commit was started but an end time wasn't set, then we don't know the commit result
        // and can't consider the transaction over until the client retries commit and definitively
        // learns the result. Note that this behavior may lead to no logging in some cases, but
        // should avoid logging an incorrect decision.
        return;
    }

    // Implicit abort may execute multiple times if a misbehaving client keeps sending statements
    // for a txnNumber after receiving an error, so only remember the first abort cause.
    if (_abortCause.empty()) {
        _abortCause = errorStatus.codeString();
    }

    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::_onExplicitAbort(OperationContext* opCtx) {
    // A behaving client should never try to commit after attempting to abort, so we can consider
    // the transaction terminated as soon as explicit abort is observed.
    if (_abortCause.empty()) {
        // Note this code means the abort was from a user abortTransaction command.
        _abortCause = "abort";
    }

    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::_onStartCommit(OperationContext* opCtx) {
    auto tickSource = opCtx->getServiceContext()->getTickSource();
    if (_timingStats.commitStartTime == 0) {
        _timingStats.commitStartTime = tickSource->getTicks();
    }
}

void TransactionRouter::_onNonRetryableCommitError(OperationContext* opCtx, Status commitStatus) {
    // If the commit failed with a command error that can't be retried on, the transaction shouldn't
    // be able to eventually commit, so it can be considered over from the router's perspective.
    if (_abortCause.empty()) {
        _abortCause = commitStatus.codeString();
    }
    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kAborted);
}

void TransactionRouter::_onSuccessfulCommit(OperationContext* opCtx) {
    _endTransactionTrackingIfNecessary(opCtx, TerminationCause::kCommitted);
}

void TransactionRouter::_endTransactionTrackingIfNecessary(OperationContext* opCtx,
                                                           TerminationCause terminationCause) {
    if (_timingStats.endTime != 0) {
        // If the transaction was already ended, don't end it again.
        return;
    }

    auto tickSource = opCtx->getServiceContext()->getTickSource();
    auto curTicks = tickSource->getTicks();

    _timingStats.endTime = curTicks;

    if (shouldLog(logger::LogComponent::kTransaction, logger::LogSeverity::Debug(1)) ||
        _timingStats.getDuration(tickSource, curTicks) > Milliseconds(serverGlobalParams.slowMS)) {
        _logSlowTransaction(opCtx, terminationCause);
    }
}

Microseconds TransactionRouter::TimingStats::getDuration(TickSource* tickSource,
                                                         TickSource::Tick curTicks) const {
    invariant(startTime > 0);

    // If the transaction hasn't ended, return how long it has been running for.
    if (endTime == 0) {
        return tickSource->ticksTo<Microseconds>(curTicks - startTime);
    }
    return tickSource->ticksTo<Microseconds>(endTime - startTime);
}

Microseconds TransactionRouter::TimingStats::getCommitDuration(TickSource* tickSource,
                                                               TickSource::Tick curTicks) const {
    invariant(commitStartTime > 0);

    // If the transaction hasn't ended, return how long commit has been running for.
    if (endTime == 0) {
        return tickSource->ticksTo<Microseconds>(curTicks - commitStartTime);
    }
    return tickSource->ticksTo<Microseconds>(endTime - commitStartTime);
}

}  // namespace mongo
