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


#include "mongo/s/query/exec/cluster_cursor_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_in_use_info.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/generic_cursor_utils.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/session/kill_sessions_common.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

#include <type_traits>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

//
// Helpers to construct a user-friendly error Status from a cursorId.
//

Status cursorNotFoundStatus(CursorId cursorId) {
    return {ErrorCodes::CursorNotFound,
            str::stream() << "Cursor not found (id: " << cursorId << ")."};
}

Status cursorInUseStatus(CursorId cursorId, StringData commandUsingCursor) {
    std::string reason = str::stream() << "Cursor already in use (id: " << cursorId << ").";
    if (!commandUsingCursor.empty()) {
        return {CursorInUseInfo(commandUsingCursor), std::move(reason)};
    } else {
        return {ErrorCodes::CursorInUse, std::move(reason)};
    }
}

// TODO SPM-4207 This function should not need to take a CursorId and a namespace, when it already
// takes a cursor.
GenericCursor cursorToGenericCursor(ClusterClientCursor* cursor,
                                    CursorId cursorId,
                                    const NamespaceString& nss) {
    invariant(cursor);
    GenericCursor gc;
    gc.setCursorId(cursorId);
    gc.setNs(nss);
    gc.setLsid(cursor->getLsid());
    gc.setTxnNumber(cursor->getTxnNumber());
    gc.setNDocsReturned(cursor->getNumReturnedSoFar());
    gc.setTailable(cursor->isTailable());
    gc.setAwaitData(cursor->isTailableAndAwaitData());
    gc.setOriginatingCommand(cursor->getOriginatingCommand());
    gc.setLastAccessDate(cursor->getLastUseDate());
    gc.setCreatedDate(cursor->getCreatedDate());
    gc.setNBatchesReturned(cursor->getNBatches());
    if (auto memoryTracker = cursor->getMemoryUsageTracker()) {
        if (auto inUseTrackedMemBytes = memoryTracker->inUseTrackedMemoryBytes()) {
            gc.setInUseTrackedMemBytes(inUseTrackedMemBytes);
        }
        if (auto peakTrackedMemBytes = memoryTracker->peakTrackedMemoryBytes()) {
            gc.setPeakTrackedMemBytes(peakTrackedMemBytes);
        }
    }
    return gc;
}

}  // namespace

/* Explicit instantiation of the templates to have available for linker. */
template Status ClusterCursorManager::checkAuthCursor<AuthzCheckFnInputType>(
    OperationContext* opCtx, CursorId cursorId, AuthzCheckFn func);

template Status ClusterCursorManager::checkAuthCursor<ReleaseMemoryAuthzCheckFnInputType>(
    OperationContext* opCtx, CursorId cursorId, ReleaseMemoryAuthzCheckFn func);

template StatusWith<ClusterCursorManager::PinnedCursor>
ClusterCursorManager::checkOutCursor<AuthzCheckFnInputType>(CursorId cursorId,
                                                            OperationContext* opCtx,
                                                            AuthzCheckFn authChecker,
                                                            AuthCheck checkSessionAuth,
                                                            StringData commandName);

template StatusWith<ClusterCursorManager::PinnedCursor> ClusterCursorManager::checkOutCursor<
    ReleaseMemoryAuthzCheckFnInputType>(CursorId cursorId,
                                        OperationContext* opCtx,
                                        ReleaseMemoryAuthzCheckFn authChecker,
                                        AuthCheck checkSessionAuth,
                                        StringData commandName);


ClusterCursorManager::PinnedCursor::PinnedCursor(ClusterCursorManager* manager,
                                                 ClusterClientCursorGuard&& cursorGuard,
                                                 const NamespaceString& nss,
                                                 CursorId cursorId)
    : _manager(manager), _cursor(cursorGuard.releaseCursor()), _nss(nss), _cursorId(cursorId) {
    invariant(_manager);
    invariant(_cursorId);  // Zero is not a valid cursor id.
}

ClusterCursorManager::PinnedCursor::~PinnedCursor() {
    if (_cursor) {
        // The underlying cursor has not yet been returned.
        returnAndKillCursor();
    }
}

ClusterCursorManager::PinnedCursor::PinnedCursor(PinnedCursor&& other)
    : _manager(std::move(other._manager)),
      _cursor(std::move(other._cursor)),
      _nss(std::move(other._nss)),
      _cursorId(std::move(other._cursorId)) {}

ClusterCursorManager::PinnedCursor& ClusterCursorManager::PinnedCursor::operator=(
    ClusterCursorManager::PinnedCursor&& other) {
    if (_cursor) {
        // The underlying cursor has not yet been returned.
        returnAndKillCursor();
    }
    _manager = std::move(other._manager);
    _cursor = std::move(other._cursor);
    _nss = std::move(other._nss);
    _cursorId = std::move(other._cursorId);
    return *this;
}

void ClusterCursorManager::PinnedCursor::returnCursor(CursorState cursorState) {
    invariant(_cursor);
    // Note that unpinning a cursor transfers ownership of the underlying ClusterClientCursor object
    // back to the manager.
    _manager->checkInCursor(std::move(_cursor), _cursorId, cursorState);
    *this = PinnedCursor();
}

CursorId ClusterCursorManager::PinnedCursor::getCursorId() const {
    return _cursorId;
}

GenericCursor ClusterCursorManager::PinnedCursor::toGenericCursor() const {
    return cursorToGenericCursor(_cursor.get(), getCursorId(), _nss);
}

void ClusterCursorManager::PinnedCursor::returnAndKillCursor() {
    invariant(_cursor);

    // Return the cursor as exhausted so that it's deleted immediately.
    returnCursor(CursorState::Exhausted);
}

ClusterCursorManager::ClusterCursorManager(ClockSource* clockSource)
    : _clockSource(clockSource),
      _randomSeed(SecureRandom().nextInt64()),
      _pseudoRandom(_randomSeed) {
    invariant(_clockSource);
}

ClusterCursorManager::~ClusterCursorManager() {
    invariant(_cursorEntryMap.empty());
}

void ClusterCursorManager::shutdown(OperationContext* opCtx) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _inShutdown = true;
    }
    killAllCursors(opCtx);
}

StatusWith<CursorId> ClusterCursorManager::registerCursor(
    OperationContext* opCtx,
    std::unique_ptr<ClusterClientCursor> cursor,
    const NamespaceString& nss,
    CursorType cursorType,
    CursorLifetime cursorLifetime,
    const boost::optional<UserName>& authenticatedUser) {
    // Read the clock out of the lock.
    const auto now = _clockSource->now();

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (_inShutdown) {
        lk.unlock();
        cursor->kill(opCtx);
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot register new cursors as we are in the process of shutting down");
    }

    invariant(cursor);
    cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

    auto cursorId = generic_cursor::allocateCursorId(
        [&](CursorId cursorId) -> bool { return _cursorEntryMap.count(cursorId) == 0; },
        _pseudoRandom);

    cursor->setMemoryUsageTracker(OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx));

    // Create a new CursorEntry and register it in the CursorEntryContainer's map.
    auto emplaceResult = _cursorEntryMap.emplace(cursorId,
                                                 CursorEntry(std::move(cursor),
                                                             cursorType,
                                                             cursorLifetime,
                                                             now,
                                                             authenticatedUser,
                                                             opCtx->getClient()->getUUID(),
                                                             opCtx->getOperationKey(),
                                                             nss));
    invariant(emplaceResult.second);

    return cursorId;
}

template <typename T>
StatusWith<ClusterCursorManager::PinnedCursor> ClusterCursorManager::checkOutCursor(
    CursorId cursorId,
    OperationContext* opCtx,
    std::function<Status(T)> authChecker,
    AuthCheck checkSessionAuth,
    StringData commandName) {

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_inShutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot check out cursor as we are in the process of shutting down");
    }

    CursorEntry* entry = _getEntry(lk, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(cursorId);
    }

    auto authCheckStatus = AuthzCheckPolicy<T>::authzCheck(entry, authChecker);
    if (!authCheckStatus.isOK()) {
        return authCheckStatus.withContext(str::stream()
                                           << "cursor id " << cursorId
                                           << " was not created by the authenticated user");
    }

    if (checkSessionAuth == kCheckSession) {
        const auto cursorPrivilegeStatus = checkCursorSessionPrivilege(opCtx, entry->getLsid());
        if (!cursorPrivilegeStatus.isOK()) {
            return cursorPrivilegeStatus;
        }
    }

    if (entry->getOperationUsingCursor()) {
        return cursorInUseStatus(cursorId, entry->getCommandUsingCursor());
    }

    auto cursorGuard = entry->releaseCursor(opCtx, commandName);

    // We use pinning of a cursor as a proxy for active, user-initiated use of a cursor. Therefore,
    // we pass down to the logical session cache and vivify the record (updating last use).
    if (cursorGuard->getLsid()) {
        auto vivifyCursorStatus =
            LogicalSessionCache::get(opCtx)->vivify(opCtx, cursorGuard->getLsid().value());
        if (!vivifyCursorStatus.isOK()) {
            return vivifyCursorStatus;
        }
    }
    cursorGuard->reattachToOperationContext(opCtx);

    CurOp::get(opCtx)->debug().planCacheShapeHash = cursorGuard->getPlanCacheShapeHash();
    CurOp::get(opCtx)->debug().queryStatsInfo.keyHash = cursorGuard->getQueryStatsKeyHash();
    CurOp::get(opCtx)->debug().setQueryShapeHash(opCtx, cursorGuard->getQueryShapeHash());

    OperationMemoryUsageTracker::moveToOpCtxIfAvailable(opCtx,
                                                        cursorGuard->releaseMemoryUsageTracker());
    return PinnedCursor(this, std::move(cursorGuard), entry->getNamespace(), cursorId);
}

StatusWith<ClusterCursorManager::PinnedCursor> ClusterCursorManager::checkOutCursorNoAuthCheck(
    CursorId cursorId, OperationContext* opCtx, StringData commandName) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_inShutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot check out cursor as we are in the process of shutting down");
    }

    CursorEntry* entry = _getEntry(lk, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(cursorId);
    }

    if (entry->getOperationUsingCursor()) {
        return cursorInUseStatus(cursorId, entry->getCommandUsingCursor());
    }

    auto cursorGuard = entry->releaseCursor(opCtx, commandName);
    cursorGuard->reattachToOperationContext(opCtx);
    OperationMemoryUsageTracker::moveToOpCtxIfAvailable(opCtx,
                                                        cursorGuard->releaseMemoryUsageTracker());

    return PinnedCursor(this, std::move(cursorGuard), entry->getNamespace(), cursorId);
}

void ClusterCursorManager::checkInCursor(std::unique_ptr<ClusterClientCursor> cursor,
                                         CursorId cursorId,
                                         CursorState cursorState,
                                         bool isReleaseMemory) {
    invariant(cursor);
    // Read the clock out of the lock.
    const auto now = _clockSource->now();

    // Detach the cursor from the operation which had checked it out.
    OperationContext* opCtx = cursor->getCurrentOperationContext();
    invariant(opCtx);
    cursor->detachFromOperationContext();
    if (!isReleaseMemory) {
        cursor->setLastUseDate(now);
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    CursorEntry* entry = _getEntry(lk, cursorId);
    invariant(entry);

    // killPending will be true if killCursor() was called while the cursor was in use.
    const bool killPending = entry->isKillPending();

    if (!isReleaseMemory) {
        entry->setLastActive(now);
    }

    if (cursorState == CursorState::NotExhausted && !killPending) {
        cursor->setMemoryUsageTracker(OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx));
    }

    entry->returnCursor(std::move(cursor));

    if (cursorState == CursorState::NotExhausted && !killPending) {
        // The caller may need the cursor again.
        return;
    }

    // After detaching the cursor, the entry will be destroyed.
    entry = nullptr;
    detachAndKillCursor(std::move(lk), opCtx, cursorId);
}

template <typename T>
Status ClusterCursorManager::checkAuthCursor(OperationContext* opCtx,
                                             CursorId cursorId,
                                             std::function<Status(T)> authChecker) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto entry = _getEntry(lk, cursorId);

    if (!entry) {
        return cursorNotFoundStatus(cursorId);
    }

    return AuthzCheckPolicy<T>::authzCheck(entry, authChecker);
}

void ClusterCursorManager::killOperationUsingCursor(WithLock, CursorEntry* entry) {
    invariant(entry->getOperationUsingCursor());
    // Interrupt any operation currently using the cursor.
    OperationContext* opUsingCursor = entry->getOperationUsingCursor();
    LOGV2_DEBUG(8928412,
                2,
                "Killing operation using cursor",
                "operationId"_attr = opUsingCursor->getOpID());

    ClientLock lk(opUsingCursor->getClient());
    opUsingCursor->getServiceContext()->killOperation(lk, opUsingCursor, ErrorCodes::CursorKilled);

    // Don't delete the cursor, as an operation is using it. It will be cleaned up when the
    // operation is done.
}

Status ClusterCursorManager::killCursor(OperationContext* opCtx, CursorId cursorId) {
    AuthzCheckFn passingAuthChecker = [](AuthzCheckFnInputType) -> Status {
        return Status::OK();
    };
    return _killCursor(opCtx, cursorId, passingAuthChecker);
}

Status ClusterCursorManager::killCursorWithAuthCheck(OperationContext* opCtx,
                                                     CursorId cursorId,
                                                     AuthzCheckFn authChecker) {
    return _killCursor(opCtx, cursorId, std::move(authChecker));
}

Status ClusterCursorManager::_killCursor(OperationContext* opCtx,
                                         CursorId cursorId,
                                         AuthzCheckFn authChecker) {
    invariant(opCtx);

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    CursorEntry* entry = _getEntry(lk, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(cursorId);
    }

    auto authCheckStatus = authChecker(entry->getAuthenticatedUser());
    if (!authCheckStatus.isOK()) {
        return authCheckStatus.withContext(str::stream()
                                           << "cursor id " << cursorId
                                           << " was not created by the authenticated user");
    }

    generic_cursor::validateKillInTransaction(
        opCtx, cursorId, entry->getLsid(), entry->getTxnNumber());

    // Interrupt any operation currently using the cursor, unless if it's the current operation.
    OperationContext* opUsingCursor = entry->getOperationUsingCursor();
    if (opUsingCursor) {
        // The caller shouldn't need to call killCursor on their own cursor.
        invariant(opUsingCursor != opCtx, "Cannot call killCursor() on your own cursor");
        killOperationUsingCursor(lk, entry);
        return Status::OK();
    }
    // No one is using the cursor, so we destroy it.
    detachAndKillCursor(std::move(lk), opCtx, cursorId);

    // We no longer hold the lock here.

    return Status::OK();
}

void ClusterCursorManager::detachAndKillCursor(stdx::unique_lock<stdx::mutex> lk,
                                               OperationContext* opCtx,
                                               CursorId cursorId) {
    LOGV2_DEBUG(8928411, 2, "Detaching and killing cursor", "cursorId"_attr = cursorId);
    auto detachedCursorGuard = _detachCursor(lk, opCtx, cursorId);
    invariant(detachedCursorGuard.getStatus());

    // Deletion of the cursor can happen out of the lock.
    lk.unlock();
    detachedCursorGuard.getValue()->kill(opCtx);
}

std::size_t ClusterCursorManager::killMortalCursorsInactiveSince(OperationContext* opCtx,
                                                                 Date_t cutoff) {
    auto cursorsKilled =
        killCursorsSatisfying(opCtx, [cutoff](CursorId cursorId, const CursorEntry& entry) -> bool {
            if (entry.getLifetimeType() == CursorLifetime::Immortal ||
                entry.getOperationUsingCursor() ||
                (entry.getLsid() && !enableTimeoutOfInactiveSessionCursors.load())) {
                return false;
            }

            bool res = entry.getLastActive() <= cutoff;

            if (res) {
                LOGV2(22837,
                      "Cursor timed out",
                      "cursorId"_attr = cursorId,
                      "idleSince"_attr = entry.getLastActive().toString());
            }

            return res;
        });

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _cursorsTimedOut += cursorsKilled;

    return cursorsKilled;
}

void ClusterCursorManager::killAllCursors(OperationContext* opCtx) {
    killCursorsSatisfying(opCtx, [](CursorId, const CursorEntry&) -> bool { return true; });
}

std::size_t ClusterCursorManager::killCursorsSatisfying(
    OperationContext* opCtx, const std::function<bool(CursorId, const CursorEntry&)>& pred) {
    invariant(opCtx);
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    std::size_t nKilled = 0;

    std::vector<ClusterClientCursorGuard> cursorsToDestroy;
    auto cursorIdEntryIt = _cursorEntryMap.begin();
    while (cursorIdEntryIt != _cursorEntryMap.end()) {
        auto cursorId = cursorIdEntryIt->first;
        auto& entry = cursorIdEntryIt->second;

        if (!pred(cursorId, entry)) {
            ++cursorIdEntryIt;
            continue;
        }

        ++nKilled;

        LOGV2_DEBUG(8928413, 2, "Killing cursor", "cursorId"_attr = cursorId);

        if (entry.getOperationUsingCursor()) {
            // Mark the OperationContext using the cursor as killed, and move on.
            killOperationUsingCursor(lk, &entry);
            ++cursorIdEntryIt;
            continue;
        }

        cursorsToDestroy.push_back(entry.releaseCursor(opCtx));

        // Destroy the entry and set the iterator to the next element.
        _cursorEntryMap.erase(cursorIdEntryIt++);
    }

    // Ensure cursors are killed outside the lock, as killing may require waiting for callbacks to
    // finish.
    lk.unlock();

    for (auto&& cursorGuard : cursorsToDestroy) {
        invariant(cursorGuard);
        cursorGuard->kill(opCtx);
    }

    return nKilled;
}

size_t ClusterCursorManager::cursorsTimedOut() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _cursorsTimedOut;
}

auto ClusterCursorManager::getOpenCursorStats() const -> OpenCursorStats {
    OpenCursorStats stats{};
    stdx::lock_guard lk(_mutex);
    for (auto&& [cursorId, entry] : _cursorEntryMap) {
        if (entry.isKillPending())
            continue;
        if (entry.getOperationUsingCursor())
            ++stats.pinned;
        switch (entry.getCursorType()) {
            case CursorType::SingleTarget:
                ++stats.singleTarget;
                break;
            case CursorType::MultiTarget:
                ++stats.multiTarget;
                break;
            case CursorType::QueuedData:
                ++stats.queuedData;
                break;
        }
    }
    return stats;
}

void ClusterCursorManager::appendActiveSessions(LogicalSessionIdSet* lsids) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (auto&& [cursorId, entry] : _cursorEntryMap) {
        if (entry.isKillPending()) {
            // Don't include sessions for killed cursors.
            continue;
        }

        auto lsid = entry.getLsid();
        if (lsid) {
            lsids->insert(*lsid);
        }
    }
}

GenericCursor ClusterCursorManager::CursorEntry::cursorToGenericCursor(
    CursorId cursorId, const NamespaceString& ns) const {
    GenericCursor gc = ::mongo::cursorToGenericCursor(_cursor.get(), cursorId, ns);
    gc.setNoCursorTimeout(getLifetimeType() == CursorLifetime::Immortal);
    return gc;
}

std::vector<GenericCursor> ClusterCursorManager::getIdleCursors(
    const OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode) const {
    std::vector<GenericCursor> cursors;

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    for (auto&& [cursorId, entry] : _cursorEntryMap) {
        // If auth is enabled, and userMode is allUsers, check if the current user has
        // permission to see this cursor.
        if (AuthorizationManager::get(opCtx->getService())->isAuthEnabled() &&
            userMode == MongoProcessInterface::CurrentOpUserMode::kExcludeOthers &&
            !ctxAuth->isCoauthorizedWith(entry.getAuthenticatedUser())) {
            continue;
        }
        if (entry.isKillPending() || entry.getOperationUsingCursor()) {
            // Don't include sessions for killed or pinned cursors.
            continue;
        }

        cursors.emplace_back(entry.cursorToGenericCursor(cursorId, entry.getNamespace()));
    }


    return cursors;
}

std::pair<Status, int> ClusterCursorManager::killCursorsWithMatchingSessions(
    OperationContext* opCtx, const SessionKiller::Matcher& matcher) {
    auto eraser = [&](ClusterCursorManager& mgr, CursorId id) {
        uassertStatusOK(mgr.killCursor(opCtx, id));
        LOGV2(22838, "Killing cursor as part of killing session(s)", "cursorId"_attr = id);
    };

    auto bySessionCursorKiller = makeKillCursorsBySessionAdaptor(opCtx, matcher, std::move(eraser));
    bySessionCursorKiller(*this);
    return std::make_pair(bySessionCursorKiller.getStatus(),
                          bySessionCursorKiller.getCursorsKilled());
}

stdx::unordered_set<CursorId> ClusterCursorManager::getCursorsForSession(
    LogicalSessionId lsid) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    stdx::unordered_set<CursorId> cursorIds;

    for (auto&& [cursorId, entry] : _cursorEntryMap) {
        if (entry.isKillPending()) {
            // Don't include sessions for killed cursors.
            continue;
        }

        auto cursorLsid = entry.getLsid();
        if (lsid == cursorLsid) {
            cursorIds.insert(cursorId);
        }
    }

    return cursorIds;
}

auto ClusterCursorManager::_getEntry(WithLock, CursorId cursorId) -> CursorEntry* {
    auto entryMapIt = _cursorEntryMap.find(cursorId);
    if (entryMapIt == _cursorEntryMap.end()) {
        return nullptr;
    }

    return &entryMapIt->second;
}

StatusWith<ClusterClientCursorGuard> ClusterCursorManager::_detachCursor(WithLock lk,
                                                                         OperationContext* opCtx,
                                                                         CursorId cursorId) {
    CursorEntry* entry = _getEntry(lk, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(cursorId);
    }

    if (entry->getOperationUsingCursor()) {
        return cursorInUseStatus(cursorId, entry->getCommandUsingCursor());
    }

    // Transfer ownership away from the entry.
    ClusterClientCursorGuard cursor = entry->releaseCursor(opCtx);

    // Destroy the entry.
    size_t eraseResult = _cursorEntryMap.erase(cursorId);
    invariant(1 == eraseResult);

    return std::move(cursor);
}

}  // namespace mongo
