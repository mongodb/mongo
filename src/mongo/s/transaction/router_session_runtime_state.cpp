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

#include "mongo/s/transaction/router_session_runtime_state.h"

#include "mongo/stdx/unordered_map.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const char kAutoCommitField[] = "autocommit";
const char kStartTransactionField[] = "startTransaction";

class RouterSessionCatalog {
public:
    std::shared_ptr<RouterSessionRuntimeState> checkoutSessionState(OperationContext* opCtx);
    void checkInSessionState(const LogicalSessionId& sessionId);

    static RouterSessionCatalog* get(ServiceContext* service);
    static RouterSessionCatalog* get(OperationContext* service);

private:
    stdx::mutex _mutex;
    stdx::unordered_map<LogicalSessionId,
                        std::shared_ptr<RouterSessionRuntimeState>,
                        LogicalSessionIdHash>
        _catalog;
};

const auto getRouterSessionCatalog = ServiceContext::declareDecoration<RouterSessionCatalog>();
const auto getRouterSessionRuntimeState =
    OperationContext::declareDecoration<std::shared_ptr<RouterSessionRuntimeState>>();

std::shared_ptr<RouterSessionRuntimeState> RouterSessionCatalog::checkoutSessionState(
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

    auto newRuntimeState = std::make_shared<RouterSessionRuntimeState>(*logicalSessionId);
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

}  // unnamed namespace

BSONObj TransactionParticipant::attachTxnFieldsIfNeeded(BSONObj cmd) {
    BSONObjBuilder newCmd(std::move(cmd));

    if (_state == State::kMustStart) {
        newCmd.append(kStartTransactionField, true);
    }

    newCmd.append(kAutoCommitField, false);

    // TODO: append readConcern

    return newCmd.obj();
}

TransactionParticipant::State TransactionParticipant::getState() {
    return _state;
}

void TransactionParticipant::markAsCommandSent() {
    if (_state == State::kMustStart) {
        _state = State::kStarted;
    }
}

RouterSessionRuntimeState* RouterSessionRuntimeState::get(OperationContext* opCtx) {
    auto& opCtxSession = getRouterSessionRuntimeState(opCtx);
    if (!opCtxSession) {
        return nullptr;
    }

    return opCtxSession.get();
}

RouterSessionRuntimeState::RouterSessionRuntimeState(LogicalSessionId sessionId)
    : _sessionId(std::move(sessionId)) {}

void RouterSessionRuntimeState::checkIn() {
    _isCheckedOut = false;
}

void RouterSessionRuntimeState::checkOut() {
    _isCheckedOut = true;
}

bool RouterSessionRuntimeState::isCheckedOut() {
    return _isCheckedOut;
}

TransactionParticipant& RouterSessionRuntimeState::getOrCreateParticipant(const ShardId& shard) {
    auto iter = _participants.find(shard.toString());

    if (iter != _participants.end()) {
        return iter->second;
    }

    auto resultPair = _participants.try_emplace(shard.toString(), TransactionParticipant());
    return resultPair.first->second;
}

const LogicalSessionId& RouterSessionRuntimeState::getSessionId() const {
    return _sessionId;
}

void RouterSessionRuntimeState::beginOrContinueTxn(TxnNumber txnNumber, bool startTransaction) {
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

    } else {
        // TODO: figure out what to do with recovery
        uassert(ErrorCodes::NoSuchTransaction,
                str::stream() << "cannot continue txnId " << _txnNumber << " for session "
                              << _sessionId
                              << " with txnId "
                              << txnNumber,
                txnNumber == _txnNumber);
    }

    if (_txnNumber == txnNumber) {
        return;
    }

    _txnNumber = txnNumber;
    _participants.clear();
}

ScopedRouterSession::ScopedRouterSession(OperationContext* opCtx) : _opCtx(opCtx) {
    auto& opCtxSession = getRouterSessionRuntimeState(opCtx);
    invariant(!opCtxSession);  // multiple sessions per OperationContext not supported

    auto logicalSessionId = opCtx->getLogicalSessionId();
    invariant(logicalSessionId);

    RouterSessionCatalog::get(opCtx)->checkoutSessionState(opCtx).swap(opCtxSession);
}

ScopedRouterSession::~ScopedRouterSession() {
    auto opCtxSession = RouterSessionRuntimeState::get(_opCtx);
    invariant(opCtxSession);
    RouterSessionCatalog::get(_opCtx)->checkInSessionState(opCtxSession->getSessionId());
}

}  // namespace mongo
