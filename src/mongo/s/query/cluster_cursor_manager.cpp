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

#include "mongo/s/query/cluster_cursor_manager.h"

#include <set>

#include "mongo/db/kill_sessions_common.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

//
// Helpers to construct a user-friendly error Status from a (nss, cursorId) pair.
//

Status cursorNotFoundStatus(const NamespaceString& nss, CursorId cursorId) {
    return {ErrorCodes::CursorNotFound,
            str::stream() << "Cursor not found (namespace: '" << nss.ns() << "', id: " << cursorId
                          << ")."};
}

Status cursorInUseStatus(const NamespaceString& nss, CursorId cursorId) {
    return {ErrorCodes::CursorInUse,
            str::stream() << "Cursor already in use (namespace: '" << nss.ns()
                          << "', id: " << cursorId << ")."};
}

//
// CursorId is a 64-bit type, made up of a 32-bit prefix and a 32-bit suffix.  The below helpers
// convert between a CursorId and its prefix/suffix.
//

CursorId createCursorId(uint32_t prefix, uint32_t suffix) {
    return (static_cast<uint64_t>(prefix) << 32) | suffix;
}

uint32_t extractPrefixFromCursorId(CursorId cursorId) {
    return static_cast<uint64_t>(cursorId) >> 32;
}

}  // namespace

ClusterCursorManager::PinnedCursor::PinnedCursor(ClusterCursorManager* manager,
                                                 std::unique_ptr<ClusterClientCursor> cursor,
                                                 const NamespaceString& nss,
                                                 CursorId cursorId)
    : _manager(manager), _cursor(std::move(cursor)), _nss(nss), _cursorId(cursorId) {
    invariant(_manager);
    invariant(_cursor);
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

StatusWith<ClusterQueryResult> ClusterCursorManager::PinnedCursor::next(
    RouterExecStage::ExecContext execContext) {
    invariant(_cursor);
    return _cursor->next(execContext);
}

bool ClusterCursorManager::PinnedCursor::isTailable() const {
    invariant(_cursor);
    return _cursor->isTailable();
}

bool ClusterCursorManager::PinnedCursor::isTailableAndAwaitData() const {
    invariant(_cursor);
    return _cursor->isTailableAndAwaitData();
}

boost::optional<ReadPreferenceSetting> ClusterCursorManager::PinnedCursor::getReadPreference()
    const {
    invariant(_cursor);
    return _cursor->getReadPreference();
}

void ClusterCursorManager::PinnedCursor::returnCursor(CursorState cursorState) {
    invariant(_cursor);
    // Note that unpinning a cursor transfers ownership of the underlying ClusterClientCursor object
    // back to the manager.
    _manager->checkInCursor(std::move(_cursor), _nss, _cursorId, cursorState);
    *this = PinnedCursor();
}

BSONObj ClusterCursorManager::PinnedCursor::getOriginatingCommand() const {
    invariant(_cursor);
    return _cursor->getOriginatingCommand();
}

const PrivilegeVector& ClusterCursorManager::PinnedCursor::getOriginatingPrivileges() const& {
    invariant(_cursor);
    return _cursor->getOriginatingPrivileges();
}

std::size_t ClusterCursorManager::PinnedCursor::getNumRemotes() const {
    invariant(_cursor);
    return _cursor->getNumRemotes();
}

BSONObj ClusterCursorManager::PinnedCursor::getPostBatchResumeToken() const {
    invariant(_cursor);
    return _cursor->getPostBatchResumeToken();
}

CursorId ClusterCursorManager::PinnedCursor::getCursorId() const {
    return _cursorId;
}

long long ClusterCursorManager::PinnedCursor::getNumReturnedSoFar() const {
    invariant(_cursor);
    return _cursor->getNumReturnedSoFar();
}

Date_t ClusterCursorManager::PinnedCursor::getLastUseDate() const {
    invariant(_cursor);
    return _cursor->getLastUseDate();
}

void ClusterCursorManager::PinnedCursor::setLastUseDate(Date_t now) {
    invariant(_cursor);
    _cursor->setLastUseDate(now);
}
Date_t ClusterCursorManager::PinnedCursor::getCreatedDate() const {
    invariant(_cursor);
    return _cursor->getCreatedDate();
}
void ClusterCursorManager::PinnedCursor::incNBatches() {
    invariant(_cursor);
    return _cursor->incNBatches();
}

long long ClusterCursorManager::PinnedCursor::getNBatches() const {
    invariant(_cursor);
    return _cursor->getNBatches();
}

void ClusterCursorManager::PinnedCursor::queueResult(const ClusterQueryResult& result) {
    invariant(_cursor);
    _cursor->queueResult(result);
}

bool ClusterCursorManager::PinnedCursor::remotesExhausted() {
    invariant(_cursor);
    return _cursor->remotesExhausted();
}

GenericCursor ClusterCursorManager::PinnedCursor::toGenericCursor() const {
    GenericCursor gc;
    gc.setCursorId(getCursorId());
    gc.setNs(_nss);
    gc.setLsid(getLsid());
    gc.setNDocsReturned(getNumReturnedSoFar());
    gc.setTailable(isTailable());
    gc.setAwaitData(isTailableAndAwaitData());
    gc.setOriginatingCommand(getOriginatingCommand());
    gc.setLastAccessDate(getLastUseDate());
    gc.setCreatedDate(getCreatedDate());
    gc.setNBatchesReturned(getNBatches());
    return gc;
}

Status ClusterCursorManager::PinnedCursor::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    invariant(_cursor);
    return _cursor->setAwaitDataTimeout(awaitDataTimeout);
}

void ClusterCursorManager::PinnedCursor::returnAndKillCursor() {
    invariant(_cursor);

    // Return the cursor as exhausted so that it's deleted immediately.
    returnCursor(CursorState::Exhausted);
}

boost::optional<LogicalSessionId> ClusterCursorManager::PinnedCursor::getLsid() const {
    invariant(_cursor);
    return _cursor->getLsid();
}

boost::optional<TxnNumber> ClusterCursorManager::PinnedCursor::getTxnNumber() const {
    invariant(_cursor);
    return _cursor->getTxnNumber();
}

ClusterCursorManager::ClusterCursorManager(ClockSource* clockSource)
    : _clockSource(clockSource),
      _pseudoRandom(std::unique_ptr<SecureRandom>(SecureRandom::create())->nextInt64()) {
    invariant(_clockSource);
}

ClusterCursorManager::~ClusterCursorManager() {
    invariant(_cursorIdPrefixToNamespaceMap.empty());
    invariant(_namespaceToContainerMap.empty());
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
    UserNameIterator authenticatedUsers) {
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

    // Find the CursorEntryContainer for this namespace.  If none exists, create one.
    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    if (nsToContainerIt == _namespaceToContainerMap.end()) {
        uint32_t containerPrefix = 0;
        do {
            // The server has always generated positive values for CursorId (which is a signed
            // type), so we use std::abs() here on the prefix for consistency with this historical
            // behavior. If the random number generated is INT_MIN, calling std::abs on it is
            // undefined behavior on 2's complement systems so we need to generate a new number.
            int32_t randomNumber = 0;
            do {
                randomNumber = _pseudoRandom.nextInt32();
            } while (randomNumber == std::numeric_limits<int32_t>::min());
            containerPrefix = static_cast<uint32_t>(std::abs(randomNumber));
        } while (_cursorIdPrefixToNamespaceMap.count(containerPrefix) > 0);
        _cursorIdPrefixToNamespaceMap[containerPrefix] = nss;

        auto emplaceResult =
            _namespaceToContainerMap.emplace(nss, CursorEntryContainer(containerPrefix));
        invariant(emplaceResult.second);
        invariant(_namespaceToContainerMap.size() == _cursorIdPrefixToNamespaceMap.size());

        nsToContainerIt = emplaceResult.first;
    } else {
        invariant(!nsToContainerIt->second.entryMap.empty());  // If exists, shouldn't be empty.
    }
    CursorEntryContainer& container = nsToContainerIt->second;

    // Generate a CursorId (which can't be the invalid value zero).
    CursorEntryMap& entryMap = container.entryMap;
    CursorId cursorId = 0;
    do {
        const uint32_t cursorSuffix = static_cast<uint32_t>(_pseudoRandom.nextInt32());
        cursorId = createCursorId(container.containerPrefix, cursorSuffix);
    } while (cursorId == 0 || entryMap.count(cursorId) > 0);

    // Create a new CursorEntry and register it in the CursorEntryContainer's map.
    auto emplaceResult = entryMap.emplace(
        cursorId,
        CursorEntry(std::move(cursor), cursorType, cursorLifetime, now, authenticatedUsers));
    invariant(emplaceResult.second);

    return cursorId;
}

StatusWith<ClusterCursorManager::PinnedCursor> ClusterCursorManager::checkOutCursor(
    const NamespaceString& nss,
    CursorId cursorId,
    OperationContext* opCtx,
    AuthzCheckFn authChecker,
    AuthCheck checkSessionAuth) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_inShutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot check out cursor as we are in the process of shutting down");
    }

    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    // Check if the user is coauthorized to access this cursor.
    auto authCheckStatus = authChecker(entry->getAuthenticatedUsers());
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
        return cursorInUseStatus(nss, cursorId);
    }

    // Note: due to SERVER-31138, despite putting this in a unique_ptr, it's actually not safe to
    // return before the end of this function. Be careful to avoid any early returns/throws after
    // this point.
    auto cursor = entry->releaseCursor(opCtx);

    // We use pinning of a cursor as a proxy for active, user-initiated use of a cursor.  Therefore,
    // we pass down to the logical session cache and vivify the record (updating last use).
    if (cursor->getLsid()) {
        auto vivifyCursorStatus =
            LogicalSessionCache::get(opCtx)->vivify(opCtx, cursor->getLsid().get());
        if (!vivifyCursorStatus.isOK()) {
            return vivifyCursorStatus;
        }
    }
    cursor->reattachToOperationContext(opCtx);
    return PinnedCursor(this, std::move(cursor), nss, cursorId);
}

void ClusterCursorManager::checkInCursor(std::unique_ptr<ClusterClientCursor> cursor,
                                         const NamespaceString& nss,
                                         CursorId cursorId,
                                         CursorState cursorState) {
    invariant(cursor);
    // Read the clock out of the lock.
    const auto now = _clockSource->now();

    // Detach the cursor from the operation which had checked it out.
    OperationContext* opCtx = cursor->getCurrentOperationContext();
    invariant(opCtx);
    cursor->detachFromOperationContext();
    cursor->setLastUseDate(now);

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    invariant(entry);

    // killPending will be true if killCursor() was called while the cursor was in use.
    const bool killPending = entry->isKillPending();

    entry->setLastActive(now);
    entry->returnCursor(std::move(cursor));

    if (cursorState == CursorState::NotExhausted && !killPending) {
        // The caller may need the cursor again.
        return;
    }

    // After detaching the cursor, the entry will be destroyed.
    entry = nullptr;
    detachAndKillCursor(std::move(lk), opCtx, nss, cursorId);
}

Status ClusterCursorManager::checkAuthForKillCursors(OperationContext* opCtx,
                                                     const NamespaceString& nss,
                                                     CursorId cursorId,
                                                     AuthzCheckFn authChecker) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto entry = _getEntry(lk, nss, cursorId);

    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    // Note that getAuthenticatedUsers() is thread-safe, so it's okay to call even if there's
    // an operation using the cursor.
    return authChecker(entry->getAuthenticatedUsers());
}

void ClusterCursorManager::killOperationUsingCursor(WithLock, CursorEntry* entry) {
    invariant(entry->getOperationUsingCursor());
    // Interrupt any operation currently using the cursor.
    OperationContext* opUsingCursor = entry->getOperationUsingCursor();
    stdx::lock_guard<Client> lk(*opUsingCursor->getClient());
    opUsingCursor->getServiceContext()->killOperation(lk, opUsingCursor, ErrorCodes::CursorKilled);

    // Don't delete the cursor, as an operation is using it. It will be cleaned up when the
    // operation is done.
}

Status ClusterCursorManager::killCursor(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        CursorId cursorId) {
    invariant(opCtx);

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    // Interrupt any operation currently using the cursor, unless if it's the current operation.
    OperationContext* opUsingCursor = entry->getOperationUsingCursor();
    if (opUsingCursor) {
        // The caller shouldn't need to call killCursor on their own cursor.
        invariant(opUsingCursor != opCtx, "Cannot call killCursor() on your own cursor");
        killOperationUsingCursor(lk, entry);
        return Status::OK();
    }

    // No one is using the cursor, so we destroy it.
    detachAndKillCursor(std::move(lk), opCtx, nss, cursorId);

    // We no longer hold the lock here.

    return Status::OK();
}

void ClusterCursorManager::detachAndKillCursor(stdx::unique_lock<stdx::mutex> lk,
                                               OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               CursorId cursorId) {
    auto detachedCursor = _detachCursor(lk, nss, cursorId);
    invariant(detachedCursor.getStatus());

    // Deletion of the cursor can happen out of the lock.
    lk.unlock();
    detachedCursor.getValue()->kill(opCtx);
    detachedCursor.getValue().reset();
}

std::size_t ClusterCursorManager::killMortalCursorsInactiveSince(OperationContext* opCtx,
                                                                 Date_t cutoff) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto pred = [cutoff](CursorId cursorId, const CursorEntry& entry) -> bool {
        bool res = entry.getLifetimeType() == CursorLifetime::Mortal &&
            !entry.getOperationUsingCursor() && entry.getLastActive() <= cutoff;

        if (res) {
            log() << "Cursor id " << cursorId << " timed out, idle since "
                  << entry.getLastActive().toString();
        }

        return res;
    };

    return killCursorsSatisfying(std::move(lk), opCtx, std::move(pred));
}

void ClusterCursorManager::killAllCursors(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto pred = [](CursorId, const CursorEntry&) -> bool { return true; };

    killCursorsSatisfying(std::move(lk), opCtx, std::move(pred));
}

std::size_t ClusterCursorManager::killCursorsSatisfying(
    stdx::unique_lock<stdx::mutex> lk,
    OperationContext* opCtx,
    std::function<bool(CursorId, const CursorEntry&)> pred) {
    invariant(opCtx);
    invariant(lk.owns_lock());
    std::size_t nKilled = 0;

    std::vector<std::unique_ptr<ClusterClientCursor>> cursorsToDestroy;
    auto nsContainerIt = _namespaceToContainerMap.begin();
    while (nsContainerIt != _namespaceToContainerMap.end()) {
        auto&& entryMap = nsContainerIt->second.entryMap;
        auto cursorIdEntryIt = entryMap.begin();
        while (cursorIdEntryIt != entryMap.end()) {
            auto cursorId = cursorIdEntryIt->first;
            auto& entry = cursorIdEntryIt->second;

            if (!pred(cursorId, entry)) {
                ++cursorIdEntryIt;
                continue;
            }

            ++nKilled;

            if (entry.getOperationUsingCursor()) {
                // Mark the OperationContext using the cursor as killed, and move on.
                killOperationUsingCursor(lk, &entry);
                ++cursorIdEntryIt;
                continue;
            }

            cursorsToDestroy.push_back(entry.releaseCursor(nullptr));

            // Destroy the entry and set the iterator to the next element.
            entryMap.erase(cursorIdEntryIt++);
        }

        if (entryMap.empty()) {
            nsContainerIt = eraseContainer(nsContainerIt);
        } else {
            ++nsContainerIt;
        }
    }

    // Call kill() outside of the lock, as it may require waiting for callbacks to finish.
    lk.unlock();

    for (auto&& cursor : cursorsToDestroy) {
        invariant(cursor.get());
        cursor->kill(opCtx);
    }

    return nKilled;
}

ClusterCursorManager::Stats ClusterCursorManager::stats() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    Stats stats;

    for (auto& nsContainerPair : _namespaceToContainerMap) {
        for (auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            const CursorEntry& entry = cursorIdEntryPair.second;

            if (entry.isKillPending()) {
                // Killed cursors do not count towards the number of pinned cursors or the number of
                // open cursors.
                continue;
            }

            if (entry.getOperationUsingCursor()) {
                ++stats.cursorsPinned;
            }

            switch (entry.getCursorType()) {
                case CursorType::SingleTarget:
                    ++stats.cursorsSingleTarget;
                    break;
                case CursorType::MultiTarget:
                    ++stats.cursorsMultiTarget;
                    break;
            }
        }
    }

    return stats;
}

void ClusterCursorManager::appendActiveSessions(LogicalSessionIdSet* lsids) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (const auto& nsContainerPair : _namespaceToContainerMap) {
        for (const auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            const CursorEntry& entry = cursorIdEntryPair.second;

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
}

GenericCursor ClusterCursorManager::CursorEntry::cursorToGenericCursor(
    CursorId cursorId, const NamespaceString& ns) const {
    invariant(_cursor);
    GenericCursor gc;
    gc.setCursorId(cursorId);
    gc.setNs(ns);
    gc.setCreatedDate(_cursor->getCreatedDate());
    gc.setLastAccessDate(_cursor->getLastUseDate());
    gc.setLsid(_cursor->getLsid());
    gc.setNDocsReturned(_cursor->getNumReturnedSoFar());
    gc.setTailable(_cursor->isTailable());
    gc.setAwaitData(_cursor->isTailableAndAwaitData());
    gc.setOriginatingCommand(_cursor->getOriginatingCommand());
    gc.setNoCursorTimeout(getLifetimeType() == CursorLifetime::Immortal);
    gc.setNBatchesReturned(_cursor->getNBatches());
    return gc;
}

std::vector<GenericCursor> ClusterCursorManager::getIdleCursors(
    const OperationContext* opCtx, MongoProcessInterface::CurrentOpUserMode userMode) const {
    std::vector<GenericCursor> cursors;

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    AuthorizationSession* ctxAuth = AuthorizationSession::get(opCtx->getClient());

    for (const auto& nsContainerPair : _namespaceToContainerMap) {
        for (const auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {

            const CursorEntry& entry = cursorIdEntryPair.second;
            // If auth is enabled, and userMode is allUsers, check if the current user has
            // permission to see this cursor.
            if (ctxAuth->getAuthorizationManager().isAuthEnabled() &&
                userMode == MongoProcessInterface::CurrentOpUserMode::kExcludeOthers &&
                !ctxAuth->isCoauthorizedWith(entry.getAuthenticatedUsers())) {
                continue;
            }
            if (entry.isKillPending() || entry.getOperationUsingCursor()) {
                // Don't include sessions for killed or pinned cursors.
                continue;
            }

            cursors.emplace_back(
                entry.cursorToGenericCursor(cursorIdEntryPair.first, nsContainerPair.first));
        }
    }

    return cursors;
}

std::pair<Status, int> ClusterCursorManager::killCursorsWithMatchingSessions(
    OperationContext* opCtx, const SessionKiller::Matcher& matcher) {
    auto eraser = [&](ClusterCursorManager& mgr, CursorId id) {
        auto cursorNss = getNamespaceForCursorId(id);
        if (!cursorNss) {
            // The cursor manager couldn't find a namespace associated with 'id'. This means the
            // cursor must have already been killed, so we have no more work to do.
            return;
        }
        uassertStatusOK(mgr.killCursor(opCtx, *cursorNss, id));
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

    for (auto&& nsContainerPair : _namespaceToContainerMap) {
        for (auto&& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            const CursorEntry& entry = cursorIdEntryPair.second;

            if (entry.isKillPending()) {
                // Don't include sessions for killed cursors.
                continue;
            }

            auto cursorLsid = entry.getLsid();
            if (lsid == cursorLsid) {
                cursorIds.insert(cursorIdEntryPair.first);
            }
        }
    }

    return cursorIds;
}

boost::optional<NamespaceString> ClusterCursorManager::getNamespaceForCursorId(
    CursorId cursorId) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    const auto it = _cursorIdPrefixToNamespaceMap.find(extractPrefixFromCursorId(cursorId));
    if (it == _cursorIdPrefixToNamespaceMap.end()) {
        return boost::none;
    }
    return it->second;
}

auto ClusterCursorManager::_getEntry(WithLock, NamespaceString const& nss, CursorId cursorId)
    -> CursorEntry* {

    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    if (nsToContainerIt == _namespaceToContainerMap.end()) {
        return nullptr;
    }
    CursorEntryMap& entryMap = nsToContainerIt->second.entryMap;
    auto entryMapIt = entryMap.find(cursorId);
    if (entryMapIt == entryMap.end()) {
        return nullptr;
    }

    return &entryMapIt->second;
}

auto ClusterCursorManager::eraseContainer(NssToCursorContainerMap::iterator it)
    -> NssToCursorContainerMap::iterator {
    auto&& container = it->second;
    auto&& entryMap = container.entryMap;
    invariant(entryMap.empty());

    // This was the last cursor remaining in the given namespace.  Erase all state associated
    // with this namespace.
    size_t numDeleted = _cursorIdPrefixToNamespaceMap.erase(container.containerPrefix);
    invariant(numDeleted == 1);
    _namespaceToContainerMap.erase(it++);
    invariant(_namespaceToContainerMap.size() == _cursorIdPrefixToNamespaceMap.size());
    return it;
}

StatusWith<std::unique_ptr<ClusterClientCursor>> ClusterCursorManager::_detachCursor(
    WithLock lk, NamespaceString const& nss, CursorId cursorId) {

    CursorEntry* entry = _getEntry(lk, nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    if (entry->getOperationUsingCursor()) {
        return cursorInUseStatus(nss, cursorId);
    }

    // Transfer ownership away from the entry.
    std::unique_ptr<ClusterClientCursor> cursor = entry->releaseCursor(nullptr);

    // Destroy the entry.
    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    invariant(nsToContainerIt != _namespaceToContainerMap.end());
    CursorEntryMap& entryMap = nsToContainerIt->second.entryMap;
    size_t eraseResult = entryMap.erase(cursorId);
    invariant(1 == eraseResult);
    if (entryMap.empty()) {
        eraseContainer(nsToContainerIt);
    }

    return std::move(cursor);
}

}  // namespace mongo
