
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/session_catalog.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto sessionTransactionTableDecoration = ServiceContext::declareDecoration<SessionCatalog>();

const auto operationSessionDecoration =
    OperationContext::declareDecoration<boost::optional<ScopedCheckedOutSession>>();

}  // namespace

SessionCatalog::~SessionCatalog() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    for (const auto& entry : _sessions) {
        auto& sri = entry.second;
        invariant(!sri->session.currentOperation());
        invariant(!sri->session.killed());
    }
}

void SessionCatalog::reset_forTest() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _sessions.clear();
}

SessionCatalog* SessionCatalog::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

SessionCatalog* SessionCatalog::get(ServiceContext* service) {
    auto& sessionTransactionTable = sessionTransactionTableDecoration(service);
    return &sessionTransactionTable;
}

ScopedCheckedOutSession SessionCatalog::checkOutSession(OperationContext* opCtx) {
    // This method is not supposed to be called with an already checked-out session due to risk of
    // deadlock
    invariant(!operationSessionDecoration(opCtx));
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    invariant(!opCtx->lockState()->isLocked());

    invariant(opCtx->getLogicalSessionId());
    const auto lsid = *opCtx->getLogicalSessionId();

    stdx::unique_lock<stdx::mutex> ul(_mutex);
    auto sri = _getOrCreateSessionRuntimeInfo(ul, opCtx, lsid);

    // Wait until the session is no longer checked out and until the previously scheduled kill has
    // completed
    opCtx->waitForConditionOrInterrupt(sri->availableCondVar, ul, [&sri]() {
        return !sri->session.currentOperation() && !sri->session.killed();
    });

    sri->session._markCheckedOut(ul, opCtx);

    return ScopedCheckedOutSession(
        opCtx, ScopedSession(std::move(sri)), boost::none /* Not checked out for kill */);
}

ScopedCheckedOutSession SessionCatalog::checkOutSessionForKill(OperationContext* opCtx,
                                                               Session::KillToken killToken) {
    // This method is not supposed to be called with an already checked-out session due to risk of
    // deadlock
    invariant(!operationSessionDecoration(opCtx));
    invariant(!opCtx->getTxnNumber());

    const auto lsid = killToken.lsidToKill;

    stdx::unique_lock<stdx::mutex> ul(_mutex);
    auto sri = _getOrCreateSessionRuntimeInfo(ul, opCtx, lsid);
    invariant(sri->session.killed());

    // Wait until the session is no longer checked out
    opCtx->waitForConditionOrInterrupt(
        sri->availableCondVar, ul, [&sri]() { return !sri->session.currentOperation(); });

    invariant(!sri->session.currentOperation());
    sri->session._markCheckedOut(ul, opCtx);

    return ScopedCheckedOutSession(
        opCtx, ScopedSession(std::move(sri)), std::move(killToken) /* Checked out for kill */);
}

ScopedSession SessionCatalog::getOrCreateSession(OperationContext* opCtx,
                                                 const LogicalSessionId& lsid) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getLogicalSessionId());
    invariant(!opCtx->getTxnNumber());

    auto ss = [&] {
        stdx::unique_lock<stdx::mutex> ul(_mutex);
        return ScopedSession(_getOrCreateSessionRuntimeInfo(ul, opCtx, lsid));
    }();

    return ss;
}

void SessionCatalog::scanSessions(const SessionKiller::Matcher& matcher,
                                  const ScanSessionsCallbackFn& workerFn) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    LOG(2) << "Beginning scanSessions. Scanning " << _sessions.size() << " sessions.";

    for (auto& sessionEntry : _sessions) {
        if (matcher.match(sessionEntry.first)) {
            workerFn(lg, &sessionEntry.second->session);
        }
    }
}

Session::KillToken SessionCatalog::killSession(const LogicalSessionId& lsid) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    auto it = _sessions.find(lsid);
    uassert(ErrorCodes::NoSuchSession, "Session not found", it != _sessions.end());

    auto& sri = it->second;
    return sri->session.kill(lg);
}

std::shared_ptr<SessionCatalog::SessionRuntimeInfo> SessionCatalog::_getOrCreateSessionRuntimeInfo(
    WithLock, OperationContext* opCtx, const LogicalSessionId& lsid) {
    auto it = _sessions.find(lsid);
    if (it == _sessions.end()) {
        it = _sessions.emplace(lsid, std::make_shared<SessionRuntimeInfo>(lsid)).first;
    }

    return it->second;
}

void SessionCatalog::_releaseSession(const LogicalSessionId& lsid,
                                     boost::optional<Session::KillToken> killToken) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _sessions.find(lsid);
    invariant(it != _sessions.end());

    auto& sri = it->second;
    invariant(sri->session.currentOperation());

    sri->session._markCheckedIn(lg);
    sri->availableCondVar.notify_one();

    if (killToken) {
        invariant(sri->session.killed());
        sri->session._markNotKilled(lg, std::move(*killToken));
    }
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

OperationContextSession::~OperationContextSession() {
    // Only release the checked out session at the end of the top-level request from the client, not
    // at the end of a nested DBDirectClient call
    if (_opCtx->getClient()->isInDirectClient()) {
        return;
    }

    auto& checkedOutSession = operationSessionDecoration(_opCtx);
    if (!checkedOutSession)
        return;

    checkIn(_opCtx);
}

Session* OperationContextSession::get(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (checkedOutSession) {
        return checkedOutSession->get();
    }

    return nullptr;
}

void OperationContextSession::checkIn(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(checkedOutSession);

    // Removing the checkedOutSession from the OperationContext must be done under the Client lock,
    // but destruction of the checkedOutSession must not be, as it takes the SessionCatalog mutex,
    // and other code may take the Client lock while holding that mutex.
    stdx::unique_lock<Client> lk(*opCtx->getClient());
    ScopedCheckedOutSession sessionToReleaseOutOfLock(std::move(*checkedOutSession));

    // This destroys the moved-from ScopedCheckedOutSession, and must be done within the client lock
    checkedOutSession = boost::none;
    lk.unlock();
}

void OperationContextSession::checkOut(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    invariant(!checkedOutSession);

    const auto catalog = SessionCatalog::get(opCtx);
    auto scopedCheckedOutSession = catalog->checkOutSession(opCtx);

    // We acquire a Client lock here to guard the construction of this session so that references to
    // this session are safe to use while the lock is held
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    checkedOutSession.emplace(std::move(scopedCheckedOutSession));
}

}  // namespace mongo
