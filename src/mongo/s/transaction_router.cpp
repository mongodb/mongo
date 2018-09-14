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

#include "mongo/s/transaction_router.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/at_cluster_time_util.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const char kCoordinatorField[] = "coordinator";

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
        // There may be no read concern level if the user only specified afterClusterTime and the
        // transaction provided the default level.
        //
        // TODO SERVER-37237: Once read concern handling has been consolidated on mongos, this
        // assertion can probably be simplified or removed.
        dassert(existingReadConcernArgs.getLevel() == readConcernArgs.getLevel() ||
                !existingReadConcernArgs.hasLevel());

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

}  // unnamed namespace

TransactionRouter::Participant::Participant(bool isCoordinator,
                                            StmtId stmtIdCreatedAt,
                                            SharedTransactionOptions sharedOptions)
    : _isCoordinator(isCoordinator),
      _stmtIdCreatedAt(stmtIdCreatedAt),
      _sharedOptions(sharedOptions) {}

BSONObj TransactionRouter::Participant::attachTxnFieldsIfNeeded(
    BSONObj cmd, bool isFirstStatementInThisParticipant) const {
    // Perform checks first before calling std::move on cmd.
    auto isTxnCmd = isTransactionCommand(cmd);

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
    bool mustStartTransaction = isFirstStatementInThisParticipant && !isTxnCmd;

    if (!mustStartTransaction) {
        dassert(!cmd.hasField(repl::ReadConcernArgs::kReadConcernFieldName));
    }

    BSONObjBuilder newCmd = mustStartTransaction
        ? appendFieldsForStartTransaction(std::move(cmd),
                                          _sharedOptions.readConcernArgs,
                                          _sharedOptions.atClusterTime,
                                          !hasStartTxn)
        : BSONObjBuilder(std::move(cmd));

    if (_isCoordinator) {
        newCmd.append(kCoordinatorField, true);
    }

    if (!hasAutoCommit) {
        newCmd.append(OperationSessionInfoFromClient::kAutocommitFieldName, false);
    }

    if (!hasTxnNum) {
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

StmtId TransactionRouter::Participant::getStmtIdCreatedAt() const {
    return _stmtIdCreatedAt;
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

BSONObj TransactionRouter::attachTxnFieldsIfNeeded(const ShardId& shardId, const BSONObj& cmdObj) {
    if (auto txnPart = getParticipant(shardId)) {
        return txnPart->attachTxnFieldsIfNeeded(cmdObj, false);
    }

    auto txnPart = _createParticipant(shardId);
    return txnPart.attachTxnFieldsIfNeeded(cmdObj, true);
}

boost::optional<TransactionRouter::Participant&> TransactionRouter::getParticipant(
    const ShardId& shard) {
    auto iter = _participants.find(shard.toString());
    if (iter == _participants.end()) {
        return boost::none;
    }

    // TODO SERVER-37223: Once mongos aborts transactions by only sending abortTransaction to
    // shards that have been successfully contacted we should be able to add an invariant here
    // to ensure the atClusterTime on the participant matches that on the transaction router.
    return iter->second;
}

TransactionRouter::Participant& TransactionRouter::_createParticipant(const ShardId& shard) {
    // The first participant is chosen as the coordinator.
    auto isFirstParticipant = _participants.empty();
    if (isFirstParticipant) {
        invariant(!_coordinatorId);
        _coordinatorId = shard.toString();
    }

    // The transaction must have been started with a readConcern.
    invariant(!_readConcernArgs.isEmpty());

    // TODO SERVER-37223: Once mongos aborts transactions by only sending abortTransaction to shards
    // that have been successfully contacted we should be able to add an invariant here to ensure
    // that an atClusterTime has been chosen if the read concern level is snapshot.

    auto resultPair = _participants.try_emplace(
        shard.toString(),
        TransactionRouter::Participant(
            isFirstParticipant,
            _latestStmtId,
            SharedTransactionOptions{_txnNumber, _readConcernArgs, _atClusterTime}));

    return resultPair.first->second;
}

const LogicalSessionId& TransactionRouter::getSessionId() const {
    return _sessionId;
}

void TransactionRouter::_clearPendingParticipants() {
    for (auto&& it = _participants.begin(); it != _participants.end();) {
        auto participant = it++;
        if (participant->second.getStmtIdCreatedAt() == _latestStmtId) {
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

void TransactionRouter::onStaleShardOrDbError(StringData cmdName) {
    // TODO SERVER-37210: Implicitly abort the entire transaction if this uassert throws.
    uassert(ErrorCodes::NoSuchTransaction,
            "Transaction was aborted due to cluster data placement change",
            _canContinueOnStaleShardOrDbError(cmdName));

    // Remove participants created during the current statement so they are sent the correct options
    // if they are targeted again by the retry.
    _clearPendingParticipants();
}

void TransactionRouter::onViewResolutionError() {
    // The router can always retry on a view resolution error.

    // Requests against views are always routed to the primary shard for its database, but the retry
    // on the resolved namespace does not have to re-target the primary, so pending participants
    // should be cleared.
    _clearPendingParticipants();
}

bool TransactionRouter::_canContinueOnSnapshotError() const {
    return _latestStmtId == _firstStmtId;
}

void TransactionRouter::onSnapshotError() {
    // TODO SERVER-37210: Implicitly abort the entire transaction if this uassert throws.
    uassert(ErrorCodes::NoSuchTransaction,
            "Transaction was aborted due to snapshot error on subsequent transaction statement",
            _canContinueOnSnapshotError());

    // The transaction must be restarted on all participants because a new read timestamp will be
    // selected, so clear all pending participants. Snapshot errors are only retryable on the first
    // client statement, so all participants should be cleared, including the coordinator.
    _clearPendingParticipants();
    invariant(_participants.empty());
    invariant(!_coordinatorId);

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

    // TODO SERVER-37115: Parse statement ids from the client and remember the statement id of the
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

    const auto& participant = citer->second;
    return uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        participant.attachTxnFieldsIfNeeded(commitCmd.toBSON(opCtx->getWriteConcern().toBSON()),
                                            false),
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

        CommitParticipant commitParticipant;
        commitParticipant.setShardId(shardId);
        participantList.push_back(std::move(commitParticipant));

        if (participantEntry.second.isCoordinator()) {
            // coordinateCommit is sent to participant that is also a coordinator.
            invariant(shardId == *_coordinatorId);
            continue;
        }

        const auto& participant = participantEntry.second;
        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        shard->runFireAndForgetCommand(opCtx,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       "admin",
                                       participant.attachTxnFieldsIfNeeded(prepareCmdObj, false));
    }

    auto coordinatorShard = uassertStatusOK(shardRegistry->getShard(opCtx, *_coordinatorId));

    CoordinateCommitTransaction coordinateCommitCmd;
    coordinateCommitCmd.setDbName("admin");
    coordinateCommitCmd.setParticipants(participantList);

    auto coordinatorIter = _participants.find(*_coordinatorId);
    invariant(coordinatorIter != _participants.end());

    return uassertStatusOK(coordinatorShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        coordinatorIter->second.attachTxnFieldsIfNeeded(
            coordinateCommitCmd.toBSON(opCtx->getWriteConcern().toBSON()), false),
        Shard::RetryPolicy::kIdempotent));
}

Shard::CommandResponse TransactionRouter::commitTransaction(OperationContext* opCtx) {
    uassert(50940, "cannot commit with no participants", !_participants.empty());

    if (_participants.size() == 1) {
        return _commitSingleShardTransaction(opCtx);
    }

    return _commitMultiShardTransaction(opCtx);
}

std::vector<AsyncRequestsSender::Response> TransactionRouter::abortTransaction(
    OperationContext* opCtx) {
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

    return gatherResponses(opCtx,
                           NamespaceString::kAdminDb,
                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                           Shard::RetryPolicy::kIdempotent,
                           abortRequests);
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
