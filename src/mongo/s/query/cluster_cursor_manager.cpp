/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_cursor_manager.h"

#include <set>

#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

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
            str::stream() << "Cursor already in use (namespace: '" << nss.ns() << "', id: "
                          << cursorId
                          << ")."};
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

StatusWith<boost::optional<BSONObj>> ClusterCursorManager::PinnedCursor::next() {
    invariant(_cursor);
    return _cursor->next();
}

bool ClusterCursorManager::PinnedCursor::isTailable() const {
    invariant(_cursor);
    return _cursor->isTailable();
}

void ClusterCursorManager::PinnedCursor::returnCursor(CursorState cursorState) {
    invariant(_cursor);
    // Note that unpinning a cursor transfers ownership of the underlying ClusterClientCursor object
    // back to the manager.
    _manager->checkInCursor(std::move(_cursor), _nss, _cursorId, cursorState);
    *this = PinnedCursor();
}

CursorId ClusterCursorManager::PinnedCursor::getCursorId() const {
    return _cursorId;
}

long long ClusterCursorManager::PinnedCursor::getNumReturnedSoFar() const {
    invariant(_cursor);
    return _cursor->getNumReturnedSoFar();
}

void ClusterCursorManager::PinnedCursor::queueResult(const BSONObj& obj) {
    invariant(_cursor);
    _cursor->queueResult(obj);
}

bool ClusterCursorManager::PinnedCursor::remotesExhausted() {
    invariant(_cursor);
    return _cursor->remotesExhausted();
}

Status ClusterCursorManager::PinnedCursor::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    invariant(_cursor);
    return _cursor->setAwaitDataTimeout(awaitDataTimeout);
}

void ClusterCursorManager::PinnedCursor::returnAndKillCursor() {
    invariant(_cursor);

    // Inform the manager that the cursor should be killed.
    invariantOK(_manager->killCursor(_nss, _cursorId));

    // Return the cursor to the manager.  It will be deleted on the next call to
    // ClusterCursorManager::reapZombieCursors().
    //
    // The value of the argument to returnCursor() doesn't matter; the cursor will be kept as a
    // zombie.
    returnCursor(CursorState::NotExhausted);
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

void ClusterCursorManager::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _inShutdown = true;
    lk.unlock();

    killAllCursors();
    reapZombieCursors();
}

StatusWith<CursorId> ClusterCursorManager::registerCursor(
    std::unique_ptr<ClusterClientCursor> cursor,
    const NamespaceString& nss,
    CursorType cursorType,
    CursorLifetime cursorLifetime) {
    // Read the clock out of the lock.
    const auto now = _clockSource->now();

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (_inShutdown) {
        lk.unlock();
        cursor->kill();
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot register new cursors as we are in the process of shutting down");
    }

    invariant(cursor);

    // Find the CursorEntryContainer for this namespace.  If none exists, create one.
    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    if (nsToContainerIt == _namespaceToContainerMap.end()) {
        uint32_t containerPrefix = 0;
        do {
            // The server has always generated positive values for CursorId (which is a signed
            // type), so we use std::abs() here on the prefix for consistency with this historical
            // behavior.
            containerPrefix = static_cast<uint32_t>(std::abs(_pseudoRandom.nextInt32()));
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
    auto emplaceResult =
        entryMap.emplace(cursorId, CursorEntry(std::move(cursor), cursorType, cursorLifetime, now));
    invariant(emplaceResult.second);

    return cursorId;
}

StatusWith<ClusterCursorManager::PinnedCursor> ClusterCursorManager::checkOutCursor(
    const NamespaceString& nss, CursorId cursorId) {
    // Read the clock out of the lock.
    const auto now = _clockSource->now();

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_inShutdown) {
        return Status(ErrorCodes::ShutdownInProgress,
                      "Cannot check out cursor as we are in the process of shutting down");
    }

    CursorEntry* entry = getEntry_inlock(nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }
    if (entry->getKillPending()) {
        return cursorNotFoundStatus(nss, cursorId);
    }
    std::unique_ptr<ClusterClientCursor> cursor = entry->releaseCursor();
    if (!cursor) {
        return cursorInUseStatus(nss, cursorId);
    }

    entry->setLastActive(now);

    // Note that pinning a cursor transfers ownership of the underlying ClusterClientCursor object
    // to the pin; the CursorEntry is left with a null ClusterClientCursor.
    return PinnedCursor(this, std::move(cursor), nss, cursorId);
}

void ClusterCursorManager::checkInCursor(std::unique_ptr<ClusterClientCursor> cursor,
                                         const NamespaceString& nss,
                                         CursorId cursorId,
                                         CursorState cursorState) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    invariant(cursor);

    const bool remotesExhausted = cursor->remotesExhausted();

    CursorEntry* entry = getEntry_inlock(nss, cursorId);
    invariant(entry);

    entry->returnCursor(std::move(cursor));

    if (cursorState == CursorState::NotExhausted || entry->getKillPending()) {
        return;
    }

    if (!remotesExhausted) {
        // The cursor still has open remote cursors that need to be cleaned up. Schedule for
        // deletion by the reaper thread by setting the kill pending flag.
        entry->setKillPending();
        return;
    }

    // The cursor is exhausted, is not already scheduled for deletion, and does not have any
    // remote cursor state left to clean up. We can delete the cursor right away.
    auto detachedCursor = detachCursor_inlock(nss, cursorId);
    invariantOK(detachedCursor.getStatus());

    // Deletion of the cursor can happen out of the lock.
    lk.unlock();
    detachedCursor.getValue().reset();
}

Status ClusterCursorManager::killCursor(const NamespaceString& nss, CursorId cursorId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    CursorEntry* entry = getEntry_inlock(nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    entry->setKillPending();

    return Status::OK();
}

void ClusterCursorManager::killMortalCursorsInactiveSince(Date_t cutoff) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (auto& nsContainerPair : _namespaceToContainerMap) {
        for (auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            CursorEntry& entry = cursorIdEntryPair.second;
            if (entry.getLifetimeType() == CursorLifetime::Mortal &&
                entry.getLastActive() <= cutoff) {
                entry.setInactive();
                log() << "Marking cursor id " << cursorIdEntryPair.first
                      << " for deletion, idle since " << entry.getLastActive().toString();
                entry.setKillPending();
            }
        }
    }
}

void ClusterCursorManager::killAllCursors() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    for (auto& nsContainerPair : _namespaceToContainerMap) {
        for (auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            cursorIdEntryPair.second.setKillPending();
        }
    }
}

std::size_t ClusterCursorManager::reapZombieCursors() {
    struct CursorDescriptor {
        CursorDescriptor(NamespaceString ns, CursorId cursorId, bool isInactive)
            : ns(std::move(ns)), cursorId(cursorId), isInactive(isInactive) {}

        NamespaceString ns;
        CursorId cursorId;
        bool isInactive;
    };

    // List all zombie cursors under the manager lock, and kill them one-by-one while not holding
    // the lock (ClusterClientCursor::kill() is blocking, so we don't want to hold a lock while
    // issuing the kill).

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    std::vector<CursorDescriptor> zombieCursorDescriptors;
    for (auto& nsContainerPair : _namespaceToContainerMap) {
        const NamespaceString& nss = nsContainerPair.first;
        for (auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            CursorId cursorId = cursorIdEntryPair.first;
            const CursorEntry& entry = cursorIdEntryPair.second;
            if (!entry.getKillPending()) {
                continue;
            }
            zombieCursorDescriptors.emplace_back(nss, cursorId, entry.isInactive());
        }
    }

    std::size_t cursorsTimedOut = 0;

    for (auto& cursorDescriptor : zombieCursorDescriptors) {
        StatusWith<std::unique_ptr<ClusterClientCursor>> zombieCursor =
            detachCursor_inlock(cursorDescriptor.ns, cursorDescriptor.cursorId);
        if (!zombieCursor.isOK()) {
            // Cursor in use, or has already been deleted.
            continue;
        }

        lk.unlock();
        zombieCursor.getValue()->kill();
        zombieCursor.getValue().reset();
        lk.lock();

        if (cursorDescriptor.isInactive) {
            ++cursorsTimedOut;
        }
    }
    return cursorsTimedOut;
}

ClusterCursorManager::Stats ClusterCursorManager::stats() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    Stats stats;

    for (auto& nsContainerPair : _namespaceToContainerMap) {
        for (auto& cursorIdEntryPair : nsContainerPair.second.entryMap) {
            const CursorEntry& entry = cursorIdEntryPair.second;

            if (entry.getKillPending()) {
                // Killed cursors do not count towards the number of pinned cursors or the number of
                // open cursors.
                continue;
            }

            if (!entry.isCursorOwned()) {
                ++stats.cursorsPinned;
            }

            switch (entry.getCursorType()) {
                case CursorType::NamespaceNotSharded:
                    ++stats.cursorsNotSharded;
                    break;
                case CursorType::NamespaceSharded:
                    ++stats.cursorsSharded;
                    break;
            }
        }
    }

    return stats;
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

ClusterCursorManager::CursorEntry* ClusterCursorManager::getEntry_inlock(const NamespaceString& nss,
                                                                         CursorId cursorId) {
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

StatusWith<std::unique_ptr<ClusterClientCursor>> ClusterCursorManager::detachCursor_inlock(
    const NamespaceString& nss, CursorId cursorId) {
    CursorEntry* entry = getEntry_inlock(nss, cursorId);
    if (!entry) {
        return cursorNotFoundStatus(nss, cursorId);
    }

    std::unique_ptr<ClusterClientCursor> cursor = entry->releaseCursor();
    if (!cursor) {
        return cursorInUseStatus(nss, cursorId);
    }

    auto nsToContainerIt = _namespaceToContainerMap.find(nss);
    invariant(nsToContainerIt != _namespaceToContainerMap.end());
    CursorEntryMap& entryMap = nsToContainerIt->second.entryMap;
    size_t eraseResult = entryMap.erase(cursorId);
    invariant(1 == eraseResult);
    if (entryMap.empty()) {
        // This was the last cursor remaining in the given namespace.  Erase all state associated
        // with this namespace.
        size_t numDeleted =
            _cursorIdPrefixToNamespaceMap.erase(nsToContainerIt->second.containerPrefix);
        invariant(numDeleted == 1);
        _namespaceToContainerMap.erase(nsToContainerIt);
        invariant(_namespaceToContainerMap.size() == _cursorIdPrefixToNamespaceMap.size());
    }

    return std::move(cursor);
}

}  // namespace mongo
