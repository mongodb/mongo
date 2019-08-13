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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/lock_state.h"

#include <vector>

#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/new.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(failNonIntentLocksIfWaitNeeded);

namespace {

/**
 * Tracks global (across all clients) lock acquisition statistics, partitioned into multiple
 * buckets to minimize concurrent access conflicts.
 *
 * Each client has a LockerId that monotonically increases across all client instances. The
 * LockerId % 8 is used to index into one of 8 LockStats instances. These LockStats objects must be
 * atomically accessed, so maintaining 8 that are indexed by LockerId reduces client conflicts and
 * improves concurrent write access. A reader, to collect global lock statics for reporting, will
 * sum the results of all 8 disjoint 'buckets' of stats.
 */
class PartitionedInstanceWideLockStats {
    PartitionedInstanceWideLockStats(const PartitionedInstanceWideLockStats&) = delete;
    PartitionedInstanceWideLockStats& operator=(const PartitionedInstanceWideLockStats&) = delete;

public:
    PartitionedInstanceWideLockStats() {}

    void recordAcquisition(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordAcquisition(resId, mode);
    }

    void recordWait(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordWait(resId, mode);
    }

    void recordWaitTime(LockerId id, ResourceId resId, LockMode mode, uint64_t waitMicros) {
        _get(id).recordWaitTime(resId, mode, waitMicros);
    }

    void report(SingleThreadedLockStats* outStats) const {
        for (int i = 0; i < NumPartitions; i++) {
            outStats->append(_partitions[i].stats);
        }
    }

    void reset() {
        for (int i = 0; i < NumPartitions; i++) {
            _partitions[i].stats.reset();
        }
    }

private:
    // This alignment is a best effort approach to ensure that each partition falls on a
    // separate page/cache line in order to avoid false sharing.
    struct alignas(stdx::hardware_destructive_interference_size) AlignedLockStats {
        AtomicLockStats stats;
    };

    enum { NumPartitions = 8 };


    AtomicLockStats& _get(LockerId id) {
        return _partitions[id % NumPartitions].stats;
    }


    AlignedLockStats _partitions[NumPartitions];
};


// Global lock manager instance.
LockManager globalLockManager;

// How often (in millis) to check for deadlock if a lock has not been granted for some time
const Milliseconds MaxWaitTime = Milliseconds(500);

// Dispenses unique LockerId identifiers
AtomicWord<unsigned long long> idCounter(0);

// Tracks lock statistics across all Locker instances. Distributes stats across multiple buckets
// indexed by LockerId in order to minimize concurrent access conflicts.
PartitionedInstanceWideLockStats globalStats;

}  // namespace

bool LockerImpl::_shouldDelayUnlock(ResourceId resId, LockMode mode) const {
    switch (resId.getType()) {
        case RESOURCE_MUTEX:
            return false;

        case RESOURCE_GLOBAL:
        case RESOURCE_PBWM:
        case RESOURCE_RSTL:
        case RESOURCE_DATABASE:
        case RESOURCE_COLLECTION:
        case RESOURCE_METADATA:
            break;

        default:
            MONGO_UNREACHABLE;
    }

    switch (mode) {
        case MODE_X:
        case MODE_IX:
            return true;

        case MODE_IS:
        case MODE_S:
            return _sharedLocksShouldTwoPhaseLock;

        default:
            MONGO_UNREACHABLE;
    }
}

bool LockerImpl::isW() const {
    return getLockMode(resourceIdGlobal) == MODE_X;
}

bool LockerImpl::isR() const {
    return getLockMode(resourceIdGlobal) == MODE_S;
}

bool LockerImpl::isLocked() const {
    return getLockMode(resourceIdGlobal) != MODE_NONE;
}

bool LockerImpl::isWriteLocked() const {
    return isLockHeldForMode(resourceIdGlobal, MODE_IX);
}

bool LockerImpl::isReadLocked() const {
    return isLockHeldForMode(resourceIdGlobal, MODE_IS);
}

bool LockerImpl::isRSTLExclusive() const {
    return getLockMode(resourceIdReplicationStateTransitionLock) == MODE_X;
}

bool LockerImpl::isRSTLLocked() const {
    return getLockMode(resourceIdReplicationStateTransitionLock) != MODE_NONE;
}

void LockerImpl::dump() const {
    StringBuilder ss;
    ss << "Locker id " << _id << " status: ";

    _lock.lock();
    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        ss << it.key().toString() << " " << lockRequestStatusName(it->status) << " in "
           << modeName(it->mode) << "; ";
        it.next();
    }
    _lock.unlock();

    log() << ss.str();
}


//
// CondVarLockGrantNotification
//

CondVarLockGrantNotification::CondVarLockGrantNotification() {
    clear();
}

void CondVarLockGrantNotification::clear() {
    _result = LOCK_INVALID;
}

LockResult CondVarLockGrantNotification::wait(Milliseconds timeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _cond.wait_for(
               lock, timeout.toSystemDuration(), [this] { return _result != LOCK_INVALID; })
        ? _result
        : LOCK_TIMEOUT;
}

LockResult CondVarLockGrantNotification::wait(OperationContext* opCtx, Milliseconds timeout) {
    invariant(opCtx);
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (opCtx->waitForConditionOrInterruptFor(
            _cond, lock, timeout, [this] { return _result != LOCK_INVALID; })) {
        // Because waitForConditionOrInterruptFor evaluates the predicate before checking for
        // interrupt, it is possible that a killed operation can acquire a lock if the request is
        // granted quickly. For that reason, it is necessary to check if the operation has been
        // killed at least once before accepting the lock grant.
        opCtx->checkForInterrupt();
        return _result;
    }
    return LOCK_TIMEOUT;
}

void CondVarLockGrantNotification::notify(ResourceId resId, LockResult result) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    invariant(_result == LOCK_INVALID);
    _result = result;

    _cond.notify_all();
}

namespace {
TicketHolder* ticketHolders[LockModesCount] = {};
}  // namespace


//
// Locker
//

/* static */
void Locker::setGlobalThrottling(class TicketHolder* reading, class TicketHolder* writing) {
    ticketHolders[MODE_S] = reading;
    ticketHolders[MODE_IS] = reading;
    ticketHolders[MODE_IX] = writing;
}

LockerImpl::LockerImpl()
    : _id(idCounter.addAndFetch(1)), _wuowNestingLevel(0), _threadId(stdx::this_thread::get_id()) {}

stdx::thread::id LockerImpl::getThreadId() const {
    return _threadId;
}

void LockerImpl::updateThreadIdToCurrentThread() {
    _threadId = stdx::this_thread::get_id();
}

void LockerImpl::unsetThreadId() {
    _threadId = stdx::thread::id();  // Reset to represent a non-executing thread.
}

LockerImpl::~LockerImpl() {
    // Cannot delete the Locker while there are still outstanding requests, because the
    // LockManager may attempt to access deleted memory. Besides it is probably incorrect
    // to delete with unaccounted locks anyways.
    invariant(!inAWriteUnitOfWork());
    invariant(_numResourcesToUnlockAtEndUnitOfWork == 0);
    invariant(_requests.empty());
    invariant(_modeForTicket == MODE_NONE);

    // Reset the locking statistics so the object can be reused
    _stats.reset();
}

Locker::ClientState LockerImpl::getClientState() const {
    auto state = _clientState.load();
    if (state == kActiveReader && hasLockPending())
        state = kQueuedReader;
    if (state == kActiveWriter && hasLockPending())
        state = kQueuedWriter;

    return state;
}

void LockerImpl::lockGlobal(OperationContext* opCtx, LockMode mode) {
    LockResult result = _lockGlobalBegin(opCtx, mode, Date_t::max());
    if (result == LOCK_WAITING) {
        lockGlobalComplete(opCtx, Date_t::max());
    }
}

void LockerImpl::reacquireTicket(OperationContext* opCtx) {
    invariant(_modeForTicket != MODE_NONE);
    auto clientState = _clientState.load();
    const bool reader = isSharedLockMode(_modeForTicket);

    // Ensure that either we don't have a ticket, or the current ticket mode matches the lock mode.
    invariant(clientState == kInactive || (clientState == kActiveReader && reader) ||
              (clientState == kActiveWriter && !reader));

    // If we already have a ticket, there's nothing to do.
    if (clientState != kInactive)
        return;

    if (!_maxLockTimeout || _uninterruptibleLocksRequested) {
        invariant(_acquireTicket(opCtx, _modeForTicket, Date_t::max()));
    } else {
        uassert(ErrorCodes::LockTimeout,
                str::stream() << "Unable to acquire ticket with mode '" << _modeForTicket
                              << "' within a max lock request timeout of '" << *_maxLockTimeout
                              << "' milliseconds.",
                _acquireTicket(opCtx, _modeForTicket, Date_t::now() + *_maxLockTimeout));
    }
}

bool LockerImpl::_acquireTicket(OperationContext* opCtx, LockMode mode, Date_t deadline) {
    const bool reader = isSharedLockMode(mode);
    auto holder = shouldAcquireTicket() ? ticketHolders[mode] : nullptr;
    if (holder) {
        _clientState.store(reader ? kQueuedReader : kQueuedWriter);

        // If the ticket wait is interrupted, restore the state of the client.
        auto restoreStateOnErrorGuard = makeGuard([&] { _clientState.store(kInactive); });

        OperationContext* interruptible = _uninterruptibleLocksRequested ? nullptr : opCtx;
        if (deadline == Date_t::max()) {
            holder->waitForTicket(interruptible);
        } else if (!holder->waitForTicketUntil(interruptible, deadline)) {
            return false;
        }
        restoreStateOnErrorGuard.dismiss();
    }
    _clientState.store(reader ? kActiveReader : kActiveWriter);
    return true;
}

LockResult LockerImpl::_lockGlobalBegin(OperationContext* opCtx, LockMode mode, Date_t deadline) {
    dassert(isLocked() == (_modeForTicket != MODE_NONE));
    if (_modeForTicket == MODE_NONE) {
        if (_uninterruptibleLocksRequested) {
            // Ignore deadline and _maxLockTimeout.
            invariant(_acquireTicket(opCtx, mode, Date_t::max()));
        } else {
            auto beforeAcquire = Date_t::now();
            deadline = std::min(deadline,
                                _maxLockTimeout ? beforeAcquire + *_maxLockTimeout : Date_t::max());
            uassert(ErrorCodes::LockTimeout,
                    str::stream() << "Unable to acquire ticket with mode '" << _modeForTicket
                                  << "' within a max lock request timeout of '"
                                  << Date_t::now() - beforeAcquire << "' milliseconds.",
                    _acquireTicket(opCtx, mode, deadline));
        }
        _modeForTicket = mode;
    }

    LockMode actualLockMode = mode;
    if (opCtx) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        if (storageEngine && !storageEngine->supportsDBLocking()) {
            actualLockMode = isSharedLockMode(mode) ? MODE_S : MODE_X;
        }
    }

    const LockResult result = lockBegin(opCtx, resourceIdGlobal, actualLockMode);
    invariant(result == LOCK_OK || result == LOCK_WAITING);
    return result;
}

void LockerImpl::lockGlobalComplete(OperationContext* opCtx, Date_t deadline) {
    lockComplete(opCtx, resourceIdGlobal, getLockMode(resourceIdGlobal), deadline);
}

bool LockerImpl::unlockGlobal() {
    if (!unlock(resourceIdGlobal)) {
        return false;
    }

    invariant(!inAWriteUnitOfWork());

    LockRequestsMap::Iterator it = _requests.begin();
    while (!it.finished()) {
        // If we're here we should only have one reference to any lock. It is a programming
        // error for any lock used with multi-granularity locking to have more references than
        // the global lock, because every scope starts by calling lockGlobal.
        const auto resType = it.key().getType();
        if (resType == RESOURCE_GLOBAL || resType == RESOURCE_PBWM || resType == RESOURCE_RSTL ||
            resType == RESOURCE_MUTEX) {
            it.next();
        } else {
            invariant(_unlockImpl(&it));
        }
    }

    return true;
}

void LockerImpl::beginWriteUnitOfWork() {
    _wuowNestingLevel++;
}

void LockerImpl::endWriteUnitOfWork() {
    invariant(_wuowNestingLevel > 0);

    if (--_wuowNestingLevel > 0) {
        // Don't do anything unless leaving outermost WUOW.
        return;
    }

    LockRequestsMap::Iterator it = _requests.begin();
    while (_numResourcesToUnlockAtEndUnitOfWork > 0) {
        if (it->unlockPending) {
            invariant(!it.finished());
            _numResourcesToUnlockAtEndUnitOfWork--;
        }
        while (it->unlockPending > 0) {
            // If a lock is converted, unlock() may be called multiple times on a resource within
            // the same WriteUnitOfWork. All such unlock() requests must thus be fulfilled here.
            it->unlockPending--;
            unlock(it.key());
        }
        it.next();
    }
}

void LockerImpl::releaseWriteUnitOfWork(WUOWLockSnapshot* stateOut) {
    stateOut->wuowNestingLevel = _wuowNestingLevel;
    _wuowNestingLevel = 0;

    for (auto it = _requests.begin(); _numResourcesToUnlockAtEndUnitOfWork > 0; it.next()) {
        if (it->unlockPending) {
            while (it->unlockPending) {
                it->unlockPending--;
                stateOut->unlockPendingLocks.push_back({it.key(), it->mode});
            }
            _numResourcesToUnlockAtEndUnitOfWork--;
        }
    }
}

void LockerImpl::restoreWriteUnitOfWork(const WUOWLockSnapshot& stateToRestore) {
    invariant(_numResourcesToUnlockAtEndUnitOfWork == 0);
    invariant(!inAWriteUnitOfWork());

    for (auto& lock : stateToRestore.unlockPendingLocks) {
        auto it = _requests.begin();
        while (it && !(it.key() == lock.resourceId && it->mode == lock.mode)) {
            it.next();
        }
        invariant(!it.finished());
        if (!it->unlockPending) {
            _numResourcesToUnlockAtEndUnitOfWork++;
        }
        it->unlockPending++;
    }
    // Equivalent to call beginWriteUnitOfWork() multiple times.
    _wuowNestingLevel = stateToRestore.wuowNestingLevel;
}

bool LockerImpl::releaseWriteUnitOfWorkAndUnlock(LockSnapshot* stateOut) {
    // Only the global WUOW can be released, since we never need to release and restore
    // nested WUOW's. Thus we don't have to remember the nesting level.
    invariant(_wuowNestingLevel == 1);
    --_wuowNestingLevel;
    invariant(!isGlobalLockedRecursively());

    // All locks should be pending to unlock.
    invariant(_requests.size() == _numResourcesToUnlockAtEndUnitOfWork);
    for (auto it = _requests.begin(); it; it.next()) {
        // No converted lock so we don't need to unlock more than once.
        invariant(it->unlockPending == 1);
        it->unlockPending--;
    }
    _numResourcesToUnlockAtEndUnitOfWork = 0;

    return saveLockStateAndUnlock(stateOut);
}

void LockerImpl::restoreWriteUnitOfWorkAndLock(OperationContext* opCtx,
                                               const LockSnapshot& stateToRestore) {
    if (stateToRestore.globalMode != MODE_NONE) {
        restoreLockState(opCtx, stateToRestore);
    }

    invariant(_numResourcesToUnlockAtEndUnitOfWork == 0);
    for (auto it = _requests.begin(); it; it.next()) {
        invariant(_shouldDelayUnlock(it.key(), (it->mode)));
        invariant(it->unlockPending == 0);
        it->unlockPending++;
    }
    _numResourcesToUnlockAtEndUnitOfWork = static_cast<unsigned>(_requests.size());

    beginWriteUnitOfWork();
}

void LockerImpl::lock(OperationContext* opCtx, ResourceId resId, LockMode mode, Date_t deadline) {
    // `lockGlobal` must be called to lock `resourceIdGlobal`.
    invariant(resId != resourceIdGlobal);

    const LockResult result = lockBegin(opCtx, resId, mode);

    // Fast, uncontended path
    if (result == LOCK_OK)
        return;

    invariant(result == LOCK_WAITING);
    lockComplete(opCtx, resId, mode, deadline);
}

void LockerImpl::downgrade(ResourceId resId, LockMode newMode) {
    LockRequestsMap::Iterator it = _requests.find(resId);
    globalLockManager.downgrade(it.objAddr(), newMode);
}

bool LockerImpl::unlock(ResourceId resId) {
    LockRequestsMap::Iterator it = _requests.find(resId);

    // Don't attempt to unlock twice. This can happen when an interrupted global lock is destructed.
    if (it.finished())
        return false;

    if (inAWriteUnitOfWork() && _shouldDelayUnlock(it.key(), (it->mode))) {
        // Only delay unlocking if the lock is not acquired more than once. Otherwise, we can simply
        // call _unlockImpl to decrement recursiveCount instead of incrementing unlockPending. This
        // is safe because the lock is still being held in the strongest mode necessary.
        if (it->recursiveCount > 1) {
            // Invariant that the lock is still being held.
            invariant(!_unlockImpl(&it));
            return false;
        }
        if (!it->unlockPending) {
            _numResourcesToUnlockAtEndUnitOfWork++;
        }
        it->unlockPending++;
        // unlockPending will be incremented if a lock is converted or acquired in the same mode
        // recursively, and unlock() is called multiple times on one ResourceId.
        invariant(it->unlockPending <= it->recursiveCount);
        return false;
    }

    return _unlockImpl(&it);
}

bool LockerImpl::unlockRSTLforPrepare() {
    auto rstlRequest = _requests.find(resourceIdReplicationStateTransitionLock);

    // Don't attempt to unlock twice. This can happen when an interrupted global lock is destructed.
    if (!rstlRequest)
        return false;

    // If the RSTL is 'unlockPending' and we are fully unlocking it, then we do not want to
    // attempt to unlock the RSTL when the WUOW ends, since it will already be unlocked.
    if (rstlRequest->unlockPending) {
        rstlRequest->unlockPending = 0;
        _numResourcesToUnlockAtEndUnitOfWork--;
    }

    // Reset the recursiveCount to 1 so that we fully unlock the RSTL. Since it will be fully
    // unlocked, any future unlocks will be noops anyways.
    rstlRequest->recursiveCount = 1;

    return _unlockImpl(&rstlRequest);
}

LockMode LockerImpl::getLockMode(ResourceId resId) const {
    scoped_spinlock scopedLock(_lock);

    const LockRequestsMap::ConstIterator it = _requests.find(resId);
    if (!it)
        return MODE_NONE;

    return it->mode;
}

bool LockerImpl::isLockHeldForMode(ResourceId resId, LockMode mode) const {
    return isModeCovered(mode, getLockMode(resId));
}

bool LockerImpl::isDbLockedForMode(StringData dbName, LockMode mode) const {
    invariant(nsIsDbOnly(dbName));

    if (isW())
        return true;
    if (isR() && isSharedLockMode(mode))
        return true;

    const ResourceId resIdDb(RESOURCE_DATABASE, dbName);
    return isLockHeldForMode(resIdDb, mode);
}

bool LockerImpl::isCollectionLockedForMode(const NamespaceString& nss, LockMode mode) const {
    invariant(nss.coll().size());

    if (isW())
        return true;
    if (isR() && isSharedLockMode(mode))
        return true;

    const ResourceId resIdDb(RESOURCE_DATABASE, nss.db());

    LockMode dbMode = getLockMode(resIdDb);
    if (!shouldConflictWithSecondaryBatchApplication())
        return true;

    switch (dbMode) {
        case MODE_NONE:
            return false;
        case MODE_X:
            return true;
        case MODE_S:
            return isSharedLockMode(mode);
        case MODE_IX:
        case MODE_IS: {
            const ResourceId resIdColl(RESOURCE_COLLECTION, nss.ns());
            return isLockHeldForMode(resIdColl, mode);
        } break;
        case LockModesCount:
            break;
    }

    MONGO_UNREACHABLE;
    return false;
}

bool LockerImpl::wasGlobalLockTakenForWrite() const {
    return _globalLockMode & ((1 << MODE_IX) | (1 << MODE_X));
}

bool LockerImpl::wasGlobalLockTakenInModeConflictingWithWrites() const {
    return _wasGlobalLockTakenInModeConflictingWithWrites.load();
}

bool LockerImpl::wasGlobalLockTaken() const {
    return _globalLockMode;
}

void LockerImpl::setGlobalLockTakenInMode(LockMode mode) {
    _globalLockMode |= (1 << mode);

    if (mode == MODE_IX || mode == MODE_X || mode == MODE_S) {
        _wasGlobalLockTakenInModeConflictingWithWrites.store(true);
    }
}

ResourceId LockerImpl::getWaitingResource() const {
    scoped_spinlock scopedLock(_lock);

    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        if (it->status == LockRequest::STATUS_WAITING ||
            it->status == LockRequest::STATUS_CONVERTING) {
            return it.key();
        }

        it.next();
    }

    return ResourceId();
}

void LockerImpl::getLockerInfo(LockerInfo* lockerInfo,
                               const boost::optional<SingleThreadedLockStats> lockStatsBase) const {
    invariant(lockerInfo);

    // Zero-out the contents
    lockerInfo->locks.clear();
    lockerInfo->waitingResource = ResourceId();
    lockerInfo->stats.reset();

    _lock.lock();
    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        OneLock info;
        info.resourceId = it.key();
        info.mode = it->mode;

        lockerInfo->locks.push_back(info);
        it.next();
    }
    _lock.unlock();

    std::sort(lockerInfo->locks.begin(), lockerInfo->locks.end());

    lockerInfo->waitingResource = getWaitingResource();
    lockerInfo->stats.append(_stats);

    // lockStatsBase is a snapshot of lock stats taken when the sub-operation starts. Only
    // sub-operations have lockStatsBase.
    if (lockStatsBase)
        // Adjust the lock stats by subtracting the lockStatsBase. No mutex is needed because
        // lockStatsBase is immutable.
        lockerInfo->stats.subtract(*lockStatsBase);
}

boost::optional<Locker::LockerInfo> LockerImpl::getLockerInfo(
    const boost::optional<SingleThreadedLockStats> lockStatsBase) const {
    Locker::LockerInfo lockerInfo;
    getLockerInfo(&lockerInfo, lockStatsBase);
    return std::move(lockerInfo);
}

bool LockerImpl::saveLockStateAndUnlock(Locker::LockSnapshot* stateOut) {
    // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());

    // Clear out whatever is in stateOut.
    stateOut->locks.clear();
    stateOut->globalMode = MODE_NONE;

    // First, we look at the global lock.  There is special handling for this so we store it
    // separately from the more pedestrian locks.
    LockRequestsMap::Iterator globalRequest = _requests.find(resourceIdGlobal);
    if (!globalRequest) {
        // If there's no global lock there isn't really anything to do. Check that.
        for (auto it = _requests.begin(); !it.finished(); it.next()) {
            invariant(it.key().getType() == RESOURCE_MUTEX);
        }
        return false;
    }

    // If the global lock or RSTL has been acquired more than once, we're probably somewhere in a
    // DBDirectClient call.  It's not safe to release and reacquire locks -- the context using
    // the DBDirectClient is probably not prepared for lock release.
    LockRequestsMap::Iterator rstlRequest =
        _requests.find(resourceIdReplicationStateTransitionLock);
    if (globalRequest->recursiveCount > 1 || (rstlRequest && rstlRequest->recursiveCount > 1)) {
        return false;
    }

    // The global lock must have been acquired just once
    stateOut->globalMode = globalRequest->mode;
    invariant(unlock(resourceIdGlobal));

    // Next, the non-global locks.
    for (LockRequestsMap::Iterator it = _requests.begin(); !it.finished(); it.next()) {
        const ResourceId resId = it.key();
        const ResourceType resType = resId.getType();
        if (resType == RESOURCE_MUTEX)
            continue;

        // We should never have to save and restore metadata locks.
        invariant(RESOURCE_DATABASE == resType || RESOURCE_COLLECTION == resType ||
                  (RESOURCE_GLOBAL == resType && isSharedLockMode(it->mode)) ||
                  (RESOURCE_PBWM == resType && isSharedLockMode(it->mode)) ||
                  (RESOURCE_RSTL == resType && it->mode == MODE_IX));

        // And, stuff the info into the out parameter.
        OneLock info;
        info.resourceId = resId;
        info.mode = it->mode;

        stateOut->locks.push_back(info);

        invariant(unlock(resId));
    }
    invariant(!isLocked());

    // Sort locks by ResourceId. They'll later be acquired in this canonical locking order.
    std::sort(stateOut->locks.begin(), stateOut->locks.end());

    return true;
}

void LockerImpl::restoreLockState(OperationContext* opCtx, const Locker::LockSnapshot& state) {
    // We shouldn't be restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());
    invariant(_modeForTicket == MODE_NONE);
    invariant(_clientState.load() == kInactive);

    if (opCtx) {
        getFlowControlTicket(opCtx, state.globalMode);
    }

    std::vector<OneLock>::const_iterator it = state.locks.begin();
    // If we locked the PBWM, it must be locked before the resourceIdGlobal and
    // resourceIdReplicationStateTransitionLock resources.
    if (it != state.locks.end() && it->resourceId == resourceIdParallelBatchWriterMode) {
        lock(opCtx, it->resourceId, it->mode);
        it++;
    }

    // If we locked the RSTL, it must be locked before the resourceIdGlobal resource.
    if (it != state.locks.end() && it->resourceId == resourceIdReplicationStateTransitionLock) {
        lock(opCtx, it->resourceId, it->mode);
        it++;
    }

    lockGlobal(opCtx, state.globalMode);
    for (; it != state.locks.end(); it++) {
        lock(opCtx, it->resourceId, it->mode);
    }
    invariant(_modeForTicket != MODE_NONE);
}

LockResult LockerImpl::lockBegin(OperationContext* opCtx, ResourceId resId, LockMode mode) {
    dassert(!getWaitingResource().isValid());

    LockRequest* request;
    bool isNew = true;

    LockRequestsMap::Iterator it = _requests.find(resId);
    if (!it) {
        scoped_spinlock scopedLock(_lock);
        LockRequestsMap::Iterator itNew = _requests.insert(resId);
        itNew->initNew(this, &_notify);

        request = itNew.objAddr();
    } else {
        request = it.objAddr();
        isNew = false;
    }

    // If unlockPending is nonzero, that means a LockRequest already exists for this resource but
    // is planned to be released at the end of this WUOW due to two-phase locking. Rather than
    // unlocking the existing request, we can reuse it if the existing mode matches the new mode.
    if (request->unlockPending && isModeCovered(mode, request->mode)) {
        request->unlockPending--;
        if (!request->unlockPending) {
            _numResourcesToUnlockAtEndUnitOfWork--;
        }
        return LOCK_OK;
    }

    // Making this call here will record lock re-acquisitions and conversions as well.
    globalStats.recordAcquisition(_id, resId, mode);
    _stats.recordAcquisition(resId, mode);

    // Give priority to the full modes for Global, PBWM, and RSTL resources so we don't stall global
    // operations such as shutdown or stepdown.
    const ResourceType resType = resId.getType();
    if (resType == RESOURCE_GLOBAL || resType == RESOURCE_PBWM || resType == RESOURCE_RSTL) {
        if (mode == MODE_S || mode == MODE_X) {
            request->enqueueAtFront = true;
            request->compatibleFirst = true;
        }
    } else if (resType != RESOURCE_MUTEX) {
        // This is all sanity checks that the global locks are always be acquired
        // before any other lock has been acquired and they must be in sync with the nesting.
        DEV {
            const LockRequestsMap::Iterator itGlobal = _requests.find(resourceIdGlobal);
            invariant(itGlobal->recursiveCount > 0);
            invariant(itGlobal->mode != MODE_NONE);
        };
    }

    // The notification object must be cleared before we invoke the lock manager, because
    // otherwise we might reset state if the lock becomes granted very fast.
    _notify.clear();

    LockResult result = isNew ? globalLockManager.lock(resId, request, mode)
                              : globalLockManager.convert(resId, request, mode);

    if (result == LOCK_WAITING) {
        globalStats.recordWait(_id, resId, mode);
        _stats.recordWait(resId, mode);
    } else if (result == LOCK_OK && opCtx && _uninterruptibleLocksRequested == 0) {
        // Lock acquisitions are not allowed to succeed when opCtx is marked as interrupted, unless
        // the caller requested an uninterruptible lock.
        auto interruptStatus = opCtx->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            auto unlockIt = _requests.find(resId);
            invariant(unlockIt);
            _unlockImpl(&unlockIt);
            uassertStatusOK(interruptStatus);
        }
    }

    return result;
}

void LockerImpl::lockComplete(OperationContext* opCtx,
                              ResourceId resId,
                              LockMode mode,
                              Date_t deadline) {

    // Clean up the state on any failed lock attempts.
    auto unlockOnErrorGuard = makeGuard([&] {
        LockRequestsMap::Iterator it = _requests.find(resId);
        invariant(it);
        _unlockImpl(&it);
    });

    // This failpoint is used to time out non-intent locks if they cannot be granted immediately.
    // Testing-only.
    if (!_uninterruptibleLocksRequested && MONGO_FAIL_POINT(failNonIntentLocksIfWaitNeeded)) {
        uassert(ErrorCodes::LockTimeout,
                str::stream() << "Cannot immediately acquire lock '" << resId.toString()
                              << "'. Timing out due to failpoint.",
                (mode == MODE_IS || mode == MODE_IX));
    }

    LockResult result;
    Milliseconds timeout;
    if (deadline == Date_t::max()) {
        timeout = Milliseconds::max();
    } else if (deadline <= Date_t()) {
        timeout = Milliseconds(0);
    } else {
        timeout = deadline - Date_t::now();
    }
    timeout = std::min(timeout, _maxLockTimeout ? *_maxLockTimeout : Milliseconds::max());
    if (_uninterruptibleLocksRequested) {
        timeout = Milliseconds::max();
    }

    // Don't go sleeping without bound in order to be able to report long waits.
    Milliseconds waitTime = std::min(timeout, MaxWaitTime);
    const uint64_t startOfTotalWaitTime = curTimeMicros64();
    uint64_t startOfCurrentWaitTime = startOfTotalWaitTime;

    while (true) {
        // It is OK if this call wakes up spuriously, because we re-evaluate the remaining
        // wait time anyways.
        // If we have an operation context, we want to use its interruptible wait so that
        // pending lock acquisitions can be cancelled, so long as no callers have requested an
        // uninterruptible lock.
        if (opCtx && _uninterruptibleLocksRequested == 0) {
            result = _notify.wait(opCtx, waitTime);
        } else {
            result = _notify.wait(waitTime);
        }

        // Account for the time spent waiting on the notification object
        const uint64_t curTimeMicros = curTimeMicros64();
        const uint64_t elapsedTimeMicros = curTimeMicros - startOfCurrentWaitTime;
        startOfCurrentWaitTime = curTimeMicros;

        globalStats.recordWaitTime(_id, resId, mode, elapsedTimeMicros);
        _stats.recordWaitTime(resId, mode, elapsedTimeMicros);

        if (result == LOCK_OK)
            break;

        // If infinite timeout was requested, just keep waiting
        if (timeout == Milliseconds::max()) {
            continue;
        }

        const auto totalBlockTime = duration_cast<Milliseconds>(
            Microseconds(int64_t(curTimeMicros - startOfTotalWaitTime)));
        waitTime = (totalBlockTime < timeout) ? std::min(timeout - totalBlockTime, MaxWaitTime)
                                              : Milliseconds(0);

        uassert(ErrorCodes::LockTimeout,
                str::stream() << "Unable to acquire lock '" << resId.toString() << "' within "
                              << timeout << "' milliseconds.",
                waitTime > Milliseconds(0));
    }

    invariant(result == LOCK_OK);
    unlockOnErrorGuard.dismiss();
}

void LockerImpl::getFlowControlTicket(OperationContext* opCtx, LockMode lockMode) {
    auto ticketholder = FlowControlTicketholder::get(opCtx);
    if (ticketholder && lockMode == LockMode::MODE_IX && _clientState.load() == kInactive &&
        opCtx->shouldParticipateInFlowControl() && !_uninterruptibleLocksRequested) {
        // FlowControl only acts when a MODE_IX global lock is being taken. The clientState is only
        // being modified here to change serverStatus' `globalLock.currentQueue` metrics. This
        // method must not exit with a side-effect on the clientState. That value is also used for
        // tracking whether other resources need to be released.
        _clientState.store(kQueuedWriter);
        auto restoreState = makeGuard([&] { _clientState.store(kInactive); });
        ticketholder->getTicket(opCtx, &_flowControlStats);
    }
}

LockResult LockerImpl::lockRSTLBegin(OperationContext* opCtx, LockMode mode) {
    invariant(mode == MODE_IX || mode == MODE_X);
    return lockBegin(opCtx, resourceIdReplicationStateTransitionLock, mode);
}

void LockerImpl::lockRSTLComplete(OperationContext* opCtx, LockMode mode, Date_t deadline) {
    lockComplete(opCtx, resourceIdReplicationStateTransitionLock, mode, deadline);
}

void LockerImpl::releaseTicket() {
    invariant(_modeForTicket != MODE_NONE);
    _releaseTicket();
}

void LockerImpl::_releaseTicket() {
    auto holder = shouldAcquireTicket() ? ticketHolders[_modeForTicket] : nullptr;
    if (holder) {
        holder->release();
    }
    _clientState.store(kInactive);
}

bool LockerImpl::_unlockImpl(LockRequestsMap::Iterator* it) {
    if (globalLockManager.unlock(it->objAddr())) {
        if (it->key() == resourceIdGlobal) {
            invariant(_modeForTicket != MODE_NONE);

            // We may have already released our ticket through a call to releaseTicket().
            if (_clientState.load() != kInactive) {
                _releaseTicket();
            }

            _modeForTicket = MODE_NONE;
        }

        scoped_spinlock scopedLock(_lock);
        it->remove();

        return true;
    }

    return false;
}

bool LockerImpl::isGlobalLockedRecursively() {
    auto globalLockRequest = _requests.find(resourceIdGlobal);
    return !globalLockRequest.finished() && globalLockRequest->recursiveCount > 1;
}

//
// Auto classes
//

namespace {
/**
 *  Periodically purges unused lock buckets. The first time the lock is used again after
 *  cleanup it needs to be allocated, and similarly, every first use by a client for an intent
 *  mode may need to create a partitioned lock head. Cleanup is done roughly once a minute.
 */
class UnusedLockCleaner : PeriodicTask {
public:
    std::string taskName() const {
        return "UnusedLockCleaner";
    }

    void taskDoWork() {
        LOG(2) << "cleaning up unused lock buckets of the global lock manager";
        getGlobalLockManager()->cleanupUnusedLocks();
    }
} unusedLockCleaner;
}  // namespace


//
// Standalone functions
//

LockManager* getGlobalLockManager() {
    return &globalLockManager;
}

void reportGlobalLockingStats(SingleThreadedLockStats* outStats) {
    globalStats.report(outStats);
}

void resetGlobalLockStats() {
    globalStats.reset();
}

// Hardcoded resource IDs.
const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, StringData("local"));
const ResourceId resourceIdOplog = ResourceId(RESOURCE_COLLECTION, StringData("local.oplog.rs"));
const ResourceId resourceIdAdminDB = ResourceId(RESOURCE_DATABASE, StringData("admin"));
const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, 1ULL);
const ResourceId resourceIdParallelBatchWriterMode = ResourceId(RESOURCE_PBWM, 1ULL);
const ResourceId resourceIdReplicationStateTransitionLock = ResourceId(RESOURCE_RSTL, 1ULL);

}  // namespace mongo
