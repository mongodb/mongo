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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/cursor_manager.h"

#include <memory>

#include "mongo/base/data_cursor.h"
#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/cursor_server_params.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/util/exit.h"

namespace mongo {

constexpr int CursorManager::kNumPartitions;

namespace {

const auto serviceCursorManager =
    ServiceContext::declareDecoration<std::unique_ptr<CursorManager>>();

ServiceContext::ConstructorActionRegisterer cursorManagerRegisterer{
    "CursorManagerRegisterer", [](ServiceContext* svcCtx) {
        auto cursorManager = std::make_unique<CursorManager>();
        CursorManager::set(svcCtx, std::move(cursorManager));
    }};
}  // namespace

CursorManager* CursorManager::get(ServiceContext* svcCtx) {
    return serviceCursorManager(svcCtx).get();
}

CursorManager* CursorManager::get(OperationContext* optCtx) {
    return get(optCtx->getServiceContext());
}

void CursorManager::set(ServiceContext* svcCtx, std::unique_ptr<CursorManager> newCursorManager) {
    invariant(newCursorManager);
    auto& cursorManager = serviceCursorManager(svcCtx);
    cursorManager = std::move(newCursorManager);
}

std::pair<Status, int> CursorManager::killCursorsWithMatchingSessions(
    OperationContext* opCtx, const SessionKiller::Matcher& matcher) {
    auto eraser = [&](CursorManager& mgr, CursorId id) {
        uassertStatusOK(mgr.killCursor(opCtx, id, true));
        LOGV2(20528, "killing cursor: {id} as part of killing session(s)", "id"_attr = id);
    };

    auto bySessionCursorKiller = makeKillCursorsBySessionAdaptor(opCtx, matcher, std::move(eraser));
    bySessionCursorKiller(*this);

    return std::make_pair(bySessionCursorKiller.getStatus(),
                          bySessionCursorKiller.getCursorsKilled());
}

CursorManager::CursorManager()
    : _random(std::make_unique<PseudoRandom>(SecureRandom().nextInt64())),
      _cursorMap(std::make_unique<Partitioned<stdx::unordered_map<CursorId, ClientCursor*>>>()) {}

CursorManager::~CursorManager() {
    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& cursor : partition) {
            // Callers must ensure that no cursors are in use.
            invariant(!cursor.second->_operationUsingCursor);
            cursor.second->dispose(nullptr);
            delete cursor.second;
        }
    }
}

bool CursorManager::cursorShouldTimeout_inlock(const ClientCursor* cursor, Date_t now) {
    if (cursor->isNoTimeout() || cursor->_operationUsingCursor) {
        return false;
    }
    return (now - cursor->_lastUseDate) >= Milliseconds(getCursorTimeoutMillis());
}

std::size_t CursorManager::timeoutCursors(OperationContext* opCtx, Date_t now) {
    std::vector<std::unique_ptr<ClientCursor, ClientCursor::Deleter>> toDisposeWithoutMutex;

    for (size_t partitionId = 0; partitionId < kNumPartitions; ++partitionId) {
        auto lockedPartition = _cursorMap->lockOnePartitionById(partitionId);
        for (auto it = lockedPartition->begin(); it != lockedPartition->end();) {
            auto* cursor = it->second;
            if (cursorShouldTimeout_inlock(cursor, now)) {
                toDisposeWithoutMutex.emplace_back(cursor);
                // Advance the iterator first since erasing from the lockedPartition will
                // invalidate any references to it.
                ++it;
                removeCursorFromMap(lockedPartition, cursor);
            } else {
                ++it;
            }
        }
    }

    // Be careful not to dispose of cursors while holding the partition lock.
    for (auto&& cursor : toDisposeWithoutMutex) {
        LOGV2(20529,
              "Cursor id {cursor_cursorid} timed out, idle since {cursor_getLastUseDate}",
              "cursor_cursorid"_attr = cursor->cursorid(),
              "cursor_getLastUseDate"_attr = cursor->getLastUseDate());
        cursor->dispose(opCtx);
    }
    return toDisposeWithoutMutex.size();
}

StatusWith<ClientCursorPin> CursorManager::pinCursor(OperationContext* opCtx,
                                                     CursorId id,
                                                     AuthCheck checkSessionAuth) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "cursor id " << id << " not found"};
    }

    ClientCursor* cursor = it->second;
    uassert(ErrorCodes::CursorInUse,
            str::stream() << "cursor id " << id << " is already in use",
            !cursor->_operationUsingCursor);
    if (cursor->getExecutor()->isMarkedAsKilled()) {
        // This cursor was killed while it was idle.
        Status error = cursor->getExecutor()->getKillStatus();
        deregisterAndDestroyCursor(std::move(lockedPartition),
                                   opCtx,
                                   std::unique_ptr<ClientCursor, ClientCursor::Deleter>(cursor));
        return error;
    }

    if (checkSessionAuth == kCheckSession) {
        auto cursorPrivilegeStatus = checkCursorSessionPrivilege(opCtx, cursor->getSessionId());
        if (!cursorPrivilegeStatus.isOK()) {
            return cursorPrivilegeStatus;
        }
    }

    cursor->_operationUsingCursor = opCtx;

    // We use pinning of a cursor as a proxy for active, user-initiated use of a cursor.  Therefore,
    // we pass down to the logical session cache and vivify the record (updating last use).
    if (cursor->getSessionId()) {
        auto vivifyCursorStatus =
            LogicalSessionCache::get(opCtx)->vivify(opCtx, cursor->getSessionId().get());
        if (!vivifyCursorStatus.isOK()) {
            return vivifyCursorStatus;
        }
    }

    return ClientCursorPin(opCtx, cursor, this);
}

void CursorManager::unpin(OperationContext* opCtx,
                          std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    // Avoid computing the current time within the critical section.
    auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();

    auto partition = _cursorMap->lockOnePartition(cursor->cursorid());
    invariant(cursor->_operationUsingCursor);

    // We must verify that no interrupts have occurred since we finished building the current
    // batch. Otherwise, the cursor will be checked back in, the interrupted opCtx will be
    // destroyed, and subsequent getMores with a fresh opCtx will succeed.
    auto interruptStatus = cursor->_operationUsingCursor->checkForInterruptNoAssert();
    cursor->_operationUsingCursor = nullptr;
    cursor->_lastUseDate = now;

    // If someone was trying to kill this cursor with a killOp or a killCursors, they are likely
    // interesting in proactively cleaning up that cursor's resources. In these cases, we
    // proactively delete the cursor. In other cases we preserve the error code so that the client
    // will see the reason the cursor was killed when asking for the next batch.
    if (interruptStatus == ErrorCodes::Interrupted || interruptStatus == ErrorCodes::CursorKilled) {
        LOGV2(20530,
              "removing cursor {cursor_cursorid} after completing batch: {interruptStatus}",
              "cursor_cursorid"_attr = cursor->cursorid(),
              "interruptStatus"_attr = interruptStatus);
        return deregisterAndDestroyCursor(std::move(partition), opCtx, std::move(cursor));
    } else if (!interruptStatus.isOK()) {
        cursor->markAsKilled(interruptStatus);
    }

    // The cursor will stay around in '_cursorMap', so release the unique pointer to avoid deleting
    // it.
    cursor.release();
}

void CursorManager::appendActiveSessions(LogicalSessionIdSet* lsids) const {
    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : partition) {
            auto cursor = entry.second;
            if (auto id = cursor->getSessionId()) {
                lsids->insert(id.value());
            }
        }
    }
}

std::vector<GenericCursor> CursorManager::getIdleCursors(
    OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode) const {
    std::vector<GenericCursor> cursors;
    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : partition) {
            auto cursor = entry.second;

            // Exclude cursors that this user does not own if auth is enabled.
            if (ctxAuth->getAuthorizationManager().isAuthEnabled() &&
                userMode == MongoProcessInterface::CurrentOpUserMode::kExcludeOthers &&
                !ctxAuth->isCoauthorizedWith(cursor->getAuthenticatedUsers())) {
                continue;
            }
            // Exclude pinned cursors.
            if (cursor->_operationUsingCursor) {
                continue;
            }
            cursors.emplace_back(cursor->toGenericCursor());
        }
    }

    return cursors;
}

stdx::unordered_set<CursorId> CursorManager::getCursorsForSession(LogicalSessionId lsid) const {
    stdx::unordered_set<CursorId> cursors;

    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : partition) {
            auto cursor = entry.second;
            if (cursor->getSessionId() == lsid) {
                cursors.insert(cursor->cursorid());
            }
        }
    }

    return cursors;
}

stdx::unordered_set<CursorId> CursorManager::getCursorsForOpKeys(
    std::vector<OperationKey> opKeys) const {
    stdx::unordered_set<CursorId> cursors;

    stdx::lock_guard<Latch> lk(_opKeyMutex);
    for (auto opKey : opKeys) {
        if (auto it = _opKeyMap.find(opKey); it != _opKeyMap.end())
            cursors.insert(it->second);
    }
    return cursors;
}

size_t CursorManager::numCursors() const {
    return _cursorMap->size();
}

CursorId CursorManager::allocateCursorId_inlock() {
    for (int i = 0; i < 10000; i++) {
        CursorId id = _random->nextInt64();

        // A cursor id of zero is reserved to indicate that the cursor has been closed. If the
        // random number generator gives us zero, then try again.
        if (id == 0) {
            continue;
        }

        // Avoid negative cursor ids by taking the absolute value. If the cursor id is the minimum
        // representable negative number, then just generate another random id.
        if (id == std::numeric_limits<CursorId>::min()) {
            continue;
        }
        id = std::abs(id);

        auto partition = _cursorMap->lockOnePartition(id);
        if (partition->count(id) == 0) {
            // The cursor id is not already in use, so return it. Even though we drop the lock on
            // the '_cursorMap' partition, another thread cannot register a cursor with the same id
            // because we still hold '_registrationLock'.
            return id;
        }

        // The cursor id is already in use. Generate another random id.
    }

    // We failed to generate a unique cursor id.
    fassertFailed(17360);
}

ClientCursorPin CursorManager::registerCursor(OperationContext* opCtx,
                                              ClientCursorParams&& cursorParams) {
    // Avoid computing the current time within the critical section.
    auto now = opCtx->getServiceContext()->getPreciseClockSource()->now();

    // Make sure the PlanExecutor isn't registered, since we will register the ClientCursor wrapping
    // it.
    invariant(cursorParams.exec);
    cursorParams.exec.get_deleter().dismissDisposal();

    // Note we must hold the registration lock from now until insertion into '_cursorMap' to ensure
    // we don't insert two cursors with the same cursor id.
    stdx::lock_guard<SimpleMutex> lock(_registrationLock);
    CursorId cursorId = allocateCursorId_inlock();
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor(
        new ClientCursor(std::move(cursorParams), cursorId, opCtx, now));

    // Register this cursor for lookup by transaction.
    if (opCtx->getLogicalSessionId() && opCtx->getTxnNumber()) {
        invariant(opCtx->getLogicalSessionId());
    }

    // Transfer ownership of the cursor to '_cursorMap'.
    auto partition = _cursorMap->lockOnePartition(cursorId);
    ClientCursor* unownedCursor = clientCursor.release();
    partition->emplace(cursorId, unownedCursor);

    // If set, store the mapping of OperationKey to the generated CursorID.
    if (auto opKey = opCtx->getOperationKey()) {
        stdx::lock_guard<Latch> lk(_opKeyMutex);
        _opKeyMap.emplace(*opKey, cursorId);
    }

    return ClientCursorPin(opCtx, unownedCursor, this);
}

void CursorManager::deregisterCursor(ClientCursor* cursor) {
    removeCursorFromMap(_cursorMap, cursor);
}

void CursorManager::deregisterAndDestroyCursor(
    Partitioned<stdx::unordered_map<CursorId, ClientCursor*>, kNumPartitions>::OnePartition&& lk,
    OperationContext* opCtx,
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    {
        auto lockWithRestrictedScope = std::move(lk);
        removeCursorFromMap(lockWithRestrictedScope, cursor.get());
    }
    // Dispose of the cursor without holding any cursor manager mutexes. Disposal of a cursor can
    // require taking lock manager locks, which we want to avoid while holding a mutex. If we did
    // so, any caller of a CursorManager method which already held a lock manager lock could induce
    // a deadlock when trying to acquire a CursorManager lock.
    cursor->dispose(opCtx);
}

Status CursorManager::killCursor(OperationContext* opCtx, CursorId id, bool shouldAudit) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(opCtx->getClient(), {}, id, ErrorCodes::CursorNotFound);
        }
        return {ErrorCodes::CursorNotFound, str::stream() << "Cursor id not found: " << id};
    }
    auto cursor = it->second;

    if (cursor->_operationUsingCursor) {
        // Rather than removing the cursor directly, kill the operation that's currently using the
        // cursor. It will stop on its own (and remove the cursor) when it sees that it's been
        // interrupted.
        {
            stdx::unique_lock<Client> lk(*cursor->_operationUsingCursor->getClient());
            cursor->_operationUsingCursor->getServiceContext()->killOperation(
                lk, cursor->_operationUsingCursor, ErrorCodes::CursorKilled);
        }

        if (shouldAudit) {
            audit::logKillCursorsAuthzCheck(opCtx->getClient(), cursor->nss(), id, ErrorCodes::OK);
        }
        return Status::OK();
    }
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> ownedCursor(cursor);

    if (shouldAudit) {
        audit::logKillCursorsAuthzCheck(opCtx->getClient(), cursor->nss(), id, ErrorCodes::OK);
    }

    deregisterAndDestroyCursor(std::move(lockedPartition), opCtx, std::move(ownedCursor));
    return Status::OK();
}

Status CursorManager::checkAuthForKillCursors(OperationContext* opCtx, CursorId id) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "cursor id " << id << " not found"};
    }

    ClientCursor* cursor = it->second;
    // Note that we're accessing the cursor without having pinned it! This is okay since we're only
    // accessing nss() and getAuthenticatedUsers() both of which return values that don't change
    // after the cursor's creation. We're guaranteed that the cursor won't get destroyed while we're
    // reading from it because we hold the partition's lock.
    AuthorizationSession* as = AuthorizationSession::get(opCtx->getClient());
    return as->checkAuthForKillCursors(cursor->nss(), cursor->getAuthenticatedUsers());
}

}  // namespace mongo
