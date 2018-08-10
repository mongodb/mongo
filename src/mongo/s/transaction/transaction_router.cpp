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

#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/repl/read_concern_args.h"
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
                                            TxnNumber txnNumber,
                                            repl::ReadConcernArgs readConcernArgs)
    : _isCoordinator(isCoordinator), _txnNumber(txnNumber), _readConcernArgs(readConcernArgs) {}

BSONObj TransactionRouter::Participant::attachTxnFieldsIfNeeded(BSONObj cmd) {
    auto isTxnCmd = isTransactionCommand(cmd);  // check first before moving cmd.

    BSONObjBuilder newCmd = (_state == State::kMustStart && !isTxnCmd)
        ? appendFieldsForStartTransaction(std::move(cmd), _readConcernArgs, _atClusterTime)
        : BSONObjBuilder(std::move(cmd));

    if (_isCoordinator) {
        newCmd.append(kCoordinatorField, true);
    }

    newCmd.append(kAutoCommitField, false);

    if (!newCmd.hasField(OperationSessionInfo::kTxnNumberFieldName)) {
        newCmd.append(OperationSessionInfo::kTxnNumberFieldName, _txnNumber);
    } else {
        auto osi =
            OperationSessionInfoFromClient::parse("OperationSessionInfo"_sd, newCmd.asTempObj());
        invariant(_txnNumber == *osi.getTxnNumber());
    }

    return newCmd.obj();
}

TransactionRouter::Participant::State TransactionRouter::Participant::getState() {
    return _state;
}

bool TransactionRouter::Participant::isCoordinator() {
    return _isCoordinator;
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

void TransactionRouter::Participant::setAtClusterTime(const LogicalTime atClusterTime) {
    _atClusterTime = atClusterTime;
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
    auto iter = _participants.find(shard.toString());

    if (iter != _participants.end()) {
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

    auto participant =
        TransactionRouter::Participant(isFirstParticipant, _txnNumber, _readConcernArgs);
    // TODO SERVER-36557: Every command that starts a cross-shard transaction should
    // compute atClusterTime with snapshot read concern. Hence, we should be able to
    // add an invariant here to ensure that atClusterTime is not none.
    if (_atClusterTime) {
        participant.setAtClusterTime(*_atClusterTime);
    }

    auto resultPair = _participants.try_emplace(shard.toString(), std::move(participant));

    return resultPair.first->second;
}

const LogicalSessionId& TransactionRouter::getSessionId() const {
    return _sessionId;
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
        return;
    }

    _txnNumber = txnNumber;
    _participants.clear();
    _coordinatorId.reset();
    _atClusterTime.reset();
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
