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

#include "mongo/db/query/client_cursor/cursor_manager.h"

// IWYU pragma: no_include "boost/align/detail/aligned_alloc_posix.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_in_use_info.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/client_cursor/cursor_server_params.h"
#include "mongo/db/query/client_cursor/generic_cursor_utils.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions_common.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/aligned.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <string>
#include <type_traits>

#include <boost/none.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

constexpr int kNumPartitions = 16;

const auto serviceCursorManager =
    ServiceContext::declareDecoration<std::unique_ptr<CursorManager>>();

ServiceContext::ConstructorActionRegisterer cursorManagerRegisterer{
    "CursorManagerRegisterer", [](ServiceContext* svcCtx) {
        auto cursorManager = std::make_unique<CursorManager>(svcCtx->getPreciseClockSource());
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
        uassertStatusOK(mgr.killCursor(opCtx, id));
        LOGV2(20528, "Killing cursor as part of killing session(s)", "cursorId"_attr = id);
    };

    auto bySessionCursorKiller = makeKillCursorsBySessionAdaptor(opCtx, matcher, std::move(eraser));
    bySessionCursorKiller(*this);

    return std::make_pair(bySessionCursorKiller.getStatus(),
                          bySessionCursorKiller.getCursorsKilled());
}

CursorManager::CursorManager(ClockSource* preciseClockSource)
    : _preciseClockSource(preciseClockSource),
      _random(std::make_unique<PseudoRandom>(SecureRandom().nextInt64())),
      _cursorMap(std::make_unique<Partitioned<stdx::unordered_map<CursorId, ClientCursor*>>>(
          kNumPartitions)) {}

CursorManager::~CursorManager() {
    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& cursor : *partition) {
            // Callers must ensure that no cursors are in use.
            invariant(!cursor.second->_operationUsingCursor);
            cursor.second->dispose(nullptr, boost::none);
            delete cursor.second;
        }
    }
}

bool CursorManager::cursorShouldTimeout_inlock(const ClientCursor* cursor, Date_t now) {
    if (cursor->isNoTimeout() || cursor->_operationUsingCursor ||
        (cursor->getSessionId() && !enableTimeoutOfInactiveSessionCursors.load())) {
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
                LOGV2_DEBUG(8928410,
                            2,
                            "Deregistering timed out cursor",
                            "cursorId"_attr = cursor->cursorid(),
                            "idleSince"_attr = cursor->getLastUseDate());
                removeCursorFromMap(lockedPartition, cursor);
            } else {
                ++it;
            }
        }
    }

    // Be careful not to dispose of cursors while holding the partition lock.
    for (auto&& cursor : toDisposeWithoutMutex) {
        LOGV2(20529,
              "Cursor timed out",
              "cursorId"_attr = cursor->cursorid(),
              "idleSince"_attr = cursor->getLastUseDate());
        cursor->dispose(opCtx, boost::none);
    }
    return toDisposeWithoutMutex.size();
}

std::vector<CursorId> CursorManager::getCursorIdsForNamespace(const NamespaceString& nss) {
    std::vector<CursorId> cursorIds;

    // Lock and inspect one partition at a time in order to avoid contention. It is acceptable for
    // the output not to include info about cursors opened/closed while iterating.
    for (size_t partitionId = 0; partitionId < kNumPartitions; ++partitionId) {
        auto lockedPartition = _cursorMap->lockOnePartitionById(partitionId);
        for (auto it = lockedPartition->begin(); it != lockedPartition->end(); ++it) {
            auto* cursor = it->second;
            if (cursor->nss() == nss) {
                cursorIds.push_back(cursor->cursorid());
            }
        }
    }

    return cursorIds;
}

StatusWith<ClientCursorPin> CursorManager::pinCursor(
    OperationContext* opCtx,
    CursorId id,
    StringData commandName,
    const std::function<void(const ClientCursor&)>& checkPinAllowed,
    AuthCheck checkSessionAuth) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "cursor id " << id << " not found"};
    }

    ClientCursor* cursor = it->second;
    if (cursor->_operationUsingCursor) {
        return Status{CursorInUseInfo{cursor->_commandUsingCursor},
                      str::stream() << "cursor id " << id << " is already in use"};
    }
    if (cursor->getExecutor()->isMarkedAsKilled()) {
        // This cursor was killed while it was idle.
        Status error = cursor->getExecutor()->getKillStatus();
        LOGV2_DEBUG(8928403,
                    2,
                    "Cursor was killed while it was idle",
                    "cursorId"_attr = cursor->cursorid(),
                    "killStatus"_attr = error);
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

    if (checkPinAllowed) {
        checkPinAllowed(*cursor);
    }

    // Pass along the original 'planCacheShapeHash', 'planCacheKey', 'queryShapeHash' for slow query
    // logging.
    CurOp::get(opCtx)->debug().planCacheShapeHash = cursor->_planCacheShapeHash;
    CurOp::get(opCtx)->debug().planCacheKey = cursor->_planCacheKey;
    CurOp::get(opCtx)->debug().setQueryShapeHash(opCtx, cursor->_queryShapeHash);

    // Pass along queryStats context so it is retrievable after query execution for storing metrics.
    CurOp::get(opCtx)->debug().queryStatsInfo.keyHash = cursor->_queryStatsKeyHash;
    CurOp::get(opCtx)->debug().queryStatsInfo.willNeverExhaust =
        cursor->_queryStatsWillNeverExhaust;
    // Pass along 'isChangeStreamQuery' for serverStatus metrics.
    CurOp::get(opCtx)->debug().isChangeStreamQuery = cursor->_isChangeStreamQuery;

    cursor->_operationUsingCursor = opCtx;
    cursor->_commandUsingCursor = std::string{commandName};

    // We use pinning of a cursor as a proxy for active, user-initiated use of a cursor.  Therefore,
    // we pass down to the logical session cache and vivify the record (updating last use).
    if (cursor->getSessionId()) {
        auto vivifyCursorStatus =
            LogicalSessionCache::get(opCtx)->vivify(opCtx, cursor->getSessionId().value());
        if (!vivifyCursorStatus.isOK()) {
            return vivifyCursorStatus;
        }
    }

    LOGV2_DEBUG(8928404, 2, "Pinning cursor", "cursorId"_attr = cursor->cursorid());
    auto pin = ClientCursorPin(opCtx, cursor, this);
    return StatusWith(std::move(pin));
}

void CursorManager::unpin(OperationContext* opCtx,
                          std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    // Avoid computing the current time within the critical section.
    auto now = _preciseClockSource->now();

    auto partition = _cursorMap->lockOnePartition(cursor->cursorid());
    invariant(cursor->_operationUsingCursor);

    // We must verify that no interrupts have occurred since we finished building the current
    // batch. Otherwise, the cursor will be checked back in, the interrupted opCtx will be
    // destroyed, and subsequent getMores with a fresh opCtx will succeed.
    auto interruptStatus = cursor->_operationUsingCursor->checkForInterruptNoAssert();
    cursor->_operationUsingCursor = nullptr;
    cursor->_commandUsingCursor = "";
    cursor->_lastUseDate = now;

    // If someone was trying to kill this cursor with a killOp or a killCursors, they are likely
    // interesting in proactively cleaning up that cursor's resources. In these cases, we
    // proactively delete the cursor. In other cases we preserve the error code so that the client
    // will see the reason the cursor was killed when asking for the next batch.
    if (interruptStatus == ErrorCodes::Interrupted || cursor->isKillPending()) {
        LOGV2(20530,
              "Removing cursor after completing batch",
              "cursorId"_attr = cursor->cursorid(),
              "error"_attr = interruptStatus);
        return deregisterAndDestroyCursor(std::move(partition), opCtx, std::move(cursor));
    } else if (!interruptStatus.isOK()) {
        LOGV2_DEBUG(8928402,
                    2,
                    "Marking executor as killed",
                    "cursorId"_attr = cursor->cursorid(),
                    "error"_attr = interruptStatus);
        cursor->getExecutor()->markAsKilled(interruptStatus);
    }

    LOGV2_DEBUG(8928405,
                2,
                "No kill pending, keeping the cursor in cursorMap",
                "cursorId"_attr = cursor->cursorid());
    // The cursor will stay around in '_cursorMap', so release the unique pointer to avoid deleting
    // it. Cast to void to explicitly ignore the return value.
    (void)cursor.release();
}

void CursorManager::appendActiveSessions(LogicalSessionIdSet* lsids) const {
    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : *partition) {
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
        for (auto&& entry : *partition) {
            auto cursor = entry.second;

            // Exclude cursors that this user does not own if auth is enabled.
            if (AuthorizationManager::get(opCtx->getService())->isAuthEnabled() &&
                userMode == MongoProcessInterface::CurrentOpUserMode::kExcludeOthers &&
                !ctxAuth->isCoauthorizedWith(cursor->getAuthenticatedUser())) {
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

stdx::unordered_set<CursorId> CursorManager::getCursorsForSession(
    const LogicalSessionId& lsid) const {
    stdx::unordered_set<CursorId> cursors;

    auto allPartitions = _cursorMap->lockAllPartitions();
    for (auto&& partition : allPartitions) {
        for (auto&& entry : *partition) {
            auto cursor = entry.second;
            if (cursor->getSessionId() == lsid) {
                cursors.insert(cursor->cursorid());
            }
        }
    }

    return cursors;
}

stdx::unordered_set<CursorId> CursorManager::getCursorsForOpKeys(
    const std::vector<OperationKey>& opKeys) const {
    stdx::unordered_set<CursorId> cursors;

    stdx::lock_guard<stdx::mutex> lk(_opKeyMutex);
    for (auto&& opKey : opKeys) {
        if (auto it = _opKeyMap.find(opKey); it != _opKeyMap.end()) {
            for (auto cursor : it->second) {
                cursors.insert(cursor);
            }
        }
    }
    return cursors;
}

size_t CursorManager::numCursors() const {
    return _cursorMap->size();
}

ClientCursorPin CursorManager::registerCursor(OperationContext* opCtx,
                                              ClientCursorParams&& cursorParams) {
    // Avoid computing the current time within the critical section.
    auto now = _preciseClockSource->now();

    // Make sure the PlanExecutor isn't registered, since we will register the ClientCursor wrapping
    // it.
    invariant(cursorParams.exec);
    cursorParams.exec.get_deleter().dismissDisposal();

    // Note we must hold the registration lock from now until insertion into '_cursorMap' to ensure
    // we don't insert two cursors with the same cursor id.
    stdx::lock_guard<stdx::mutex> lock(_registrationLock);
    CursorId cursorId = generic_cursor::allocateCursorId(
        [&](CursorId cursorId) -> bool {
            // Even though we drop the lock on the '_cursorMap' partition, another thread cannot
            // register a cursor with the same id because we still hold '_registrationLock'.
            auto partition = _cursorMap->lockOnePartition(cursorId);
            return partition->count(cursorId) == 0;
        },
        *_random);

    std::unique_ptr<ClientCursor, ClientCursor::Deleter> clientCursor(
        new ClientCursor(std::move(cursorParams), cursorId, opCtx, now));
    clientCursor->_memoryUsageTracker =
        OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx);

    // Transfer ownership of the cursor to '_cursorMap'.
    auto partition = _cursorMap->lockOnePartition(cursorId);
    ClientCursor* unownedCursor = clientCursor.release();
    partition->emplace(cursorId, unownedCursor);

    // If set, store the mapping of OperationKey to the generated CursorID.
    if (auto opKey = opCtx->getOperationKey()) {
        stdx::lock_guard<stdx::mutex> lk(_opKeyMutex);
        auto it = _opKeyMap.find(*opKey);
        if (it != _opKeyMap.end()) {
            it->second.insert(cursorId);
        } else {
            _opKeyMap.emplace(*opKey, std::set<CursorId>{cursorId});
        }
    }

    LOGV2_DEBUG(8928407, 2, "Registered cursor", "cursorId"_attr = unownedCursor->cursorid());
    // Restores the maxTimeMS provided in the cursor generating command in the case it used
    // maxTimeMSOpOnly. This way the pinned cursor will have the leftover time consistent with the
    // maxTimeMS.
    opCtx->restoreMaxTimeMS();
    return ClientCursorPin(opCtx, unownedCursor, this);
}

// Note the following subleties of the implementations of deregisterAndDestroyCursor:
// - We must make sure the cursor is unpinned (by clearing the '_operationUsingCursor' field) before
//   destruction, since it is an error to delete a pinned cursor.
// - In addition, we must deregister the cursor from the manager's map before clearing the
//   '_operationUsingCursor' field, since it is an error to unpin a registered cursor without
//   holidng the appropriate cursor manager mutex. By first deregistering the cursor, we ensure that
//   no other thread can access '_cursor', meaning that it is safe for us to write to
//   '_operationUsingCursor' without holding the CursorManager mutex.
void CursorManager::deregisterAndDestroyCursor(
    OperationContext* opCtx, std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    removeCursorFromMap(_cursorMap, cursor.get());
    _destroyCursor(opCtx, std::move(cursor));
}

void CursorManager::deregisterAndDestroyCursor(
    Partitioned<stdx::unordered_map<CursorId, ClientCursor*>>::OnePartition&& lk,
    OperationContext* opCtx,
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    LOGV2_DEBUG(8928408, 2, "Deregistering cursor", "cursorId"_attr = cursor->cursorid());
    // Restrict the scope of the lock so we can destroy the cursor without holding any cursor
    // manager mutexes.
    {
        auto lockWithRestrictedScope = std::move(lk);
        removeCursorFromMap(lockWithRestrictedScope, cursor.get());
    }
    _destroyCursor(opCtx, std::move(cursor));
}

void CursorManager::_destroyCursor(OperationContext* opCtx,
                                   std::unique_ptr<ClientCursor, ClientCursor::Deleter> cursor) {
    LOGV2_DEBUG(8928406, 2, "Destroying cursor", "cursorId"_attr = cursor->cursorid());

    // Dispose of the cursor without holding any cursor manager mutexes. Disposal of a cursor can
    // require taking lock manager locks, which we want to avoid while holding a mutex. If we did
    // so, any caller of a CursorManager method which already held a lock manager lock could induce
    // a deadlock when trying to acquire a CursorManager lock.
    cursor->dispose(opCtx, _preciseClockSource->now());
    cursor->_operationUsingCursor = nullptr;
}

Status CursorManager::killCursor(OperationContext* opCtx, CursorId id) {
    return _killCursor(opCtx, id, /*authChecker*/ {});
}

Status CursorManager::killCursorWithAuthCheck(
    OperationContext* opCtx,
    CursorId id,
    const std::function<void(const ClientCursor&)>& authChecker) {
    return _killCursor(opCtx, id, authChecker);
}

Status CursorManager::_killCursor(OperationContext* opCtx,
                                  CursorId id,
                                  const std::function<void(const ClientCursor&)>& authChecker) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "Cursor id not found: " << id};
    }
    auto cursor = it->second;

    if (authChecker) {
        authChecker(*cursor);
    }

    generic_cursor::validateKillInTransaction(
        opCtx, id, cursor->getSessionId(), cursor->getTxnNumber());

    if (cursor->_operationUsingCursor) {
        // Rather than removing the cursor directly, kill the operation that's currently using the
        // cursor. It will stop on its own (and remove the cursor) when it sees that it's been
        // interrupted.
        {
            ClientLock lk(cursor->_operationUsingCursor->getClient());
            cursor->_operationUsingCursor->getServiceContext()->killOperation(
                lk, cursor->_operationUsingCursor, ErrorCodes::CursorKilled);
        }

        LOGV2_DEBUG(8928409,
                    2,
                    "Killed operation using cursor, marking cursor as killPending",
                    "cursorId"_attr = cursor->cursorid());
        // Mark that the cursor has been killed on the cursor object itself as well, as other errors
        // e.g. MaxTimeMSExpired may override the CursorKilled status.
        cursor->setKillPending(true);
        return Status::OK();
    }
    std::unique_ptr<ClientCursor, ClientCursor::Deleter> ownedCursor(cursor);
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
    return auth::checkAuthForKillCursors(as, cursor->nss(), cursor->getAuthenticatedUser());
}

Status CursorManager::checkAuthForReleaseMemory(OperationContext* opCtx, CursorId id) {
    auto lockedPartition = _cursorMap->lockOnePartition(id);
    auto it = lockedPartition->find(id);
    if (it == lockedPartition->end()) {
        return {ErrorCodes::CursorNotFound, str::stream() << "cursor id " << id << " not found"};
    }

    ClientCursor* cursor = it->second;
    AuthorizationSession* as = AuthorizationSession::get(opCtx->getClient());
    return auth::checkAuthForReleaseMemory(as, cursor->nss());
}

}  // namespace mongo
