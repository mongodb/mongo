/**
 *    Copyright (C) 2018 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/transaction/transaction_router.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction/at_cluster_time_util.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const char kAutoCommitField[] = "autocommit";
const char kCoordinatorField[] = "coordinator";
const char kStartTransactionField[] = "startTransaction";

class RouterSessionCatalog {
public:
    std::shared_ptr<TransactionRouter> checkoutSessionState(OperationContext* opCtx);
    void checkInSessionState(const LogicalSessionId& sessionId);

    static RouterSessionCatalog* get(ServiceContext* service);
    static RouterSessionCatalog* get(OperationContext* service);

private:
    stdx::mutex _mutex;
    stdx::unordered_map<LogicalSessionId, std::shared_ptr<TransactionRouter>, LogicalSessionIdHash>
        _catalog;
};

const auto getRouterSessionCatalog = ServiceContext::declareDecoration<RouterSessionCatalog>();
const auto getRouterSessionRuntimeState =
    OperationContext::declareDecoration<std::shared_ptr<TransactionRouter>>();

std::shared_ptr<TransactionRouter> RouterSessionCatalog::checkoutSessionState(
    OperationContext* opCtx) {
    auto logicalSessionId = opCtx->getLogicalSessionId();
    invariant(logicalSessionId);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto iter = _catalog.find(*logicalSessionId);
    if (iter != _catalog.end()) {
        uassert(50866,
                str::stream() << "cannot checkout " << *logicalSessionId
                              << ", session already in use",
                !iter->second->isCheckedOut());
        iter->second->checkOut();
        return iter->second;
    }

    auto newRuntimeState = std::make_shared<TransactionRouter>(*logicalSessionId);
    newRuntimeState->checkOut();
    _catalog.insert(std::make_pair(*logicalSessionId, newRuntimeState));
    return newRuntimeState;
}

void RouterSessionCatalog::checkInSessionState(const LogicalSessionId& sessionId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto iter = _catalog.find(sessionId);
    invariant(iter != _catalog.end());
    invariant(iter->second->isCheckedOut());
    iter->second->checkIn();
}

RouterSessionCatalog* RouterSessionCatalog::get(ServiceContext* service) {
    auto& catalog = getRouterSessionCatalog(service);
    return &catalog;
}

RouterSessionCatalog* RouterSessionCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

bool isTransactionCommand(const BSONObj& cmd) {
    auto cmdName = cmd.firstElement().fieldNameStringData();
    return cmdName == "abortTransaction" || cmdName == "commitTransaction" ||
        cmdName == "prepareTransaction";
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
        return atClusterTime
            ? at_cluster_time_util::appendAtClusterTime(std::move(cmd), *atClusterTime)
            : cmd;
    }

    BSONObjBuilder bob(std::move(cmd));
    readConcernArgs.appendInfo(&bob);

    return atClusterTime
        ? at_cluster_time_util::appendAtClusterTime(bob.asTempObj(), *atClusterTime)
        : bob.obj();
}

BSONObjBuilder appendFieldsForStartTransaction(BSONObj cmd,
                                               repl::ReadConcernArgs readConcernArgs,
                                               boost::optional<LogicalTime> atClusterTime) {
    auto cmdWithReadConcern =
        appendReadConcernForTxn(std::move(cmd), readConcernArgs, atClusterTime);
    BSONObjBuilder bob(std::move(cmdWithReadConcern));
    bob.append(kStartTransactionField, true);

    return bob;
}

}  // unnamed namespace

TransactionRouter::Participant::Participant(bool isCoordinator,
                                            SharedTransactionOptions sharedOptions)
    : _isCoordinator(isCoordinator), _sharedOptions(sharedOptions) {}

BSONObj TransactionRouter::Participant::attachTxnFieldsIfNeeded(BSONObj cmd) {
    auto isTxnCmd = isTransactionCommand(cmd);  // check first before moving cmd.

    // The first command sent to a participant must start a transaction, unless it is a transaction
    // command, which don't support the options that start transactions, i.e. starTransaction and
    // readConcern. Otherwise the command must not have a read concern.
    bool mustStartTransaction = _state == State::kMustStart && !isTxnCmd;

    if (!mustStartTransaction) {
        dassert(!cmd.hasField(repl::ReadConcernArgs::kReadConcernFieldName));
    }

    BSONObjBuilder newCmd = mustStartTransaction
        ? appendFieldsForStartTransaction(
              std::move(cmd), _sharedOptions.readConcernArgs, _sharedOptions.atClusterTime)
        : BSONObjBuilder(std::move(cmd));

    if (_isCoordinator) {
        newCmd.append(kCoordinatorField, true);
    }

    newCmd.append(kAutoCommitField, false);

    if (!newCmd.hasField(OperationSessionInfo::kTxnNumberFieldName)) {
        newCmd.append(OperationSessionInfo::kTxnNumberFieldName, _sharedOptions.txnNumber);
    } else {
        auto osi =
            OperationSessionInfoFromClient::parse("OperationSessionInfo"_sd, newCmd.asTempObj());
        invariant(_sharedOptions.txnNumber == *osi.getTxnNumber());
    }

    return newCmd.obj();
}

bool TransactionRouter::Participant::isCoordinator() const {
    return _isCoordinator;
}

bool TransactionRouter::Participant::mustStartTransaction() const {
    return _state == State::kMustStart;
}

void TransactionRouter::Participant::markAsCommandSent() {
    if (_state == State::kMustStart) {
        _state = State::kStarted;
    }
}

TransactionRouter* TransactionRouter::get(OperationContext* opCtx) {
    auto& opCtxSession = getRouterSessionRuntimeState(opCtx);
    if (!opCtxSession) {
        return nullptr;
    }

    return opCtxSession.get();
}

TransactionRouter::TransactionRouter(LogicalSessionId sessionId)
    : _sessionId(std::move(sessionId)) {}

void TransactionRouter::checkIn() {
    _isCheckedOut = false;
}

void TransactionRouter::checkOut() {
    _isCheckedOut = true;
}

bool TransactionRouter::isCheckedOut() {
    return _isCheckedOut;
}

boost::optional<ShardId> TransactionRouter::getCoordinatorId() const {
    return _coordinatorId;
}

TransactionRouter::Participant& TransactionRouter::getOrCreateParticipant(const ShardId& shard) {
    // Remove the shard from the abort list if it is present.
    _orphanedParticipants.erase(shard.toString());

    auto iter = _participants.find(shard.toString());
    if (iter != _participants.end()) {
        // TODO SERVER-36589: Once mongos aborts transactions by only sending abortTransaction to
        // shards that have been successfully contacted we should be able to add an invariant here
        // to ensure the atClusterTime on the participant matches that on the transaction router.
        return iter->second;
    }

    // The first participant is chosen as the coordinator.
    auto isFirstParticipant = _participants.empty();
    if (isFirstParticipant) {
        invariant(!_coordinatorId);
        _coordinatorId = shard.toString();
    }

    // The transaction must have been started with a readConcern.
    invariant(!_readConcernArgs.isEmpty());

    // TODO SERVER-36589: Once mongos aborts transactions by only sending abortTransaction to shards
    // that have been successfully contacted we should be able to add an invariant here to ensure
    // that an atClusterTime has been chosen if the read concern level is snapshot.

    auto resultPair = _participants.try_emplace(
        shard.toString(),
        TransactionRouter::Participant(
            isFirstParticipant,
            SharedTransactionOptions{_txnNumber, _readConcernArgs, _atClusterTime}));

    return resultPair.first->second;
}

const LogicalSessionId& TransactionRouter::getSessionId() const {
    return _sessionId;
}

bool TransactionRouter::canContinueOnSnapshotError() const {
    return _latestStmtId == _firstStmtId;
}

void TransactionRouter::onSnapshotError() {
    invariant(canContinueOnSnapshotError());

    // Add each participant to the orphaned list because the retry attempt isn't guaranteed to
    // re-target it.
    for (const auto& participant : _participants) {
        _orphanedParticipants.try_emplace(participant.first);
    }

    // New transactions must be started on each contacted participant since the retry will select a
    // new read timestamp.
    _participants.clear();
    _coordinatorId.reset();

    // Reset the global snapshot timestamp so the retry will select a new one.
    _atClusterTime.reset();
}

void TransactionRouter::computeAtClusterTime(OperationContext* opCtx,
                                             bool mustRunOnAll,
                                             const std::set<ShardId>& shardIds,
                                             const NamespaceString& nss,
                                             const BSONObj query,
                                             const BSONObj collation) {
    // TODO SERVER-36688: We should also return immediately if the read concern
    // is not snapshot.
    if (_atClusterTime) {
        return;
    }

    // atClusterTime could be none if the the read concern is not snapshot.
    auto atClusterTime = at_cluster_time_util::computeAtClusterTime(
        opCtx, mustRunOnAll, shardIds, nss, query, collation);
    // TODO SERVER-36688: atClusterTime should never be none once we add the check above.
    invariant(!atClusterTime || *atClusterTime != LogicalTime::kUninitialized);
    if (atClusterTime) {
        _atClusterTime = *atClusterTime;
    }
}

void TransactionRouter::computeAtClusterTimeForOneShard(OperationContext* opCtx,
                                                        const ShardId& shardId) {
    // TODO SERVER-36688: We should also return immediately if the read concern
    // is not snapshot.
    if (_atClusterTime) {
        return;
    }

    // atClusterTime could be none if the the read concern is not snapshot.
    auto atClusterTime = at_cluster_time_util::computeAtClusterTimeForOneShard(opCtx, shardId);
    // TODO SERVER-36688: atClusterTime should never be none once we add the check above.
    invariant(!atClusterTime || *atClusterTime != LogicalTime::kUninitialized);
    if (atClusterTime) {
        _atClusterTime = *atClusterTime;
    }
}

void TransactionRouter::setAtClusterTimeToLatestTime(OperationContext* opCtx) {
    if (_atClusterTime ||
        _readConcernArgs.getLevel() != repl::ReadConcernLevel::kSnapshotReadConcern) {
        return;
    }

    auto atClusterTime = LogicalClock::get(opCtx)->getClusterTime();

    // If the user passed afterClusterTime, atClusterTime for the transaction must be selected so it
    // is at least equal to or greater than it.
    auto afterClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAfterClusterTime();
    if (afterClusterTime && *afterClusterTime > atClusterTime) {
        atClusterTime = *afterClusterTime;
    }

    _atClusterTime = atClusterTime;
}

void TransactionRouter::beginOrContinueTxn(OperationContext* opCtx,
                                           TxnNumber txnNumber,
                                           bool startTransaction) {
    invariant(_isCheckedOut);

    if (startTransaction) {
        // TODO: do we need more robust checking? Like, did we actually sent start to the
        // participants?
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "txnNumber " << _txnNumber << " for session " << _sessionId
                              << " already started",
                txnNumber != _txnNumber);

        uassert(ErrorCodes::TransactionTooOld,
                str::stream() << "txnNumber " << txnNumber << " is less than last txnNumber "
                              << _txnNumber
                              << " seen in session "
                              << _sessionId,
                txnNumber > _txnNumber);

        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        if (!readConcernArgs.hasLevel()) {
            // Transactions started without a readConcern level will use snapshot as the default.
            uassertStatusOK(readConcernArgs.upconvertReadConcernLevelToSnapshot());
        } else {
            uassert(ErrorCodes::InvalidOptions,
                    "The first command in a transaction cannot specify a readConcern level other "
                    "than snapshot",
                    readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern);
        }
        _readConcernArgs = readConcernArgs;
    } else {
        // TODO: figure out what to do with recovery
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "cannot continue txnId " << _txnNumber << " for session "
                              << _sessionId
                              << " with txnId "
                              << txnNumber,
                txnNumber == _txnNumber);

        uassert(ErrorCodes::InvalidOptions,
                "Only the first command in a transaction may specify a readConcern",
                repl::ReadConcernArgs::get(opCtx).isEmpty());
    }

    if (_txnNumber == txnNumber) {
        ++_latestStmtId;
        return;
    }

    _txnNumber = txnNumber;
    _participants.clear();
    _coordinatorId.reset();
    _atClusterTime.reset();

    // TODO SERVER-XXXXX: Parse statement ids from the client and remember the statement id of the
    // command that started the transaction, if one was included.
    _latestStmtId = kDefaultFirstStmtId;
    _firstStmtId = kDefaultFirstStmtId;
}


Shard::CommandResponse TransactionRouter::_commitSingleShardTransaction(OperationContext* opCtx) {
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    auto citer = _participants.cbegin();
    ShardId shardId(citer->first);
    auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

    CommitTransaction commitCmd;
    commitCmd.setDbName("admin");
    return uassertStatusOK(
        shard->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                "admin",
                                                commitCmd.toBSON(opCtx->getWriteConcern().toBSON()),
                                                Shard::RetryPolicy::kIdempotent));
}

Shard::CommandResponse TransactionRouter::_commitMultiShardTransaction(OperationContext* opCtx) {
    invariant(_coordinatorId);

    auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    PrepareTransaction prepareCmd;
    prepareCmd.setDbName("admin");
    prepareCmd.setCoordinatorId(*_coordinatorId);

    auto prepareCmdObj = prepareCmd.toBSON(
        BSON(WriteConcernOptions::kWriteConcernField << WriteConcernOptions::Majority));

    std::vector<CommitParticipant> participantList;
    for (const auto& participantEntry : _participants) {
        ShardId shardId(participantEntry.first);

        CommitParticipant participant;
        participant.setShardId(shardId);
        participantList.push_back(std::move(participant));

        if (participantEntry.second.isCoordinator()) {
            // coordinateCommit is sent to participant that is also a coordinator.
            invariant(shardId == *_coordinatorId);
            continue;
        }

        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        shard->runFireAndForgetCommand(
            opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly}, "admin", prepareCmdObj);
    }

    auto coordinatorShard = uassertStatusOK(shardRegistry->getShard(opCtx, *_coordinatorId));

    CoordinateCommitTransaction coordinateCommitCmd;
    coordinateCommitCmd.setDbName("admin");
    coordinateCommitCmd.setParticipants(participantList);

    return uassertStatusOK(coordinatorShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        coordinateCommitCmd.toBSON(opCtx->getWriteConcern().toBSON()),
        Shard::RetryPolicy::kIdempotent));
}

Shard::CommandResponse TransactionRouter::commitTransaction(OperationContext* opCtx) {
    uassert(50940, "cannot commit with no participants", !_participants.empty());

    if (_participants.size() == 1) {
        return _commitSingleShardTransaction(opCtx);
    }

    return _commitMultiShardTransaction(opCtx);
}

ScopedRouterSession::ScopedRouterSession(OperationContext* opCtx) : _opCtx(opCtx) {
    auto& opCtxSession = getRouterSessionRuntimeState(opCtx);
    invariant(!opCtxSession);  // multiple sessions per OperationContext not supported

    auto logicalSessionId = opCtx->getLogicalSessionId();
    invariant(logicalSessionId);

    RouterSessionCatalog::get(opCtx)->checkoutSessionState(opCtx).swap(opCtxSession);
}

ScopedRouterSession::~ScopedRouterSession() {
    auto opCtxSession = TransactionRouter::get(_opCtx);
    invariant(opCtxSession);
    RouterSessionCatalog::get(_opCtx)->checkInSessionState(opCtxSession->getSessionId());
}

}  // namespace mongo
