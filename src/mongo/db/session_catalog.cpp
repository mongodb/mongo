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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/session_catalog.h"

#include <memory>

#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

const auto sessionTransactionTableDecoration = ServiceContext::declareDecoration<SessionCatalog>();

const auto operationSessionDecoration =
    OperationContext::declareDecoration<boost::optional<SessionCatalog::ScopedCheckedOutSession>>();

MONGO_FAIL_POINT_DEFINE(hangAfterIncrementingNumWaitingToCheckOut);

}  // namespace

SessionCatalog::~SessionCatalog() {
    stdx::lock_guard<Latch> lg(_mutex);
    for (const auto& [_, sri] : _sessions) {
        ObservableSession osession(lg, sri.get(), &sri->parentSession);
        invariant(!osession.hasCurrentOperation());
        invariant(!osession._killed());
    }
}

void SessionCatalog::reset_forTest() {
    stdx::lock_guard<Latch> lg(_mutex);
    _sessions.clear();
}

SessionCatalog* SessionCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

SessionCatalog* SessionCatalog::get(ServiceContext* service) {
    auto& sessionTransactionTable = sessionTransactionTableDecoration(service);
    return &sessionTransactionTable;
}

SessionCatalog::ScopedCheckedOutSession SessionCatalog::_checkOutSessionInner(
    OperationContext* opCtx, const LogicalSessionId& lsid, boost::optional<KillToken> killToken) {
    if (killToken) {
        invariant(killToken->lsidToKill == lsid);
    } else {
        invariant(opCtx->getLogicalSessionId() == lsid);
    }

    stdx::unique_lock<Latch> ul(_mutex);

    auto sri = _getOrCreateSessionRuntimeInfo(ul, lsid);
    auto session = sri->getSession(lsid);
    invariant(session);

    if (killToken) {
        invariant(ObservableSession(ul, sri, session)._killed());
    }

    // Wait until the session is no longer checked out and until the previously scheduled kill has
    // completed.
    ++session->_numWaitingToCheckOut;
    ON_BLOCK_EXIT([&] { --session->_numWaitingToCheckOut; });

    if (MONGO_unlikely(hangAfterIncrementingNumWaitingToCheckOut.shouldFail())) {
        ul.unlock();
        hangAfterIncrementingNumWaitingToCheckOut.pauseWhileSet(opCtx);
        ul.lock();
    }

    opCtx->waitForConditionOrInterrupt(
        sri->availableCondVar, ul, [&ul, &sri, &session, forKill = killToken.has_value()]() {
            ObservableSession osession(ul, sri, session);
            return osession._isAvailableForCheckOut(forKill);
        });

    sri->checkoutOpCtx = opCtx;
    sri->lastCheckout = Date_t::now();

    return ScopedCheckedOutSession(*this, std::move(sri), session, std::move(killToken));
}

SessionCatalog::ScopedCheckedOutSession SessionCatalog::_checkOutSession(OperationContext* opCtx) {
    // This method is not supposed to be called with an already checked-out session due to risk of
    // deadlock
    invariant(opCtx->getLogicalSessionId());
    invariant(!operationSessionDecoration(opCtx));
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(!opCtx->lockState()->isLocked());

    auto lsid = *opCtx->getLogicalSessionId();
    return _checkOutSessionInner(opCtx, lsid, boost::none /* killToken */);
}

SessionCatalog::SessionToKill SessionCatalog::checkOutSessionForKill(OperationContext* opCtx,
                                                                     KillToken killToken) {
    // This method is not supposed to be called with an already checked-out session due to risk of
    // deadlock
    invariant(!operationSessionDecoration(opCtx));
    invariant(!opCtx->getTxnNumber());

    auto lsid = killToken.lsidToKill;
    return SessionToKill(_checkOutSessionInner(opCtx, lsid, std::move(killToken)));
}

void SessionCatalog::scanSession(const LogicalSessionId& lsid,
                                 const ScanSessionsCallbackFn& workerFn) {
    stdx::lock_guard<Latch> lg(_mutex);

    if (auto sri = _getSessionRuntimeInfo(lg, lsid)) {
        auto session = sri->getSession(lsid);
        invariant(session);

        ObservableSession osession(lg, sri, session);
        workerFn(osession);
        invariant(!osession._markedForReap, "Cannot reap a session via 'scanSession'");
    }
}

void SessionCatalog::scanSessions(const SessionKiller::Matcher& matcher,
                                  const ScanSessionsCallbackFn& workerFn) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2_DEBUG(21976,
                2,
                "Scanning {sessionCount} sessions",
                "Scanning sessions",
                "sessionCount"_attr = _sessions.size());

    for (auto& [parentLsid, sri] : _sessions) {
        if (matcher.match(parentLsid)) {
            ObservableSession osession(lg, sri.get(), &sri->parentSession);
            workerFn(osession);
            invariant(!osession._markedForReap, "Cannot reap a session via 'scanSessions'");
        }

        for (auto& [childLsid, session] : sri->childSessions) {
            if (matcher.match(childLsid)) {
                ObservableSession osession(lg, sri.get(), &session);
                workerFn(osession);
                invariant(!osession._markedForReap, "Cannot reap a session via 'scanSessions'");
            }
        }
    }
}

LogicalSessionIdSet SessionCatalog::scanSessionsForReap(
    const LogicalSessionId& parentLsid,
    const ScanSessionsCallbackFn& parentSessionWorkerFn,
    const ScanSessionsCallbackFn& childSessionWorkerFn) {
    invariant(!getParentSessionId(parentLsid));

    std::unique_ptr<SessionRuntimeInfo> sriToReap;
    {
        stdx::lock_guard<Latch> lg(_mutex);

        auto sriIt = _sessions.find(parentLsid);
        // The reaper should never try to reap a non-existent session id.
        invariant(sriIt != _sessions.end());
        auto sri = sriIt->second.get();

        LogicalSessionIdSet remainingSessions;
        bool shouldReapRemaining = true;

        {
            ObservableSession osession(lg, sri, &sri->parentSession);
            parentSessionWorkerFn(osession);

            remainingSessions.insert(osession.getSessionId());
            shouldReapRemaining = osession._shouldBeReaped();
        }

        {
            auto childSessionIt = sri->childSessions.begin();
            while (childSessionIt != sri->childSessions.end()) {
                ObservableSession osession(lg, sri, &childSessionIt->second);
                childSessionWorkerFn(osession);

                if (osession._shouldBeReaped() &&
                    (osession._reapMode == ObservableSession::ReapMode::kExclusive)) {
                    sri->childSessions.erase(childSessionIt++);
                    continue;
                }

                remainingSessions.insert(osession.getSessionId());
                shouldReapRemaining &= osession._shouldBeReaped();
                ++childSessionIt;
            }
        }

        if (shouldReapRemaining) {
            sriToReap = std::move(sriIt->second);
            _sessions.erase(sriIt);
            remainingSessions.clear();
        }

        return remainingSessions;
    }
}

SessionCatalog::KillToken SessionCatalog::killSession(const LogicalSessionId& lsid) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto sri = _getSessionRuntimeInfo(lg, lsid);
    uassert(ErrorCodes::NoSuchSession, "Session not found", sri);
    return ObservableSession(lg, sri, &sri->parentSession).kill();
}

size_t SessionCatalog::size() const {
    stdx::lock_guard<Latch> lg(_mutex);
    return _sessions.size();
}

void SessionCatalog::createSessionIfDoesNotExist(const LogicalSessionId& lsid) {
    stdx::lock_guard<Latch> lg(_mutex);
    _getOrCreateSessionRuntimeInfo(lg, lsid);
}

SessionCatalog::SessionRuntimeInfo* SessionCatalog::_getSessionRuntimeInfo(
    WithLock, const LogicalSessionId& lsid) {
    auto parentLsid = castToParentSessionId(lsid);
    auto sriIt = _sessions.find(parentLsid);

    if (sriIt == _sessions.end()) {
        return nullptr;
    }

    auto sri = sriIt->second.get();
    auto session = sri->getSession(lsid);

    if (session) {
        return sri;
    }

    return nullptr;
}

SessionCatalog::SessionRuntimeInfo* SessionCatalog::_getOrCreateSessionRuntimeInfo(
    WithLock lk, const LogicalSessionId& lsid) {
    if (auto sri = _getSessionRuntimeInfo(lk, lsid)) {
        return sri;
    }

    auto parentLsid = castToParentSessionId(lsid);
    auto sriIt =
        _sessions.emplace(parentLsid, std::make_unique<SessionRuntimeInfo>(parentLsid)).first;
    auto sri = sriIt->second.get();

    if (getParentSessionId(lsid)) {
        auto [childSessionIt, inserted] = sri->childSessions.try_emplace(lsid, lsid);
        // Insert should always succeed since the session did not exist prior to this.
        invariant(inserted);

        auto& childSession = childSessionIt->second;
        childSession._parentSession = &sri->parentSession;
    }

    return sri;
}

void SessionCatalog::_releaseSession(SessionRuntimeInfo* sri,
                                     Session* session,
                                     boost::optional<KillToken> killToken) {
    stdx::lock_guard<Latch> lg(_mutex);

    // Make sure we have exactly the same session on the map and that it is still associated with an
    // operation context (meaning checked-out)
    invariant(_sessions[sri->parentSession.getSessionId()].get() == sri);
    invariant(sri->checkoutOpCtx);
    if (killToken) {
        invariant(killToken->lsidToKill == session->getSessionId());
    }

    sri->checkoutOpCtx = nullptr;
    sri->availableCondVar.notify_all();

    if (killToken) {
        invariant(sri->killsRequested > 0);
        --sri->killsRequested;
    }
}

Session* SessionCatalog::SessionRuntimeInfo::getSession(const LogicalSessionId& lsid) {
    if (lsid == parentSession._sessionId) {
        return &parentSession;
    }

    invariant(getParentSessionId(lsid) == parentSession._sessionId);
    auto it = childSessions.find(lsid);
    if (it == childSessions.end()) {
        return nullptr;
    }
    return &it->second;
}

SessionCatalog::KillToken ObservableSession::kill(ErrorCodes::Error reason) const {
    const bool firstKiller = (0 == _sri->killsRequested);
    ++_sri->killsRequested;

    // For currently checked-out sessions, interrupt the operation context so that the current owner
    // can release the session
    if (firstKiller && hasCurrentOperation()) {
        invariant(_clientLock.owns_lock());
        const auto serviceContext = _sri->checkoutOpCtx->getServiceContext();
        serviceContext->killOperation(_clientLock, _sri->checkoutOpCtx, reason);
    }

    return SessionCatalog::KillToken(getSessionId());
}

void ObservableSession::markForReap(ReapMode reapMode) {
    if (!getParentSessionId(getSessionId())) {
        // By design, parent sessions are only safe to be reaped if all of their child sessions are.
        invariant(reapMode == ReapMode::kNonExclusive);
    }
    _markedForReap = true;
    _reapMode.emplace(reapMode);
}

bool ObservableSession::_shouldBeReaped() const {
    bool isCheckedOut = [&] {
        if (_sri->checkoutOpCtx) {
            return _sri->checkoutOpCtx->getLogicalSessionId() == getSessionId();
        }
        return false;
    }();
    return _markedForReap && !isCheckedOut && !get()->_numWaitingToCheckOut && !_killed();
}

bool ObservableSession::_killed() const {
    return _sri->killsRequested > 0;
}

OperationContextSession::OperationContextSession(OperationContext* opCtx) : _opCtx(opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (checkedOutSession) {
        // The only case where a session can be checked-out more than once is due to DBDirectClient
        // reentrancy
        invariant(opCtx->getClient()->isInDirectClient());
        return;
    }

    checkOut(opCtx);
}

OperationContextSession::OperationContextSession(OperationContext* opCtx,
                                                 SessionCatalog::KillToken killToken)
    : _opCtx(opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);

    invariant(!checkedOutSession);
    invariant(!opCtx->getLogicalSessionId());  // lsid is specified by killToken argument.

    const auto catalog = SessionCatalog::get(opCtx);
    auto scopedSessionForKill = catalog->checkOutSessionForKill(opCtx, std::move(killToken));

    // We acquire a Client lock here to guard the construction of this session so that references to
    // this session are safe to use while the lock is held
    stdx::lock_guard lk(*opCtx->getClient());
    checkedOutSession.emplace(std::move(scopedSessionForKill._scos));
}

OperationContextSession::~OperationContextSession() {
    // Only release the checked out session at the end of the top-level request from the client, not
    // at the end of a nested DBDirectClient call
    if (_opCtx->getClient()->isInDirectClient()) {
        return;
    }

    auto& checkedOutSession = operationSessionDecoration(_opCtx);
    if (!checkedOutSession)
        return;

    checkIn(_opCtx, CheckInReason::kDone);
}

Session* OperationContextSession::get(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (checkedOutSession) {
        return checkedOutSession->get();
    }

    return nullptr;
}

void OperationContextSession::checkIn(OperationContext* opCtx, CheckInReason reason) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(checkedOutSession);

    if (reason == CheckInReason::kYield) {
        // Don't allow yielding a session that was checked out for kill because it will "unkill" the
        // session and the subsequent check out will not have priority, which can easily lead to
        // bugs. If you need to run an operation with a session that may yield, kill the session,
        // check it out for kill, release it, then check it out normally.
        invariant(!checkedOutSession->wasCheckedOutForKill());
    }

    // Removing the checkedOutSession from the OperationContext must be done under the Client lock,
    // but destruction of the checkedOutSession must not be, as it takes the SessionCatalog mutex,
    // and other code may take the Client lock while holding that mutex.
    stdx::unique_lock<Client> lk(*opCtx->getClient());
    SessionCatalog::ScopedCheckedOutSession sessionToReleaseOutOfLock(
        std::move(*checkedOutSession));

    // This destroys the moved-from ScopedCheckedOutSession, and must be done within the client lock
    checkedOutSession = boost::none;
    lk.unlock();
}

void OperationContextSession::checkOut(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(!checkedOutSession);

    const auto catalog = SessionCatalog::get(opCtx);
    auto scopedCheckedOutSession = catalog->_checkOutSession(opCtx);

    // We acquire a Client lock here to guard the construction of this session so that references to
    // this session are safe to use while the lock is held
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    checkedOutSession.emplace(std::move(scopedCheckedOutSession));
}

}  // namespace mongo
