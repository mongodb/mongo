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
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/session_catalog.h"
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

// TODO (SERVER-37886): Remove this failpoint once failover can be tested on coordinators that
// have a local participant.
MONGO_FAIL_POINT_DEFINE(sendCoordinateCommitToConfigServer);

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
    }
}

LogicalTime TransactionRouter::AtClusterTime::getTime() const {
    invariant(_atClusterTime != LogicalTime::kUninitialized);
    invariant(_stmtIdSelectedAt != kUninitializedStmtId);
    return _atClusterTime;
}

void TransactionRouter::AtClusterTime::setTime(LogicalTime atClusterTime, StmtId currentStmtId) {
    invariant(atClusterTime != LogicalTime::kUninitialized);
    _atClusterTime = atClusterTime;
    _stmtIdSelectedAt = currentStmtId;
}

bool TransactionRouter::AtClusterTime::canChange(StmtId currentStmtId) const {
    return _stmtIdSelectedAt == kUninitializedStmtId || _stmtIdSelectedAt == currentStmtId;
}

TransactionRouter* TransactionRouter::get(OperationContext* opCtx) {
    const auto session = OperationContextSession::get(opCtx);
    if (session) {
        return &getTransactionRouter(session);
    }

    return nullptr;
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
    if (action == TransactionActions::kStart) {
        // TODO: do we need more robust checking? Like, did we actually sent start to the
        // participants?
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "txnNumber " << _txnNumber << " for session " << _sessionId()
                              << " already started",
                txnNumber != _txnNumber);

        uassert(ErrorCodes::TransactionTooOld,
                str::stream() << "txnNumber " << txnNumber << " is less than last txnNumber "
                              << _txnNumber
                              << " seen in session "
                              << _sessionId(),
                txnNumber > _txnNumber);

        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        uassert(ErrorCodes::InvalidOptions,
                "The first command in a transaction cannot specify a readConcern level other "
                "than local, majority, or snapshot",
                !readConcernArgs.hasLevel() ||
                    isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel()));
        _readConcernArgs = readConcernArgs;
    } else if (action == TransactionActions::kCommit) {
        uassert(ErrorCodes::TransactionTooOld,
                str::stream() << "txnNumber " << txnNumber << " is less than last txnNumber "
                              << _txnNumber
                              << " seen in session "
                              << _sessionId(),
                txnNumber >= _txnNumber);

        if (_participants.empty()) {
            _isRecoveringCommit = true;
        }
    } else {
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "cannot continue txnId " << _txnNumber << " for session "
                              << _sessionId()
                              << " with txnId "
                              << txnNumber,
                txnNumber == _txnNumber);

        uassert(ErrorCodes::InvalidOptions,
                "Only the first command in a transaction may specify a readConcern",
                repl::ReadConcernArgs::get(opCtx).isEmpty());

        repl::ReadConcernArgs::get(opCtx) = _readConcernArgs;
    }

    if (_txnNumber == txnNumber) {
        ++_latestStmtId;
        return;
    }

    _txnNumber = txnNumber;
    _participants.clear();
    _coordinatorId.reset();
    _atClusterTime.reset();
    _commitType = CommitType::kNotInitiated;

    // TODO SERVER-37115: Parse statement ids from the client and remember the statement id of the
    // command that started the transaction, if one was included.
    _latestStmtId = kDefaultFirstStmtId;
    _firstStmtId = kDefaultFirstStmtId;

    if (_readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        _atClusterTime.emplace();
    }

    LOG(3) << txnIdToString() << " New transaction started";
}

const LogicalSessionId& TransactionRouter::_sessionId() const {
    const auto* owningSession = getTransactionRouter.owner(this);
    return owningSession->getSessionId();
}

BSONObj TransactionRouter::_commitSingleShardTransaction(OperationContext* opCtx) {
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    const auto citer = _participants.cbegin();

    const auto& shardId(citer->first);
    const auto& participant = citer->second;

    auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

    _commitType = CommitType::kDirectCommit;

    LOG(3) << txnIdToString()
           << " Committing single-shard transaction, single participant: " << shardId;

    CommitTransaction commitCmd;
    commitCmd.setDbName(NamespaceString::kAdminDb);

    return uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
                               opCtx,
                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                               "admin",
                               participant.attachTxnFieldsIfNeeded(
                                   commitCmd.toBSON(BSON(WriteConcernOptions::kWriteConcernField
                                                         << opCtx->getWriteConcern().toBSON())),
                                   false),
                               Shard::RetryPolicy::kIdempotent))
        .response;
}

BSONObj TransactionRouter::_commitReadOnlyTransaction(OperationContext* opCtx) {
    // Assemble requests.
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& participant : _participants) {
        CommitTransaction commitCmd;
        commitCmd.setDbName(NamespaceString::kAdminDb);
        const auto commitCmdObj = commitCmd.toBSON(
            BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));
        requests.emplace_back(participant.first, commitCmdObj);
    }

    _commitType = CommitType::kDirectCommit;

    LOG(3) << txnIdToString() << " Committing read-only transaction on " << requests.size()
           << " shards";

    // Send the requests.
    MultiStatementTransactionRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        NamespaceString::kAdminDb,
        requests,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        Shard::RetryPolicy::kIdempotent);

    // Receive the responses.
    while (!ars.done()) {
        auto response = ars.next();

        uassertStatusOK(response.swResponse);
        const auto result = response.swResponse.getValue().data;

        // If any shard returned an error, return the error immediately.
        const auto commandStatus = getStatusFromCommandResult(result);
        if (!commandStatus.isOK()) {
            return result;
        }

        // If any participant had a writeConcern error, return the participant's writeConcern
        // error immediately.
        const auto writeConcernStatus = getWriteConcernStatusFromCommandResult(result);
        if (!writeConcernStatus.isOK()) {
            return result;
        }
    }

    // If all the responses were ok, return empty BSON, which the commitTransaction command will
    // interpret as success.
    return BSONObj();
}

BSONObj TransactionRouter::_commitMultiShardTransaction(OperationContext* opCtx) {
    invariant(_coordinatorId);
    auto coordinatorIter = _participants.find(*_coordinatorId);
    invariant(coordinatorIter != _participants.end());

    std::vector<CommitParticipant> participantList;
    for (const auto& participantEntry : _participants) {
        CommitParticipant commitParticipant;
        commitParticipant.setShardId(participantEntry.first);
        participantList.push_back(std::move(commitParticipant));
    }

    auto coordinatorShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, *_coordinatorId));

    if (MONGO_FAIL_POINT(sendCoordinateCommitToConfigServer)) {
        LOG(3) << "Sending coordinateCommit for transaction " << *opCtx->getTxnNumber()
               << " on session " << opCtx->getLogicalSessionId()->toBSON()
               << " to config server rather than actual coordinator because failpoint is active";

        coordinatorShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        if (_commitType == CommitType::kNotInitiated) {
            SharedTransactionOptions options;
            options.txnNumber = _txnNumber;
            // Intentionally leave atClusterTime blank since we don't care and to minimize
            // possibility that storage engine won't have it available.
            Participant configParticipant(true, 0, options);

            // Send a fake transaction statement to the config server primary so that the config
            // server primary sets up state in memory to receive coordinateCommit.
            auto cmdResponse = coordinatorShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                "dummy",
                configParticipant.attachTxnFieldsIfNeeded(BSON("distinct"
                                                               << "dummy"
                                                               << "key"
                                                               << "dummy"),
                                                          true),
                Shard::RetryPolicy::kIdempotent);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));

            // Abort the fake transaction on the config server to release the actual transaction's
            // resources.
            cmdResponse = coordinatorShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                "admin",
                configParticipant.attachTxnFieldsIfNeeded(BSON("abortTransaction" << 1), false),
                Shard::RetryPolicy::kIdempotent);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }
    }

    CoordinateCommitTransaction coordinateCommitCmd;
    coordinateCommitCmd.setDbName("admin");
    coordinateCommitCmd.setParticipants(participantList);

    _commitType = CommitType::kTwoPhaseCommit;

    LOG(3) << txnIdToString()
           << " Committing multi-shard transaction, coordinator: " << *_coordinatorId;

    return uassertStatusOK(
               coordinatorShard->runCommandWithFixedRetryAttempts(
                   opCtx,
                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                   "admin",
                   coordinatorIter->second.attachTxnFieldsIfNeeded(
                       coordinateCommitCmd.toBSON(BSON(WriteConcernOptions::kWriteConcernField
                                                       << opCtx->getWriteConcern().toBSON())),
                       false),
                   Shard::RetryPolicy::kIdempotent))
        .response;
}

BSONObj TransactionRouter::commitTransaction(
    OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken) {
    if (_isRecoveringCommit) {
        uassert(50940,
                "Cannot recover the transaction decision without a recoveryToken",
                recoveryToken);
        return _commitWithRecoveryToken(opCtx, *recoveryToken);
    }

    if (_participants.empty()) {
        // The participants list can be empty if a transaction was began on mongos, but it never
        // ended up targeting any hosts. Such cases are legal for example if a find is issued
        // against a non-existend database.
        uassert(ErrorCodes::IllegalOperation,
                "Cannot commit without participants",
                _txnNumber != kUninitializedTxnNumber);
        return BSON("ok" << 1);
    }

    bool allParticipantsReadOnly = true;
    for (const auto& participant : _participants) {
        uassert(ErrorCodes::NoSuchTransaction,
                "Can't send commit unless all previous statements were successful",
                participant.second.readOnly != Participant::ReadOnly::kUnset);
        if (participant.second.readOnly == Participant::ReadOnly::kNotReadOnly) {
            allParticipantsReadOnly = false;
        }
    }

    // Make the single-shard commit path take precedence. The read-only optimization is only to skip
    // two-phase commit for a read-only multi-shard transaction.
    if (_participants.size() == 1) {
        return _commitSingleShardTransaction(opCtx);
    }

    if (allParticipantsReadOnly) {
        return _commitReadOnlyTransaction(opCtx);
    }

    return _commitMultiShardTransaction(opCtx);
}

std::vector<AsyncRequestsSender::Response> TransactionRouter::abortTransaction(
    OperationContext* opCtx, bool isImplicit) {
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

    // Implicit aborts log earlier.
    if (!isImplicit) {
        LOG(3) << txnIdToString() << " Aborting transaction on " << _participants.size()
               << " shard(s)";
    }

    return gatherResponses(opCtx,
                           NamespaceString::kAdminDb,
                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                           Shard::RetryPolicy::kIdempotent,
                           abortRequests);
}

void TransactionRouter::implicitlyAbortTransaction(OperationContext* opCtx,
                                                   const Status& errorStatus) {
    if (_participants.empty()) {
        return;
    }

    if (_commitType == CommitType::kTwoPhaseCommit ||
        _commitType == CommitType::kRecoverWithToken) {
        LOG(3) << txnIdToString() << " Router not sending implicit abortTransaction because commit "
                                     "may have been handed off to the coordinator";
        return;
    }

    LOG(3) << txnIdToString() << " Implicitly aborting transaction on " << _participants.size()
           << " shard(s) due to error: " << errorStatus;

    try {
        abortTransaction(opCtx, true /*isImplicit*/);
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
    if (!_coordinatorId)
        return;

    BSONObjBuilder recoveryTokenBuilder(
        builder->subobjStart(CommitTransaction::kRecoveryTokenFieldName));

    TxnRecoveryToken recoveryToken;

    // Only return a populated recovery token if the transaction has done a write (transactions that
    // only did reads do not need to be recovered; they can just be retried).
    for (const auto& participant : _participants) {
        if (participant.second.readOnly == Participant::ReadOnly::kNotReadOnly) {
            recoveryToken.setShardId(*_coordinatorId);
            break;
        }
    }

    recoveryToken.serialize(&recoveryTokenBuilder);
    recoveryTokenBuilder.doneFast();
}

BSONObj TransactionRouter::_commitWithRecoveryToken(OperationContext* opCtx,
                                                    const TxnRecoveryToken& recoveryToken) {
    uassert(ErrorCodes::NoSuchTransaction,
            "Recovery token is empty, meaning the transaction only performed reads and can be "
            "safely retried",
            recoveryToken.getShardId());
    const auto& coordinatorId = *recoveryToken.getShardId();

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    auto coordinateCommitCmd = [&] {
        CoordinateCommitTransaction coordinateCommitCmd;
        coordinateCommitCmd.setDbName("admin");
        coordinateCommitCmd.setParticipants({});

        auto rawCoordinateCommit = coordinateCommitCmd.toBSON(
            BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON()));

        auto existingParticipant = getParticipant(coordinatorId);
        auto coordinatorParticipant =
            existingParticipant ? existingParticipant : &_createParticipant(coordinatorId);
        return coordinatorParticipant->attachTxnFieldsIfNeeded(rawCoordinateCommit, false);
    }();

    _commitType = CommitType::kRecoverWithToken;

    auto coordinatorShard = uassertStatusOK(shardRegistry->getShard(opCtx, coordinatorId));
    return uassertStatusOK(coordinatorShard->runCommandWithFixedRetryAttempts(
                               opCtx,
                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                               "admin",
                               coordinateCommitCmd,
                               Shard::RetryPolicy::kIdempotent))
        .response;
}

}  // namespace mongo
