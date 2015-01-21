/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/lock_state.h"

#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/namespace_string.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

    /**
     * Partitioned global lock statistics, so we don't hit the same bucket.
     */
    class PartitionedInstanceWideLockStats {
        MONGO_DISALLOW_COPYING(PartitionedInstanceWideLockStats);
    public:

        PartitionedInstanceWideLockStats() { }

        void recordAcquisition(LockerId id, ResourceId resId, LockMode mode) {
            LockStats& stats = _get(id);
            stats.recordAcquisition(resId, mode);
        }

        void recordWait(LockerId id, ResourceId resId, LockMode mode) {
            LockStats& stats = _get(id);
            stats.recordWait(resId, mode);
        }

        void recordWaitTime(LockerId id, ResourceId resId, LockMode mode, uint64_t waitMicros) {
            LockStats& stats = _get(id);
            stats.recordWaitTime(resId, mode, waitMicros);
        }

        void recordDeadlock(ResourceId resId, LockMode mode) {
            LockStats& stats = _get(resId);
            stats.recordDeadlock(resId, mode);
        }

        void report(LockStats* outStats) const {
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
        // separate page/cache line in order to avoid false sharing. The 4096-byte alignment is
        // in an effort to play nicely with NUMA.
        struct MONGO_COMPILER_ALIGN_TYPE(4096) AlignedLockStats {
            LockStats stats;
        };

        enum { NumPartitions = 8 };


        LockStats& _get(LockerId id) {
            return _partitions[id % NumPartitions].stats;
        }


        AlignedLockStats _partitions[NumPartitions];
    };


    /**
     * Used to sort locks by granularity when snapshotting lock state. We must report and reacquire
     * locks in the same granularity in which they are acquired (i.e. global, flush, database,
     * collection, etc).
     */
    struct SortByGranularity {
        inline bool operator()(const Locker::OneLock& lhs, const Locker::OneLock& rhs) const {
            return lhs.resourceId.getType() < rhs.resourceId.getType();
        }
    };


    // Global lock manager instance.
    LockManager globalLockManager;

    // Global lock. Every server operation, which uses the Locker must acquire this lock at least
    // once. See comments in the header file (begin/endTransaction) for more information.
    const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, 1ULL);

    // Flush lock. This is only used for the MMAP V1 storage engine and synchronizes journal writes
    // to the shared view and remaps. See the comments in the header for information on how MMAP V1
    // concurrency control works.
    const ResourceId resourceIdMMAPV1Flush = ResourceId(RESOURCE_MMAPV1_FLUSH, 2ULL);

    // How often (in millis) to check for deadlock if a lock has not been granted for some time
    const unsigned DeadlockTimeoutMs = 500;

    // Dispenses unique LockerId identifiers
    AtomicUInt64 idCounter(0);

    // Partitioned global lock statistics, so we don't hit the same bucket
    PartitionedInstanceWideLockStats globalStats;


    /**
     * Whether the particular lock's release should be held until the end of the operation. We
     * delay release of exclusive locks (locks that are for write operations) in order to ensure
     * that the data they protect is committed successfully.
     */
    bool shouldDelayUnlock(ResourceId resId, LockMode mode) {
        // Global and flush lock are not used to protect transactional resources and as such, they
        // need to be acquired and released when requested.
        if (resId == resourceIdGlobal) {
            return false;
        }

        if (resId == resourceIdMMAPV1Flush) {
            return false;
        }

        switch (mode) {
        case MODE_X:
        case MODE_IX:
            return true;

        case MODE_IS:
        case MODE_S:
            return false;

        default:
            invariant(false);
        }
    }

} // namespace


    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isW() const {
        return getLockMode(resourceIdGlobal) == MODE_X;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isR() const {
        return getLockMode(resourceIdGlobal) == MODE_S;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isLocked() const {
        return getLockMode(resourceIdGlobal) != MODE_NONE;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isWriteLocked() const {
        return isLockHeldForMode(resourceIdGlobal, MODE_IX);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isReadLocked() const {
        return isLockHeldForMode(resourceIdGlobal, MODE_IS);
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::assertEmpty() const {
        invariant(!inAWriteUnitOfWork());
        invariant(_resourcesToUnlockAtEndOfUnitOfWork.empty());
        invariant(_requests.empty());
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::dump() const {
        StringBuilder ss;
        ss << "lock status: ";

        //  isLocked() must be called without holding _lock
        if (!isLocked()) {
            ss << "unlocked";
        }
        else {
            // SERVER-14978: Dump lock stats information
        }

        ss << " requests:";

        _lock.lock();
        LockRequestsMap::ConstIterator it = _requests.begin();
        while (!it.finished()) {
            ss << " " << it.key().toString();
            it.next();
        }
        _lock.unlock();

        log() << ss.str() << std::endl;
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

    LockResult CondVarLockGrantNotification::wait(unsigned timeoutMs) {
        boost::unique_lock<boost::mutex> lock(_mutex);
        while (_result == LOCK_INVALID) {
            if (!_cond.timed_wait(lock, Milliseconds(timeoutMs))) {
                // Timeout
                return LOCK_TIMEOUT;
            }
        }

        return _result;
    }

    void CondVarLockGrantNotification::notify(ResourceId resId, LockResult result) {
        boost::unique_lock<boost::mutex> lock(_mutex);
        invariant(_result == LOCK_INVALID);
        _result = result;

        _cond.notify_all();
    }


    //
    // Locker
    //

    template<bool IsForMMAPV1>
    LockerImpl<IsForMMAPV1>::LockerImpl()
        : _id(idCounter.addAndFetch(1)),
          _requestStartTime(0),
          _wuowNestingLevel(0),
          _batchWriter(false),
          _lockPendingParallelWriter(false) {

    }

    template<bool IsForMMAPV1>
    LockerImpl<IsForMMAPV1>::~LockerImpl() {
        // Cannot delete the Locker while there are still outstanding requests, because the
        // LockManager may attempt to access deleted memory. Besides it is probably incorrect
        // to delete with unaccounted locks anyways.
        assertEmpty();
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lockGlobal(LockMode mode, unsigned timeoutMs) {
        LockResult result = lockGlobalBegin(mode);
        if (result == LOCK_WAITING) {
            result = lockGlobalComplete(timeoutMs);
        }

        if (result == LOCK_OK) {
            lockMMAPV1Flush();
        }

        return result;
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lockGlobalBegin(LockMode mode) {
        const LockResult result = lockBegin(resourceIdGlobal, mode);
        if (result == LOCK_OK) return LOCK_OK;

        // Currently, deadlock detection does not happen inline with lock acquisition so the only
        // unsuccessful result that the lock manager would return is LOCK_WAITING.
        invariant(result == LOCK_WAITING);

        return result;
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lockGlobalComplete(unsigned timeoutMs) {
        return lockComplete(resourceIdGlobal, getLockMode(resourceIdGlobal), timeoutMs, false);
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::lockMMAPV1Flush() {
        if (!IsForMMAPV1) return;

        // The flush lock always has a reference count of 1, because it is dropped at the end of
        // each write unit of work in order to allow the flush thread to run. See the comments in
        // the header for information on how the MMAP V1 journaling system works.
        LockRequest* globalLockRequest = _requests.find(resourceIdGlobal).objAddr();
        if (globalLockRequest->recursiveCount == 1) {
            invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
        }

        dassert(getLockMode(resourceIdMMAPV1Flush) == _getModeForMMAPV1FlushLock());
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::downgradeGlobalXtoSForMMAPV1() {
        invariant(!inAWriteUnitOfWork());

        LockRequest* globalLockRequest = _requests.find(resourceIdGlobal).objAddr();
        invariant(globalLockRequest->mode == MODE_X);
        invariant(globalLockRequest->recursiveCount == 1);
        globalLockManager.downgrade(globalLockRequest, MODE_S);

        if (IsForMMAPV1) {
            invariant(unlock(resourceIdMMAPV1Flush));
        }
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::unlockAll() {
        if (!unlock(resourceIdGlobal)) {
            return false;
        }

        LockRequestsMap::Iterator it = _requests.begin();
        while (!it.finished()) {
            // If we're here we should only have one reference to any lock. It is a programming
            // error for any lock to have more references than the global lock, because every
            // scope starts by calling lockGlobal.
            invariant(_unlockImpl(it));
        }

        return true;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::beginWriteUnitOfWork() {
        // Sanity check that write transactions under MMAP V1 have acquired the flush lock, so we
        // don't allow partial changes to be written.
        dassert(!IsForMMAPV1 || isLockHeldForMode(resourceIdMMAPV1Flush, MODE_IX));

        _wuowNestingLevel++;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::endWriteUnitOfWork() {
        invariant(_wuowNestingLevel > 0);

        if (--_wuowNestingLevel > 0) {
            // Don't do anything unless leaving outermost WUOW.
            return;
        }

        while (!_resourcesToUnlockAtEndOfUnitOfWork.empty()) {
            unlock(_resourcesToUnlockAtEndOfUnitOfWork.front());
            _resourcesToUnlockAtEndOfUnitOfWork.pop();
        }

        // For MMAP V1, we need to yield the flush lock so that the flush thread can run
        if (IsForMMAPV1) {
            invariant(unlock(resourceIdMMAPV1Flush));

            while (true) {
                LockResult result =
                    lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock(), UINT_MAX, true);

                if (result == LOCK_OK) break;

                invariant(result == LOCK_DEADLOCK);
            }
        }
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lock(ResourceId resId,
                                             LockMode mode,
                                             unsigned timeoutMs,
                                             bool checkDeadlock) {

        const LockResult result = lockBegin(resId, mode);

        // Fast, uncontended path
        if (result == LOCK_OK) return LOCK_OK;

        // Currently, deadlock detection does not happen inline with lock acquisition so the only
        // unsuccessful result that the lock manager would return is LOCK_WAITING.
        invariant(result == LOCK_WAITING);

        return lockComplete(resId, mode, timeoutMs, checkDeadlock);
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::downgrade(ResourceId resId, LockMode newMode) {
        LockRequestsMap::Iterator it = _requests.find(resId);
        globalLockManager.downgrade(it.objAddr(), newMode);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::unlock(ResourceId resId) {
        LockRequestsMap::Iterator it = _requests.find(resId);
        return _unlockImpl(it);
    }

    template<bool IsForMMAPV1>
    LockMode LockerImpl<IsForMMAPV1>::getLockMode(ResourceId resId) const {
        scoped_spinlock scopedLock(_lock);

        const LockRequestsMap::ConstIterator it = _requests.find(resId);
        if (!it) return MODE_NONE;

        return it->mode;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isLockHeldForMode(ResourceId resId, LockMode mode) const {
        return isModeCovered(mode, getLockMode(resId));
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isDbLockedForMode(const StringData& dbName,
                                                    LockMode mode) const {
        invariant(nsIsDbOnly(dbName));

        if (isW()) return true;
        if (isR() && isSharedLockMode(mode)) return true;

        const ResourceId resIdDb(RESOURCE_DATABASE, dbName);
        return isLockHeldForMode(resIdDb, mode);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isCollectionLockedForMode(const StringData& ns,
                                                            LockMode mode) const {
        invariant(nsIsFull(ns));

        if (isW()) return true;
        if (isR() && isSharedLockMode(mode)) return true;

        const NamespaceString nss(ns);
        const ResourceId resIdDb(RESOURCE_DATABASE, nss.db());

        LockMode dbMode = getLockMode(resIdDb);

        switch (dbMode) {
        case MODE_NONE: return false;
        case MODE_X: return true;
        case MODE_S: return isSharedLockMode(mode);
        case MODE_IX:
        case MODE_IS:
            {
                const ResourceId resIdColl(RESOURCE_COLLECTION, ns);
                return isLockHeldForMode(resIdColl, mode);
            }
            break;
        case LockModesCount:
            break;
        }

        invariant(false);
        return false;
    }

    template<bool IsForMMAPV1>
    ResourceId LockerImpl<IsForMMAPV1>::getWaitingResource() const {
        scoped_spinlock scopedLock(_lock);

        LockRequestsMap::ConstIterator it = _requests.begin();
        while (!it.finished()) {
            if (it->status != LockRequest::STATUS_GRANTED) {
                return it.key();
            }

            it.next();
        }

        return ResourceId();
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::getLockerInfo(LockerInfo* lockerInfo) const {
        invariant(lockerInfo);

        // Zero-out the contents
        lockerInfo->locks.clear();
        lockerInfo->waitingResource = ResourceId();
        lockerInfo->stats.reset();

        if (!isLocked()) return;

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

        std::sort(lockerInfo->locks.begin(), lockerInfo->locks.end(), SortByGranularity());

        lockerInfo->waitingResource = getWaitingResource();
        lockerInfo->stats.append(_stats);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::saveLockStateAndUnlock(Locker::LockSnapshot* stateOut) {
        // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
        invariant(!inAWriteUnitOfWork());

        // Clear out whatever is in stateOut.
        stateOut->locks.clear();
        stateOut->globalMode = MODE_NONE;

        // First, we look at the global lock.  There is special handling for this (as the flush
        // lock goes along with it) so we store it separately from the more pedestrian locks.
        LockRequestsMap::Iterator globalRequest = _requests.find(resourceIdGlobal);
        if (!globalRequest) {
            // If there's no global lock there isn't really anything to do.
            invariant(_requests.empty());
            return false;
        }

        // If the global lock has been acquired more than once, we're probably somewhere in a
        // DBDirectClient call.  It's not safe to release and reacquire locks -- the context using
        // the DBDirectClient is probably not prepared for lock release.
        if (globalRequest->recursiveCount > 1) {
            return false;
        }

        // The global lock must have been acquired just once
        stateOut->globalMode = globalRequest->mode;
        invariant(unlock(resourceIdGlobal));

        // Next, the non-global locks.
        for (LockRequestsMap::Iterator it = _requests.begin(); !it.finished(); it.next()) {
            const ResourceId resId = it.key();

            // We should never have to save and restore metadata locks.
            invariant((IsForMMAPV1 && (resourceIdMMAPV1Flush == resId)) ||
                      RESOURCE_DATABASE == resId.getType() ||
                      RESOURCE_COLLECTION == resId.getType());

            // And, stuff the info into the out parameter.
            OneLock info;
            info.resourceId = resId;
            info.mode = it->mode;

            stateOut->locks.push_back(info);

            invariant(unlock(resId));
        }

        // Sort locks from coarsest to finest.  They'll later be acquired in this order.
        std::sort(stateOut->locks.begin(), stateOut->locks.end(), SortByGranularity());

        return true;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::restoreLockState(const Locker::LockSnapshot& state) {
        // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
        invariant(!inAWriteUnitOfWork());

        invariant(LOCK_OK == lockGlobal(state.globalMode));

        std::vector<OneLock>::const_iterator it = state.locks.begin();
        for (; it != state.locks.end(); it++) {
            // This is a sanity check that lockGlobal restored the MMAP V1 flush lock in the
            // expected mode.
            if (IsForMMAPV1 && (it->resourceId == resourceIdMMAPV1Flush)) {
                invariant(it->mode == _getModeForMMAPV1FlushLock());
            }
            else {
                invariant(LOCK_OK == lock(it->resourceId, it->mode));
            }
        }
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lockBegin(ResourceId resId, LockMode mode) {
        invariant(!getWaitingResource().isValid());

        LockRequest* request;
        bool isNew = true;

        LockRequestsMap::Iterator it = _requests.find(resId);
        if (!it) {
            scoped_spinlock scopedLock(_lock);
            LockRequestsMap::Iterator itNew = _requests.insert(resId);
            itNew->initNew(this, &_notify);

            request = itNew.objAddr();
        }
        else {
            request = it.objAddr();
            isNew = false;
        }

        // Making this call here will record lock re-acquisitions and conversions as well.
        globalStats.recordAcquisition(_id, resId, mode);
        _stats.recordAcquisition(resId, mode);

        // Give priority to the full modes for global and flush lock so we don't stall global
        // operations such as shutdown or flush.
        if (resId == resourceIdGlobal || (IsForMMAPV1 && resId == resourceIdMMAPV1Flush)) {
            if (mode == MODE_S || mode == MODE_X) {
                request->enqueueAtFront = true;
                request->compatibleFirst = true;
            }
        }
        else {
            // This is all sanity checks that the global and flush locks are always be acquired
            // before any other lock has been acquired and they must be in sync with the nesting.
            DEV {
                const LockRequestsMap::Iterator itGlobal = _requests.find(resourceIdGlobal);
                invariant(itGlobal->recursiveCount > 0);
                invariant(itGlobal->mode != MODE_NONE);

                // Check the MMAP V1 flush lock is held in the appropriate mode
                invariant(!IsForMMAPV1 || isLockHeldForMode(resourceIdMMAPV1Flush,
                                                            _getModeForMMAPV1FlushLock()));
            };
        }

        // The notification object must be cleared before we invoke the lock manager, because
        // otherwise we might reset state if the lock becomes granted very fast.
        _notify.clear();

        LockResult result = isNew ? globalLockManager.lock(resId, request, mode) :
                                    globalLockManager.convert(resId, request, mode);

        if (result == LOCK_WAITING) {
            // Start counting the wait time so that lockComplete can update that metric
            _requestStartTime = curTimeMicros64();
            globalStats.recordWait(_id, resId, mode);
            _stats.recordWait(resId, mode);
        }

        return result;
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lockComplete(ResourceId resId,
                                                     LockMode mode,
                                                     unsigned timeoutMs,
                                                     bool checkDeadlock) {

        // Under MMAP V1 engine a deadlock can occur if a thread goes to sleep waiting on
        // DB lock, while holding the flush lock, so it has to be released. This is only
        // correct to do if not in a write unit of work.
        const bool yieldFlushLock = IsForMMAPV1 &&
                                    !inAWriteUnitOfWork() &&
                                    resId != resourceIdGlobal &&
                                    resId != resourceIdMMAPV1Flush;
        if (yieldFlushLock) {
            invariant(unlock(resourceIdMMAPV1Flush));
        }

        LockResult result;

        // Don't go sleeping without bound in order to be able to report long waits or wake up for
        // deadlock detection.
        unsigned waitTimeMs = std::min(timeoutMs, DeadlockTimeoutMs);
        while (true) {
            // It is OK if this call wakes up spuriously, because we re-evaluate the remaining
            // wait time anyways.
            result = _notify.wait(waitTimeMs);

            // Account for the time spent waiting on the notification object
            const uint64_t elapsedTimeMicros = curTimeMicros64() - _requestStartTime;
            globalStats.recordWaitTime(_id, resId, mode, elapsedTimeMicros);
            _stats.recordWaitTime(resId, mode, elapsedTimeMicros);

            if (result == LOCK_OK) break;

            if (checkDeadlock) {
                DeadlockDetector wfg(globalLockManager, this);
                if (wfg.check().hasCycle()) {
                    warning() << "Deadlock found: " << wfg.toString();

                    globalStats.recordDeadlock(resId, mode);
                    _stats.recordDeadlock(resId, mode);

                    result = LOCK_DEADLOCK;
                    break;
                }
            }

            // If infinite timeout was requested, just keep waiting
            if (timeoutMs == UINT_MAX) {
                continue;
            }

            const unsigned elapsedTimeMs = elapsedTimeMicros / 1000;
            waitTimeMs = (elapsedTimeMs < timeoutMs) ?
                std::min(timeoutMs - elapsedTimeMs, DeadlockTimeoutMs) : 0;

            if (waitTimeMs == 0) {
                break;
            }
        }

        // Cleanup the state, since this is an unused lock now
        if (result != LOCK_OK) {
            LockRequestsMap::Iterator it = _requests.find(resId);
            if (globalLockManager.unlock(it.objAddr())) {
                scoped_spinlock scopedLock(_lock);
                it.remove();
            }
        }

        if (yieldFlushLock) {
            // We cannot obey the timeout here, because it is not correct to return from the lock
            // request with the flush lock released.
            invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
        }

        return result;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::_unlockImpl(LockRequestsMap::Iterator& it) {
        if (inAWriteUnitOfWork() && shouldDelayUnlock(it.key(), it->mode)) {
            _resourcesToUnlockAtEndOfUnitOfWork.push(it.key());
            return false;
        }

        if (globalLockManager.unlock(it.objAddr())) {
            scoped_spinlock scopedLock(_lock);
            it.remove();

            return true;
        }

        return false;
    }

    template<bool IsForMMAPV1>
    LockMode LockerImpl<IsForMMAPV1>::_getModeForMMAPV1FlushLock() const {
        invariant(IsForMMAPV1);

        LockMode mode = getLockMode(resourceIdGlobal);
        switch (mode) {
        case MODE_X:
        case MODE_IX:
            return MODE_IX;
        case MODE_S:
        case MODE_IS:
            return MODE_IS;
        default:
            invariant(false);
            return MODE_NONE;
        }
    }


    //
    // Auto classes
    //

    AutoYieldFlushLockForMMAPV1Commit::AutoYieldFlushLockForMMAPV1Commit(Locker* locker)
        : _locker(static_cast<MMAPV1LockerImpl*>(locker)) {

        // Explicit yielding of the flush lock should happen only at global synchronization points
        // such as database drop. There should not be any active writes at these points.
        invariant(!_locker->inAWriteUnitOfWork());

        if (isMMAPV1()) {
            invariant(_locker->unlock(resourceIdMMAPV1Flush));
        }
    }

    AutoYieldFlushLockForMMAPV1Commit::~AutoYieldFlushLockForMMAPV1Commit() {
        if (isMMAPV1()) {
            invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush,
                                               _locker->_getModeForMMAPV1FlushLock()));
        }
    }


    AutoAcquireFlushLockForMMAPV1Commit::AutoAcquireFlushLockForMMAPV1Commit(Locker* locker)
        : _locker(locker),
          _released(false) {

        invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush, MODE_S));
    }

    void AutoAcquireFlushLockForMMAPV1Commit::upgradeFlushLockToExclusive() {
        invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush, MODE_X));

        // Lock bumps the recursive count. Drop it back down so that the destructor doesn't
        // complain.
        invariant(!_locker->unlock(resourceIdMMAPV1Flush));
    }

    void AutoAcquireFlushLockForMMAPV1Commit::release() {
        if (!_released) {
            invariant(_locker->unlock(resourceIdMMAPV1Flush));
            _released = true;
        }
    }

    AutoAcquireFlushLockForMMAPV1Commit::~AutoAcquireFlushLockForMMAPV1Commit() {
        release();
    }


    //
    // Standalone functions
    //

    LockManager* getGlobalLockManager() {
        return &globalLockManager;
    }

    void reportGlobalLockingStats(LockStats* outStats) {
        globalStats.report(outStats);
    }

    void resetGlobalLockStats() {
        globalStats.reset();
    }

    
    // Ensures that there are two instances compiled for LockerImpl for the two values of the
    // template argument.
    template class LockerImpl<true>;
    template class LockerImpl<false>;

    // Definition for the hardcoded localdb and oplog collection info
    const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, StringData("local"));
    const ResourceId resourceIdOplog =
        ResourceId(RESOURCE_COLLECTION, StringData("local.oplog.rs"));
    const ResourceId resourceIdAdminDB = ResourceId(RESOURCE_DATABASE, StringData("admin"));

} // namespace mongo
