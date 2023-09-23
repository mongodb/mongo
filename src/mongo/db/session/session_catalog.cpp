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


#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/concurrency/locker.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {
namespace {

const auto sessionTransactionTableDecoration = ServiceContext::declareDecoration<SessionCatalog>();

const auto operationSessionDecoration =
    OperationContext::declareDecoration<boost::optional<SessionCatalog::ScopedCheckedOutSession>>();

MONGO_FAIL_POINT_DEFINE(hangAfterIncrementingNumWaitingToCheckOut);

std::string provenanceToString(SessionCatalog::Provenance provenance) {
    switch (provenance) {
        case SessionCatalog::Provenance::kRouter:
            return "router";
        case SessionCatalog::Provenance::kParticipant:
            return "participant";
    }
    MONGO_UNREACHABLE;
}

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
        dassert(killToken->lsidToKill == lsid);
    } else {
        dassert(opCtx->getLogicalSessionId() == lsid);
    }

    stdx::unique_lock<Latch> ul(_mutex);

    auto sri = _getOrCreateSessionRuntimeInfo(ul, lsid);
    auto session = sri->getSession(ul, lsid);
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
                                 const ScanSessionsCallbackFn& workerFn,
                                 ScanSessionCreateSession createSession) {
    stdx::lock_guard<Latch> lg(_mutex);

    auto sri = (createSession == ScanSessionCreateSession::kYes)
        ? _getOrCreateSessionRuntimeInfo(lg, lsid)
        : _getSessionRuntimeInfo(lg, lsid);

    if (sri) {
        auto session = sri->getSession(lg, lsid);
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

void SessionCatalog::scanParentSessions(const ScanSessionsCallbackFn& workerFn) {
    stdx::lock_guard<Latch> lg(_mutex);

    LOGV2_DEBUG(6685000,
                2,
                "Scanning {sessionCount} sessions",
                "Scanning sessions",
                "sessionCount"_attr = _sessions.size());

    for (auto& [parentLsid, sri] : _sessions) {
        ObservableSession osession(lg, sri.get(), &sri->parentSession);
        workerFn(osession);
        invariant(!osession._markedForReap, "Cannot reap a session via 'scanSessions'");
    }
}

LogicalSessionIdSet SessionCatalog::scanSessionsForReap(
    const LogicalSessionId& parentLsid,
    const ScanSessionsCallbackFn& parentSessionWorkerFn,
    const ScanSessionsCallbackFn& childSessionWorkerFn) {
    invariant(isParentSessionId(parentLsid));

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
    auto session = sri->getSession(lg, lsid);
    uassert(ErrorCodes::NoSuchSession, "Session not found", session);
    return ObservableSession(lg, sri, session).kill();
}

size_t SessionCatalog::size() const {
    stdx::lock_guard<Latch> lg(_mutex);
    return _sessions.size();
}

SessionCatalog::SessionRuntimeInfo* SessionCatalog::_getSessionRuntimeInfo(
    WithLock wl, const LogicalSessionId& lsid) {
    const auto& parentLsid = isParentSessionId(lsid) ? lsid : *getParentSessionId(lsid);
    auto sriIt = _sessions.find(parentLsid);

    if (sriIt == _sessions.end()) {
        return nullptr;
    }

    auto sri = sriIt->second.get();
    auto session = sri->getSession(wl, lsid);

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

    const auto& parentLsid = isParentSessionId(lsid) ? lsid : *getParentSessionId(lsid);
    auto sriIt =
        _sessions.emplace(parentLsid, std::make_unique<SessionRuntimeInfo>(parentLsid)).first;
    auto sri = sriIt->second.get();

    if (isChildSession(lsid)) {
        auto [childSessionIt, inserted] = sri->childSessions.try_emplace(lsid, lsid);
        // Insert should always succeed since the session did not exist prior to this.
        invariant(inserted);

        auto& childSession = childSessionIt->second;
        childSession._parentSession = &sri->parentSession;
    }

    return sri;
}

void SessionCatalog::_releaseSession(
    SessionRuntimeInfo* sri,
    Session* session,
    boost::optional<KillToken> killToken,
    boost::optional<TxnNumberAndProvenance> clientTxnNumberStarted) {
    stdx::unique_lock<Latch> ul(_mutex);

    // Make sure we have exactly the same session on the map and that it is still associated with an
    // operation context (meaning checked-out)
    invariant(_sessions[sri->parentSession.getSessionId()].get() == sri);
    invariant(sri->checkoutOpCtx);
    if (killToken) {
        dassert(killToken->lsidToKill == session->getSessionId());
    }

    ServiceContext* service = sri->checkoutOpCtx->getServiceContext();

    sri->checkoutOpCtx = nullptr;
    sri->availableCondVar.notify_all();

    if (killToken) {
        invariant(sri->killsRequested > 0);
        --sri->killsRequested;
    }

    std::vector<LogicalSessionId> eagerlyReapedSessions;
    if (clientTxnNumberStarted.has_value()) {
        auto [txnNumber, provenance] = *clientTxnNumberStarted;

        // Since the given txnNumber successfully started, we know any child sessions with older
        // txnNumbers can be discarded. This needed to wait until a transaction started because that
        // can fail, e.g. if the active transaction is prepared.
        auto workerFn = _makeSessionWorkerFnForEagerReap(service, txnNumber, provenance);
        auto numReaped = stdx::erase_if(sri->childSessions, [&](auto&& it) {
            ObservableSession osession(ul, sri, &it.second);
            workerFn(osession);

            bool willReap = osession._shouldBeReaped() &&
                (osession._reapMode == ObservableSession::ReapMode::kExclusive);
            if (willReap) {
                eagerlyReapedSessions.push_back(std::move(it.first));
            }
            return willReap;
        });

        sri->lastClientTxnNumberStarted = txnNumber;

        LOGV2_DEBUG(6685200,
                    4,
                    "Erased child sessions",
                    "releasedLsid"_attr = session->getSessionId(),
                    "clientTxnNumber"_attr = txnNumber,
                    "childSessionsRemaining"_attr = sri->childSessions.size(),
                    "numReaped"_attr = numReaped,
                    "provenance"_attr = provenanceToString(provenance));
    }

    invariant(ul);
    ul.unlock();

    if (eagerlyReapedSessions.size() && _onEagerlyReapedSessionsFn) {
        (*_onEagerlyReapedSessionsFn)(service, std::move(eagerlyReapedSessions));
    }
}

SessionCatalog::ScanSessionsCallbackFn SessionCatalog::_defaultMakeSessionWorkerFnForEagerReap(
    ServiceContext* service, TxnNumber clientTxnNumberStarted, Provenance provenance) {
    return [clientTxnNumberStarted](ObservableSession& osession) {
        // If a higher txnNumber has been seen for a client and started a transaction, assume any
        // child sessions for lower transactions have been superseded and can be reaped.
        const auto& transactionSessionId = osession.getSessionId();
        if (transactionSessionId.getTxnNumber() &&
            *transactionSessionId.getTxnNumber() < clientTxnNumberStarted) {
            osession.markForReap(ObservableSession::ReapMode::kExclusive);
        }
    };
}

Session* SessionCatalog::SessionRuntimeInfo::getSession(WithLock, const LogicalSessionId& lsid) {
    if (isParentSessionId(lsid)) {
        // We should have already compared the parent lsid when we found this SRI.
        dassert(lsid == parentSession._sessionId);
        return &parentSession;
    }

    dassert(getParentSessionId(lsid) == parentSession._sessionId);
    auto it = childSessions.find(lsid);
    if (it == childSessions.end()) {
        return nullptr;
    }
    return &it->second;
}

SessionCatalog::KillToken ObservableSession::kill(ErrorCodes::Error reason) const {
    const bool firstKiller = (0 == _sri->killsRequested);
    ++_sri->killsRequested;

    if (firstKiller && hasCurrentOperation()) {
        invariant(_clientLock.owns_lock());
        const auto serviceContext = _sri->checkoutOpCtx->getServiceContext();
        serviceContext->killOperation(_clientLock, _sri->checkoutOpCtx, reason);
    }

    return SessionCatalog::KillToken(getSessionId());
}

void ObservableSession::markForReap(ReapMode reapMode) {
    if (isParentSessionId(getSessionId())) {
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

void OperationContextSession::observeNewTxnNumberStarted(
    OperationContext* opCtx,
    const LogicalSessionId& lsid,
    SessionCatalog::TxnNumberAndProvenance txnNumberAndProvenance) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(checkedOutSession);

    LOGV2_DEBUG(6685201,
                4,
                "Observing new retryable write number started on session",
                "lsid"_attr = lsid,
                "txnNumber"_attr = txnNumberAndProvenance.first,
                "provenance"_attr = txnNumberAndProvenance.second);

    const auto& checkedOutLsid = (*checkedOutSession)->getSessionId();
    if (isParentSessionId(lsid)) {
        // Observing a new transaction/retryable write on a parent session.

        // The operation must have checked out the parent session itself or a child session of the
        // parent. This is safe because both share the same SessionRuntimeInfo.
        dassert(lsid == checkedOutLsid || lsid == *getParentSessionId(checkedOutLsid));

        checkedOutSession->observeNewClientTxnNumberStarted(txnNumberAndProvenance);
    } else if (isInternalSessionForRetryableWrite(lsid)) {
        // Observing a new internal transaction on a retryable session.

        // A transaction on a child session is always begun on an operation that checked it out
        // directly.
        dassert(lsid == checkedOutLsid);

        checkedOutSession->observeNewClientTxnNumberStarted(
            {*lsid.getTxnNumber(), txnNumberAndProvenance.second});
    }
}

}  // namespace mongo
