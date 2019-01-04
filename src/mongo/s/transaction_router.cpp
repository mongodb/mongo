
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
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// TODO (SERVER-37886): Remove this failpoint once failover can be tested on coordinators that
// have a local participant.
MONGO_FAIL_POINT_DEFINE(sendCoordinateCommitToConfigServer);

const char kCoordinatorField[] = "coordinator";
const char kReadConcernLevelSnapshotName[] = "snapshot";

const auto getTransactionRouter = Session::declareDecoration<TransactionRouter>();

bool isTransactionCommand(const BSONObj& cmd) {
    auto cmdName = cmd.firstElement().fieldNameStringData();
    return cmdName == "abortTransaction" || cmdName == "commitTransaction" ||
        cmdName == "prepareTransaction";
}

BSONObj appendAtClusterTimeToReadConcern(BSONObj cmdObj, LogicalTime atClusterTime) {
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

            // Transactions will upconvert a read concern with afterClusterTime but no level to have
            // level snapshot, so a command may have a read concern field with no level.
            //
            // TODO SERVER-37237: Once read concern handling has been consolidated on mongos, this
            // assertion can probably be removed.
            if (!readConcernBob.hasField(repl::ReadConcernArgs::kLevelFieldName)) {
                readConcernBob.append(repl::ReadConcernArgs::kLevelFieldName,
                                      kReadConcernLevelSnapshotName);
            }

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
        // There may be no read concern level if the user only specified afterClusterTime and the
        // transaction provided the default level.
        //
        // TODO SERVER-37237: Once read concern handling has been consolidated on mongos, this
        // assertion can probably be simplified or removed.
        dassert(existingReadConcernArgs.getLevel() == readConcernArgs.getLevel() ||
                !existingReadConcernArgs.hasLevel());

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
    auto cmdWithReadConcern =
        appendReadConcernForTxn(std::move(cmd), readConcernArgs, atClusterTime);
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

bool TransactionRouter::AtClusterTime::isSet() const {
    return _atClusterTime != LogicalTime::kUninitialized;
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
        LOG(4) << _txnIdToString()
               << " Sending transaction fields to existing participant: " << shardId;
        return txnPart->attachTxnFieldsIfNeeded(cmdObj, false);
    }

    auto txnPart = _createParticipant(shardId);
    LOG(4) << _txnIdToString() << " Sending transaction fields to new participant: " << shardId;
    return txnPart.attachTxnFieldsIfNeeded(cmdObj, true);
}

void TransactionRouter::_verifyReadConcern() {
    if (!_isRecoveringCommit) {
        invariant(!_readConcernArgs.isEmpty());
    }

    if (_atClusterTime) {
        invariant(_atClusterTime->isSet());
    }
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

    _verifyReadConcern();

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

    _verifyReadConcern();

    auto sharedOptions = _atClusterTime
        ? SharedTransactionOptions{_txnNumber, _readConcernArgs, _atClusterTime->getTime()}
        : SharedTransactionOptions{_txnNumber, _readConcernArgs, boost::none};

    auto resultPair =
        _participants.try_emplace(shard.toString(),
                                  TransactionRouter::Participant(
                                      isFirstParticipant, _latestStmtId, std::move(sharedOptions)));

    return resultPair.first->second;
}

void TransactionRouter::_clearPendingParticipants() {
    for (auto&& it = _participants.begin(); it != _participants.end();) {
        auto participant = it++;
        if (participant->second.stmtIdCreatedAt == _latestStmtId) {
            _participants.erase(participant);
        }
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

bool TransactionRouter::_canContinueOnStaleShardOrDbError(StringData cmdName) const {
    // We can always retry on the first overall statement.
    if (_latestStmtId == _firstStmtId) {
        return true;
    }

    if (alwaysRetryableCmds.count(cmdName)) {
        return true;
    }

    return false;
}

void TransactionRouter::onStaleShardOrDbError(StringData cmdName, const Status& errorStatus) {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << _txnNumber << " was aborted on statement "
                          << _latestStmtId
                          << " due to an error from cluster data placement change: "
                          << errorStatus,
            _canContinueOnStaleShardOrDbError(cmdName));

    LOG(0) << _txnIdToString()
           << " Clearing pending participants after stale version error: " << errorStatus;

    // Remove participants created during the current statement so they are sent the correct options
    // if they are targeted again by the retry.
    _clearPendingParticipants();
}

void TransactionRouter::onViewResolutionError(const NamespaceString& nss) {
    // The router can always retry on a view resolution error.

    LOG(0) << _txnIdToString()
           << " Clearing pending participants after view resolution error on namespace: " << nss;

    // Requests against views are always routed to the primary shard for its database, but the retry
    // on the resolved namespace does not have to re-target the primary, so pending participants
    // should be cleared.
    _clearPendingParticipants();
}

bool TransactionRouter::_canContinueOnSnapshotError() const {
    return _atClusterTime && _atClusterTime->canChange(_latestStmtId);
}

void TransactionRouter::onSnapshotError(const Status& errorStatus) {
    uassert(ErrorCodes::NoSuchTransaction,
            str::stream() << "Transaction " << _txnNumber << " was aborted on statement "
                          << _latestStmtId
                          << " due to a non-retryable snapshot error: "
                          << errorStatus,
            _canContinueOnSnapshotError());

    LOG(0) << _txnIdToString() << " Clearing pending participants and resetting global snapshot "
                                  "timestamp after snapshot error: "
           << errorStatus << ", previous timestamp: " << _atClusterTime->getTime();

    // The transaction must be restarted on all participants because a new read timestamp will be
    // selected, so clear all pending participants. Snapshot errors are only retryable on the first
    // client statement, so all participants should be cleared, including the coordinator.
    _clearPendingParticipants();
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

    LOG(0) << _txnIdToString() << " Setting global snapshot timestamp to " << candidateTime
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
        if (!readConcernArgs.hasLevel()) {
            // Transactions started without a readConcern level will use snapshot as the default.
            uassertStatusOK(readConcernArgs.upconvertReadConcernLevelToSnapshot());
        } else {
            uassert(ErrorCodes::InvalidOptions,
                    "The first command in a transaction cannot specify a readConcern level other "
                    "than local, majority, or snapshot",
                    isReadConcernLevelAllowedInTransaction(readConcernArgs.getLevel()));
        }
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
    _initiatedTwoPhaseCommit = false;

    // TODO SERVER-37115: Parse statement ids from the client and remember the statement id of the
    // command that started the transaction, if one was included.
    _latestStmtId = kDefaultFirstStmtId;
    _firstStmtId = kDefaultFirstStmtId;

    if (_readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
        _atClusterTime.emplace();
    }

    LOG(0) << _txnIdToString() << " New transaction started";
}

const LogicalSessionId& TransactionRouter::_sessionId() const {
    const auto* owningSession = getTransactionRouter.owner(this);
    return owningSession->getSessionId();
}

Shard::CommandResponse TransactionRouter::_commitSingleShardTransaction(OperationContext* opCtx) {
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    const auto citer = _participants.cbegin();

    const auto& shardId(citer->first);
    const auto& participant = citer->second;

    auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

    LOG(0) << _txnIdToString()
           << " Committing single shard transaction, single participant: " << shardId;

    CommitTransaction commitCmd;
    commitCmd.setDbName(NamespaceString::kAdminDb);

    return uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        participant.attachTxnFieldsIfNeeded(
            commitCmd.toBSON(
                BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON())),
            false),
        Shard::RetryPolicy::kIdempotent));
}

Shard::CommandResponse TransactionRouter::_commitMultiShardTransaction(OperationContext* opCtx) {
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
        LOG(0) << "Sending coordinateCommit for transaction " << *opCtx->getTxnNumber()
               << " on session " << opCtx->getLogicalSessionId()->toBSON()
               << " to config server rather than actual coordinator because failpoint is active";

        coordinatorShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        if (!_initiatedTwoPhaseCommit) {
            // Send a fake transaction statement to the config server primary so that the config
            // server primary sets up state in memory to receive coordinateCommit.
            auto cmdResponse = coordinatorShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                "dummy",
                coordinatorIter->second.attachTxnFieldsIfNeeded(BSON("distinct"
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
                coordinatorIter->second.attachTxnFieldsIfNeeded(BSON("abortTransaction" << 1),
                                                                false),
                Shard::RetryPolicy::kIdempotent);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(cmdResponse));
        }
    }

    CoordinateCommitTransaction coordinateCommitCmd;
    coordinateCommitCmd.setDbName("admin");
    coordinateCommitCmd.setParticipants(participantList);

    _initiatedTwoPhaseCommit = true;

    LOG(0) << _txnIdToString()
           << " Committing multi shard transaction, coordinator: " << *_coordinatorId;

    return uassertStatusOK(coordinatorShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        coordinatorIter->second.attachTxnFieldsIfNeeded(
            coordinateCommitCmd.toBSON(
                BSON(WriteConcernOptions::kWriteConcernField << opCtx->getWriteConcern().toBSON())),
            false),
        Shard::RetryPolicy::kIdempotent));
}

Shard::CommandResponse TransactionRouter::commitTransaction(
    OperationContext* opCtx, const boost::optional<TxnRecoveryToken>& recoveryToken) {
    if (_participants.empty()) {
        uassert(50940, "cannot commit with no participants", recoveryToken);
        return _commitWithRecoveryToken(opCtx, *recoveryToken);
    }

    if (_participants.size() == 1) {
        return _commitSingleShardTransaction(opCtx);
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
        LOG(0) << _txnIdToString() << " Aborting transaction on " << _participants.size()
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

    if (_initiatedTwoPhaseCommit) {
        LOG(0) << _txnIdToString() << " Router not sending implicit abortTransaction because "
                                      "already initiated two phase commit for the transaction";
        return;
    }

    LOG(0) << _txnIdToString() << " Implicitly aborting transaction on " << _participants.size()
           << " shard(s) due to error: " << errorStatus;

    try {
        abortTransaction(opCtx, true /*isImplicit*/);
    } catch (...) {
        // Ignore any exceptions.
    }
}

std::string TransactionRouter::_txnIdToString() const {
    return str::stream() << _sessionId().getId() << ":" << _txnNumber;
}

void TransactionRouter::appendRecoveryToken(BSONObjBuilder* builder) const {
    if (!_coordinatorId)
        return;

    BSONObjBuilder recoveryTokenBuilder(
        builder->subobjStart(CommitTransaction::kRecoveryTokenFieldName));
    TxnRecoveryToken recoveryToken(*_coordinatorId);
    recoveryToken.serialize(&recoveryTokenBuilder);
    recoveryTokenBuilder.doneFast();
}

Shard::CommandResponse TransactionRouter::_commitWithRecoveryToken(
    OperationContext* opCtx, const TxnRecoveryToken& recoveryToken) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto& coordinatorId = recoveryToken.getShardId();

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

    _initiatedTwoPhaseCommit = true;

    auto coordinatorShard = uassertStatusOK(shardRegistry->getShard(opCtx, coordinatorId));
    return uassertStatusOK(coordinatorShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        coordinateCommitCmd,
        Shard::RetryPolicy::kIdempotent));
}

}  // namespace mongo
