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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

    namespace {

        // Global lock manager instance.
        LockManager globalLockManager;

        // Global lock. Every server operation, which uses the Locker must acquire this lock at
        // least once. See comments in the header file (begin/endTransaction) for more information
        // on its use.
        const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, 1ULL);

        // Flush lock. This is only used for the MMAP V1 storage engine and synchronizes the
        // application of journal writes to the shared view and remaps. See the comments in the
        // header for _acquireFlushLockForMMAPV1/_releaseFlushLockForMMAPV1 for more information
        // on its use.
        const ResourceId resourceIdMMAPV1Flush = ResourceId(RESOURCE_MMAPV1_FLUSH, 2ULL);

        // How often to check for deadlock if a lock has not been granted for some time
        const Microseconds DeadlockTimeout(100*1000);

        /**
         * Used to sort locks by granularity when snapshotting lock state. We must report and
         * reacquire locks in the same granularity in which they are acquired (i.e. global, flush,
         * database, collection, etc).
         */
        struct SortByGranularity {
            inline bool operator()(const Locker::OneLock& lhs, const Locker::OneLock& rhs) {
                return lhs.resourceId.getType() < rhs.resourceId.getType();
            }
        };

        /**
         * Returns whether the passed in mode is S or IS. Used for validation checks.
         */
        bool isSharedMode(LockMode mode) {
            return (mode == MODE_IS || mode == MODE_S);
        }

        /**
         * Whether the particular lock's release should be held until the end of the operation. We
         * delay releases for exclusive locks (locks that are for write operations) in order to
         * ensure that the data they protect is committed successfully.
         */
        bool shouldDelayUnlock(const ResourceId& resId, LockMode mode) {
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

        /**
         * Dumps the contents of the global lock manager to the server log for diagnostics.
         */
        const uint64_t LockMgrDumpThrottleMicros = 30 * Timer::microsPerSecond;
        AtomicUInt64 lastDumpTimestampMicros(0);

        void dumpGlobalLockManagerAndCallstackThrottled(const Locker* locker) {
            const uint64_t lastDumpMicros = lastDumpTimestampMicros.load();

            // Don't print too frequently
            if (curTimeMicros64() - lastDumpMicros < LockMgrDumpThrottleMicros) return;

            // Only one thread should dump the lock manager in order to not pollute the log
            if (lastDumpTimestampMicros.compareAndSwap(lastDumpMicros,
                                                       curTimeMicros64()) == lastDumpMicros) {

                log() << "LockerId " << locker->getId()
                      << " has been waiting to acquire lock for more than 30 seconds. MongoDB will"
                      << " print the lock manager state and the stack of the thread that has been"
                      << " waiting, for diagnostic purposes. This message does not necessary imply"
                      << " that the server is experiencing an outage, but might be an indication"
                      << " of an overloaded server.";

                // Dump the lock manager state and the stack trace so we can investigate
                globalLockManager.dump();

                log() << '\n';
                printStackTrace();

                // If a deadlock was discovered, the server will never recover from it, so shutdown
                DeadlockDetector wfg(globalLockManager, locker);
                if (wfg.check().hasCycle()) {
                    severe() << "Deadlock found during lock acquisition: " << wfg.toString();
                    fassertFailed(28557);
                }
            }
        }
    }


    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isW() const {
        return getLockMode(resourceIdGlobal) == MODE_X;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isR() const {
        return getLockMode(resourceIdGlobal) == MODE_S;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::hasAnyReadLock() const {
        return isLockHeldForMode(resourceIdGlobal, MODE_IS);
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
    bool LockerImpl<IsForMMAPV1>::isWriteLocked(const StringData& ns) const {
        if (isWriteLocked()) {
            return true;
        }

        const StringData db = nsToDatabaseSubstring(ns);
        const ResourceId resIdNs(RESOURCE_DATABASE, db);

        return isLockHeldForMode(resIdNs, MODE_X);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isRecursive() const {
        return recursiveCount() > 1;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::assertWriteLocked(const StringData& ns) const {
        if (!isWriteLocked(ns)) {
            dump();
            msgasserted(
                16105, mongoutils::str::stream() << "expected to be write locked for " << ns);
        }
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

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::enterScopedLock(Lock::ScopedLock* lock) {
        _recursive++;
        if (_recursive == 1) {
            invariant(_scopedLk == NULL);
            _scopedLk = lock;
        }
    }

    template<bool IsForMMAPV1>
    Lock::ScopedLock* LockerImpl<IsForMMAPV1>::getCurrentScopedLock() const {
        invariant(_recursive == 1);
        return _scopedLk;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::leaveScopedLock(Lock::ScopedLock* lock) {
        if (_recursive == 1) {
            // Sanity check we are releasing the same lock
            invariant(_scopedLk == lock);
            _scopedLk = NULL;
        }
        _recursive--;
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

    LockResult CondVarLockGrantNotification::wait(int timeoutMs) {
        if (timeoutMs <= 0) return LOCK_TIMEOUT;

        boost::unique_lock<boost::mutex> lock(_mutex);
        while (_result == LOCK_INVALID) {
            if (!_cond.timed_wait(lock, Milliseconds(timeoutMs))) {
                // Timeout
                return LOCK_TIMEOUT;
            }
        }

        return _result;
    }

    void CondVarLockGrantNotification::notify(const ResourceId& resId, LockResult result) {
        boost::unique_lock<boost::mutex> lock(_mutex);
        invariant(_result == LOCK_INVALID);
        _result = result;

        _cond.notify_all();
    }


    //
    // Locker
    //

    template<bool IsForMMAPV1>
    LockerImpl<IsForMMAPV1>::LockerImpl(LockerId id)
        : _id(id),
          _wuowNestingLevel(0),
          _batchWriter(false),
          _lockPendingParallelWriter(false),
          _recursive(0),
          _scopedLk(NULL) {

    }

    template<bool IsForMMAPV1>
    LockerImpl<IsForMMAPV1>::~LockerImpl() {
        // Cannot delete the Locker while there are still outstanding requests, because the
        // LockManager may attempt to access deleted memory. Besides it is probably incorrect
        // to delete with unaccounted locks anyways.
        invariant(!inAWriteUnitOfWork());
        invariant(_resourcesToUnlockAtEndOfUnitOfWork.empty());
        invariant(_requests.empty());
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lockGlobal(LockMode mode,
                                                   Microseconds* timeBudgetRemaining) {
        LockRequestsMap::Iterator it = _requests.find(resourceIdGlobal);
        if (!it) {
            // Global lock should be the first lock on any operation
            invariant(_requests.empty());
        }
        else {
            // No upgrades on the GlobalLock are currently necessary. Should not be used until we
            // are handling deadlocks on anything other than the flush thread.
            invariant(it->mode >= mode);
        }

        LockResult globalLockResult = lock(resourceIdGlobal, mode, timeBudgetRemaining);
        if (globalLockResult != LOCK_OK) {
            invariant(globalLockResult == LOCK_TIMEOUT);

            return globalLockResult;
        }

        // Special-handling for MMAP V1 concurrency control
        if (IsForMMAPV1 && !it) {
            LockResult flushLockResult =
                lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock(), timeBudgetRemaining);

            if (flushLockResult != LOCK_OK) {
                invariant(flushLockResult == LOCK_TIMEOUT);
                invariant(unlock(resourceIdGlobal));

                return flushLockResult;
            }
        }

        return LOCK_OK;
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
                    lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock(), NULL, true);

                if (result == LOCK_OK) break;

                invariant(result == LOCK_DEADLOCK);
            }
        }
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lock(const ResourceId& resId,
                                             LockMode mode,
                                             Microseconds* timeBudgetRemaining,
                                             bool checkDeadlock) {

        LockResult result = lockImpl(resId, mode);

        // Fast, uncontended path
        if (result == LOCK_OK) return LOCK_OK;

        // Currently, deadlock detection does not happen inline with lock acquisition so the only
        // unsuccessful result that the lock manager would return is LOCK_WAITING.
        invariant(result == LOCK_WAITING);

        if (timeBudgetRemaining && *timeBudgetRemaining <= Microseconds(0)) return LOCK_TIMEOUT;

        // Tracks the lock acquisition time if we don't obtain the lock immediately
        Timer timer(timeBudgetRemaining ? Timer::START : Timer::DONT_START);

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

        // Don't go sleeping without bound in order to be able to report long waits or wake up for
        // deadlock detection.
        Microseconds timeToWait = DeadlockTimeout;
        if (timeBudgetRemaining && *timeBudgetRemaining < timeToWait)
            timeToWait = *timeBudgetRemaining;

        for (int attempt = 1; /*forever*/; attempt++) {
            result = _notify.wait(timeToWait.total_milliseconds());

            if (result == LOCK_OK) break;

            if (checkDeadlock) {
                DeadlockDetector wfg(globalLockManager, this);
                if (wfg.check().hasCycle()) {
                    log() << "Deadlock found: " << wfg.toString();

                    result = LOCK_DEADLOCK;
                    break;
                }
            }

            if (timeBudgetRemaining) {
                *timeBudgetRemaining -= Microseconds(timer.microsReset());
                if (*timeBudgetRemaining < timeToWait) {
                    timeToWait = *timeBudgetRemaining;
                }
            }

            if (timeToWait <= Microseconds(0)) {
                break;
            }

            // This will occasionally dump the global lock manager in case lock acquisition is
            // taking too long.
            static const int attemptsPerLog = 30*1000*1000 / DeadlockTimeout.total_microseconds();
            if ((attempt % attemptsPerLog) == 0) {
                dumpGlobalLockManagerAndCallstackThrottled(this);
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

        if (timeBudgetRemaining) *timeBudgetRemaining -= Microseconds(timer.micros());

        return result;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::downgrade(const ResourceId& resId, LockMode newMode) {
        LockRequestsMap::Iterator it = _requests.find(resId);
        globalLockManager.downgrade(it.objAddr(), newMode);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::unlock(const ResourceId& resId) {
        LockRequestsMap::Iterator it = _requests.find(resId);
        return _unlockImpl(it);
    }

    template<bool IsForMMAPV1>
    LockMode LockerImpl<IsForMMAPV1>::getLockMode(const ResourceId& resId) const {
        scoped_spinlock scopedLock(_lock);

        const LockRequestsMap::ConstIterator it = _requests.find(resId);
        if (!it) return MODE_NONE;

        return it->mode;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isLockHeldForMode(const ResourceId& resId, LockMode mode) const {
        return isModeCovered(mode, getLockMode(resId));
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isDbLockedForMode(const StringData& dbName,
                                                    LockMode mode) const {
        invariant(!nsIsFull(dbName));

        if (isW()) return true;
        if (isR() && isSharedMode(mode)) return true;

        const ResourceId resIdDb(RESOURCE_DATABASE, dbName);
        return isLockHeldForMode(resIdDb, mode);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isCollectionLockedForMode(const StringData& ns,
                                                            LockMode mode) const {
        invariant(nsIsFull(ns));

        if (isW()) return true;
        if (isR() && isSharedMode(mode)) return true;

        const NamespaceString nss(ns);
        const ResourceId resIdDb(RESOURCE_DATABASE, nss.db());

        LockMode dbMode = getLockMode(resIdDb);

        switch (dbMode) {
        case MODE_NONE: return false;
        case MODE_X: return true;
        case MODE_S: return isSharedMode(mode);
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
        if (IsForMMAPV1) {
            invariant(unlock(resourceIdMMAPV1Flush));
        }

        // Next, the non-global locks.
        for (LockRequestsMap::Iterator it = _requests.begin(); !it.finished(); it.next()) {
            const ResourceId& resId = it.key();

            // We don't support saving and restoring document-level locks.
            invariant(RESOURCE_DATABASE == resId.getType() ||
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

        lockGlobal(state.globalMode); // also handles MMAPV1Flush

        std::vector<OneLock>::const_iterator it = state.locks.begin();
        for (; it != state.locks.end(); it++) {
            invariant(LOCK_OK == lock(it->resourceId, it->mode));
        }
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lockImpl(const ResourceId& resId, LockMode mode) {
        LockRequest* request;

        LockRequestsMap::Iterator it = _requests.find(resId);
        if (!it) {
            scoped_spinlock scopedLock(_lock);
            LockRequestsMap::Iterator itNew = _requests.insert(resId);
            itNew->initNew(this, &_notify);

            request = itNew.objAddr();
        }
        else {
            request = it.objAddr();
        }

        _notify.clear();

        return globalLockManager.lock(resId, request, mode);
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
        : _locker(static_cast<MMAPV1LockerImpl*>(locker)),
          _isReleased(false) {

        invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush, MODE_S));
    }

    void AutoAcquireFlushLockForMMAPV1Commit::upgradeFlushLockToExclusive() {
        invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush, MODE_X));

        // Lock bumps the recursive count. Drop it back down so that the destructor doesn't
        // complain
        invariant(!_locker->unlock(resourceIdMMAPV1Flush));
    }

    void AutoAcquireFlushLockForMMAPV1Commit::release() {
        invariant(_locker->unlock(resourceIdMMAPV1Flush));
        _isReleased = true;
    }

    AutoAcquireFlushLockForMMAPV1Commit::~AutoAcquireFlushLockForMMAPV1Commit() {
        if (!_isReleased) {
            invariant(_locker->unlock(resourceIdMMAPV1Flush));
        }
    }


    //
    // Standalone functions
    //

    LockManager* getGlobalLockManager() {
        return &globalLockManager;
    }

    
    // Ensures that there are two instances compiled for LockerImpl for the two values of the
    // template argument.
    template class LockerImpl<true>;
    template class LockerImpl<false>;

} // namespace mongo
