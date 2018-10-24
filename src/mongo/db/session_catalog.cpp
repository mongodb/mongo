
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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/rpc/get_status_from_command_result.h"
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
        invariant(!sri->checkedOut);
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
    invariant(!opCtx->lockState()->isLocked());
    invariant(opCtx->getLogicalSessionId());

    const auto lsid = *opCtx->getLogicalSessionId();

    stdx::unique_lock<stdx::mutex> ul(_mutex);

    auto sri = _getOrCreateSessionRuntimeInfo(ul, opCtx, lsid);

    // Wait until the session is no longer checked out
    opCtx->waitForConditionOrInterrupt(
        sri->availableCondVar, ul, [&sri]() { return !sri->checkedOut; });

    invariant(!sri->checkedOut);
    sri->checkedOut = true;

    return ScopedCheckedOutSession(opCtx, ScopedSession(std::move(sri)));
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

void SessionCatalog::invalidateSessions(OperationContext* opCtx,
                                        boost::optional<BSONObj> singleSessionDoc) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    bool isReplSet = replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
    if (isReplSet) {
        uassert(40528,
                str::stream() << "Direct writes against "
                              << NamespaceString::kSessionTransactionsTableNamespace.ns()
                              << " cannot be performed using a transaction or on a session.",
                !opCtx->getLogicalSessionId());
    }

    const auto invalidateSessionFn = [&](WithLock, decltype(_sessions)::iterator it) {
        auto& sri = it->second;
        auto const txnParticipant =
            TransactionParticipant::getFromNonCheckedOutSession(&sri->txnState);
        txnParticipant->invalidate();

        // We cannot remove checked-out sessions from the cache, because operations expect to find
        // them there to check back in
        if (!sri->checkedOut) {
            _sessions.erase(it);
        }
    };

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    if (singleSessionDoc) {
        const auto lsid = LogicalSessionId::parse(IDLParserErrorContext("lsid"),
                                                  singleSessionDoc->getField("_id").Obj());

        auto it = _sessions.find(lsid);
        if (it != _sessions.end()) {
            invalidateSessionFn(lg, it);
        }
    } else {
        auto it = _sessions.begin();
        while (it != _sessions.end()) {
            invalidateSessionFn(lg, it++);
        }
    }
}

void SessionCatalog::scanSessions(OperationContext* opCtx,
                                  const SessionKiller::Matcher& matcher,
                                  const ScanSessionsCallbackFn& workerFn) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    LOG(2) << "Beginning scanSessions. Scanning " << _sessions.size() << " sessions.";

    for (auto& sessionEntry : _sessions) {
        if (matcher.match(sessionEntry.first)) {
            workerFn(opCtx, &sessionEntry.second->txnState);
        }
    }
}

std::shared_ptr<SessionCatalog::SessionRuntimeInfo> SessionCatalog::_getOrCreateSessionRuntimeInfo(
    WithLock, OperationContext* opCtx, const LogicalSessionId& lsid) {
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    auto it = _sessions.find(lsid);
    if (it == _sessions.end()) {
        it = _sessions.emplace(lsid, std::make_shared<SessionRuntimeInfo>(lsid)).first;
    }

    return it->second;
}

void SessionCatalog::_releaseSession(const LogicalSessionId& lsid) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    auto it = _sessions.find(lsid);
    invariant(it != _sessions.end());

    auto& sri = it->second;
    invariant(sri->checkedOut);

    sri->checkedOut = false;
    sri->availableCondVar.notify_one();
}

OperationContextSession::OperationContextSession(OperationContext* opCtx, bool checkOutSession)
    : _opCtx(opCtx) {
    if (!opCtx->getLogicalSessionId()) {
        return;
    }

    if (!checkOutSession) {
        return;
    }

    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (!checkedOutSession) {
        auto sessionTransactionTable = SessionCatalog::get(opCtx);
        auto scopedCheckedOutSession = sessionTransactionTable->checkOutSession(opCtx);
        // We acquire a Client lock here to guard the construction of this session so that
        // references to this session are safe to use while the lock is held.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        checkedOutSession.emplace(std::move(scopedCheckedOutSession));
    } else {
        // The only reason to be trying to check out a session when you already have a session
        // checked out is if you're in DBDirectClient.
        invariant(opCtx->getClient()->isInDirectClient());
        return;
    }

    const auto session = checkedOutSession->get();
    invariant(opCtx->getLogicalSessionId() == session->getSessionId());
    session->setCurrentOperation(opCtx);
}

OperationContextSession::~OperationContextSession() {
    // Only release the checked out session at the end of the top-level request from the client,
    // not at the end of a nested DBDirectClient call.
    if (_opCtx->getClient()->isInDirectClient()) {
        return;
    }

    auto& checkedOutSession = operationSessionDecoration(_opCtx);
    if (checkedOutSession) {
        checkedOutSession->get()->clearCurrentOperation();

        // Removing the checkedOutSession from the OperationContext must be done under the Client
        // lock, but destruction of the checkedOutSession must not be, as it takes the
        // SessionCatalog mutex, and other code may take the Client lock while holding that mutex.
        stdx::unique_lock<Client> lk(*_opCtx->getClient());
        ScopedCheckedOutSession sessionToDelete(std::move(checkedOutSession.get()));
        // This destroys the moved-from ScopedCheckedOutSession, and must be done within the client
        // lock.
        checkedOutSession = boost::none;
        lk.unlock();
    }
}

Session* OperationContextSession::get(OperationContext* opCtx) {
    auto& checkedOutSession = operationSessionDecoration(opCtx);
    if (checkedOutSession) {
        return checkedOutSession->get();
    }

    return nullptr;
}

}  // namespace mongo
