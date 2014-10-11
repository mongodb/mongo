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

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/lock_state.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"


namespace mongo {

    namespace {

        // Dispenses unique Locker instance identifiers
        AtomicUInt64 idCounter(0);

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

        const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, string("local"));

        /**
         * Returns whether the passed in mode is S or IS. Used for validation checks.
         */
        bool isSharedMode(LockMode mode) {
            return (mode == MODE_IS || mode == MODE_S);
        }

        /**
         * Whether the particular lock's release should be held until the end of the operation.
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
                // unlocks of exclusive locks are delayed to the end of the WUOW
            case MODE_X:
            case MODE_IX:
                return true;

                // nothing else should be
            case MODE_IS:
            case MODE_S:
                return false;

                // these should never be passed in
            case MODE_NONE:
                invariant(false);
            }

            invariant(false);
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
    bool LockerImpl<IsForMMAPV1>::isDbLockedForMode(const StringData& dbName, LockMode mode) const {
        DEV {
            const NamespaceString nss(dbName);
            dassert(nss.coll().empty());
        };

        if (isW()) return true;
        if (isR() && isSharedMode(mode)) return true;

        const ResourceId resIdDb(RESOURCE_DATABASE, dbName);
        return isLockHeldForMode(resIdDb, mode);
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isAtLeastReadLocked(const StringData& ns) const {
        if (threadState() == 'R' || threadState() == 'W') {
            return true; // global
        }
        if (!isLocked()) {
            return false;
        }

        const StringData db = nsToDatabaseSubstring(ns);
        const ResourceId resIdDb(RESOURCE_DATABASE, db);

        // S on the database means we don't need to check further down the hierarchy
        if (isLockHeldForMode(resIdDb, MODE_S)) {
            return true;
        }

        if (!isLockHeldForMode(resIdDb, MODE_IS)) {
            return false;
        }

        if (nsIsFull(ns)) {
            const ResourceId resIdColl(RESOURCE_DATABASE, ns);
            return isLockHeldForMode(resIdColl, MODE_IS);
        }

        // We're just asking about a database, so IS on the db is enough.
        return true;
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
    BSONObj LockerImpl<IsForMMAPV1>::reportState() {
        BSONObjBuilder b;
        reportState(&b);

        return b.obj();
    }
    
    /** Note: this is is called by the currentOp command, which is a different 
              thread. So be careful about thread safety here. For example reading 
              this->otherName would not be safe as-is!
    */
    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::reportState(BSONObjBuilder* res) {
        BSONObjBuilder b;
        if (threadState()) {
            char buf[2];
            buf[0] = threadState();
            buf[1] = 0;
            b.append("^", buf);
        }

        // SERVER-14978: Report state from the Locker

        BSONObj o = b.obj();
        if (!o.isEmpty()) {
            res->append("locks", o);
        }
        res->append("waitingForLock", _lockPending);
    }

    template<bool IsForMMAPV1>
    char LockerImpl<IsForMMAPV1>::threadState() const {
        switch (getLockMode(resourceIdGlobal)) {
        case MODE_IS:
            return 'r';
        case MODE_IX:
            return 'w';
        case MODE_S:
            return 'R';
        case MODE_X:
            return 'W';
        case MODE_NONE:
            return '\0';
        }

        invariant(false);
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

        _lock.lock();
        ss << " requests:";
        for (LockRequestsMap::const_iterator it = _requests.begin(); it != _requests.end(); ++it) {
            ss << " " << it->first.toString();
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
    LockerImpl<IsForMMAPV1>::LockerImpl(uint64_t id) 
        : _id(id),
          _wuowNestingLevel(0),
          _batchWriter(false),
          _lockPendingParallelWriter(false),
          _recursive(0),
          _scopedLk(NULL),
          _lockPending(false) {

    }

    template<bool IsForMMAPV1>
    LockerImpl<IsForMMAPV1>::LockerImpl() 
        : _id(idCounter.addAndFetch(1)),
          _wuowNestingLevel(0),
          _batchWriter(false),
          _lockPendingParallelWriter(false),
          _recursive(0),
          _scopedLk(NULL),
          _lockPending(false) {

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
    LockResult LockerImpl<IsForMMAPV1>::lockGlobal(LockMode mode, unsigned timeoutMs) {
        LockRequest* request = _find(resourceIdGlobal);
        if (request != NULL) {
            // No upgrades on the GlobalLock are allowed until we can handle deadlocks.
            invariant(request->mode >= mode);
        }
        else {
            // Global lock should be the first lock on the operation
            invariant(_requests.empty());
        }

        Timer timer;

        LockResult globalLockResult = lock(resourceIdGlobal, mode, timeoutMs);
        if (globalLockResult != LOCK_OK) {
            invariant(globalLockResult == LOCK_TIMEOUT);

            return globalLockResult;
        }

        // Special-handling for MMAP V1 concurrency control
        if (IsForMMAPV1 && (request == NULL)) {
            // Obey the requested timeout
            const unsigned elapsedTimeMs = timer.millis();
            const unsigned remainingTimeMs =
                elapsedTimeMs < timeoutMs ? (timeoutMs - elapsedTimeMs) : 0;

            LockResult flushLockResult =
                lock(resourceIdMMAPV1Flush, getLockMode(resourceIdGlobal), remainingTimeMs);

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

        // Only Global and Flush lock could be held at this point.
        if ( IsForMMAPV1 ) {
            invariant(_requests.size() == 2);
        }
        else {
            invariant(_requests.size() == 1);
        }

        LockRequest* globalLockRequest = _find(resourceIdGlobal);
        invariant(globalLockRequest->mode == MODE_X);
        invariant(globalLockRequest->recursiveCount == 1);
        globalLockManager.downgrade(globalLockRequest, MODE_S);

        if (IsForMMAPV1) {
            LockRequest* flushLockRequest = _find(resourceIdMMAPV1Flush);
            invariant(flushLockRequest->mode == MODE_X);
            invariant(flushLockRequest->recursiveCount == 1);
            globalLockManager.downgrade(flushLockRequest, MODE_S);
        }
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::unlockAll() {
        if (!unlock(resourceIdGlobal)) {
            return false;
        }

        LockRequestsMap::const_iterator it = _requests.begin();
        while (it != _requests.end()) {
            const ResourceId& resId = it->first;

            // If we're here we should only have one reference to any lock.  Even if we're in
            // DBDirectClient or some other nested scope, we would have to release the global lock
            // fully before we get here. Therefore we're not here unless we've unlocked the global
            // lock, in which case it's a programming error to have > 1 reference.
            invariant(unlock(resId));

            // Unlocking modifies the state of _requests, but we're iterating over it, so we
            // have to start from the beginning every time we unlock something.
            it = _requests.begin();
        }

        return true;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::beginWriteUnitOfWork() {
        _wuowNestingLevel++;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::endWriteUnitOfWork() {
        _wuowNestingLevel--;
        if (_wuowNestingLevel > 0) {
            // Don't do anything unless leaving outermost WUOW.
            return;
        }

        invariant(_wuowNestingLevel == 0);

        while (!_resourcesToUnlockAtEndOfUnitOfWork.empty()) {
            unlock(_resourcesToUnlockAtEndOfUnitOfWork.front());
            _resourcesToUnlockAtEndOfUnitOfWork.pop();
        }

        if (IsForMMAPV1) {
            _yieldFlushLockForMMAPV1();
        }
    }

    template<bool IsForMMAPV1>
    LockResult LockerImpl<IsForMMAPV1>::lock(const ResourceId& resId,
                                             LockMode mode,
                                             unsigned timeoutMs) {

        _notify.clear();

        LockRequest* request = _find(resId);

        _lock.lock();
        if (request == NULL) {
            request = new LockRequest();
            request->initNew(this, &_notify);

            _requests.insert(LockRequestsPair(resId, request));
        }
        _lock.unlock();

        // Methods on the Locker class are always called single-threadly, so it is safe to release
        // the spin lock, which protects the Locker here. The only thing which could alter the
        // state of the request is deadlock detection, which however would synchronize on the
        // LockManager calls.

        LockResult result = globalLockManager.lock(resId, request, mode);
        if (result == LOCK_WAITING) {
            if (IsForMMAPV1) {
                // Under MMAP V1 engine a deadlock can occur if a thread goes to sleep waiting on
                // DB lock, while holding the flush lock, so it has to be released. This is only
                // correct to do if not in a write unit of work.
                bool unlockedFlushLock = false;

                if (!inAWriteUnitOfWork() &&
                    (resId != resourceIdGlobal) &&
                    (resId != resourceIdMMAPV1Flush) &&
                    (resId != resourceIdLocalDB)) {

                    invariant(unlock(resourceIdMMAPV1Flush));
                    unlockedFlushLock = true;
                }

                result = _notify.wait(timeoutMs);

                if (unlockedFlushLock) {
                    // We cannot obey the timeout here, because it is not correct to return from
                    // the lock request with the flush lock released.
                    invariant(LOCK_OK ==
                        lock(resourceIdMMAPV1Flush, getLockMode(resourceIdGlobal), UINT_MAX));
                }
            }
            else {
                result = _notify.wait(timeoutMs);
            }
        }

        if (result != LOCK_OK) {
            // Can only be LOCK_TIMEOUT, because the lock manager does not return any other errors
            // at this point. Could be LOCK_DEADLOCK, when deadlock detection is implemented.
            invariant(result == LOCK_TIMEOUT);

            if (globalLockManager.unlock(request)) {
                _freeRequest(resId, request);
            }
        }

        return result;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::unlock(const ResourceId& resId) {
        LockRequest* request = _find(resId);

        invariant(request);
        invariant(request->mode != MODE_NONE);

        // Methods on the Locker class are always called single-threadly, so it is safe to release
        // the spin lock, which protects the Locker here. The only thing which could alter the
        // state of the request is deadlock detection, which however would synchronize on the
        // LockManager calls.

        if (inAWriteUnitOfWork() && shouldDelayUnlock(resId, request->mode)) {
            _resourcesToUnlockAtEndOfUnitOfWork.push(resId);
            return false;
        }

        if (globalLockManager.unlock(request)) {
            _freeRequest(resId, request);
            return true;
        }

        return false;
    }

    template<bool IsForMMAPV1>
    LockMode LockerImpl<IsForMMAPV1>::getLockMode(const ResourceId& resId) const {
        scoped_spinlock scopedLock(_lock);

        const LockRequest* request = _find(resId);
        if (request == NULL) return MODE_NONE;

        return request->mode;
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::isLockHeldForMode(const ResourceId& resId, LockMode mode) const {
        return getLockMode(resId) >= mode;
    }

    namespace {
        /**
         * Used to sort locks by granularity when snapshotting lock state.
         * We must restore locks in increasing granularity
         * (ie global, then database, then collection...)
         */
        struct SortByGranularity {
            inline bool operator()(const Locker::LockSnapshot::OneLock& lhs,
                                   const Locker::LockSnapshot::OneLock& rhs) {

                return lhs.resourceId.getType() < rhs.resourceId.getType();
            }
        };
    }

    template<bool IsForMMAPV1>
    bool LockerImpl<IsForMMAPV1>::saveLockStateAndUnlock(Locker::LockSnapshot* stateOut) {
        // Clear out whatever is in stateOut.
        stateOut->locks.clear();
        stateOut->globalMode = MODE_NONE;
        stateOut->globalRecursiveCount = 0;

        // First, we look at the global lock.  There is special handling for this (as the flush
        // lock goes along with it) so we store it separately from the more pedestrian locks.
        LockRequest* globalRequest = _find(resourceIdGlobal);
        if (NULL == globalRequest) {
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

        // The global lock has been acquired just once.
        invariant(1 == globalRequest->recursiveCount);
        stateOut->globalMode = globalRequest->mode;
        stateOut->globalRecursiveCount = globalRequest->recursiveCount;

        // Flush lock state is inferred from the global state so we don't bother to store it.

        // Next, the non-global locks.
        for (LockRequestsMap::const_iterator it = _requests.begin(); it != _requests.end(); it++) {
            const ResourceId& resId = it->first;
            const LockRequest* request = it->second;

            // This is handled separately from normal locks as mentioned above.
            if (resourceIdGlobal == resId) {
                continue;
            }

            // This is an internal lock that is obtained when the global lock is locked.
            if (IsForMMAPV1 && (resourceIdMMAPV1Flush == resId)) {
                continue;
            }

            // We don't support saving and restoring document-level locks.
            invariant(RESOURCE_DATABASE == resId.getType() ||
                      RESOURCE_COLLECTION == resId.getType());

            // And, stuff the info into the out parameter.
            Locker::LockSnapshot::OneLock info;
            info.resourceId = resId;
            info.mode = request->mode;
            info.recursiveCount = request->recursiveCount;

            stateOut->locks.push_back(info);
        }

        // Sort locks from coarsest to finest.  They'll later be acquired in this order.
        std::sort(stateOut->locks.begin(), stateOut->locks.end(), SortByGranularity());

        // Unlock everything.

        // Step 1: Unlock all requests that are not-flush and not-global.
        for (size_t i = 0; i < stateOut->locks.size(); ++i) {
            for (size_t j = 0; j < stateOut->locks[i].recursiveCount; ++j) {
                unlock(stateOut->locks[i].resourceId);
            }
        }

        // Step 2: Unlock the global lock.
        for (size_t i = 0; i < stateOut->globalRecursiveCount; ++i) {
            unlock(resourceIdGlobal);
        }

        // Step 3: Unlock flush.  It's only acquired on the first global lock acquisition
        // so we only unlock it once.
        if (IsForMMAPV1) {
            invariant(unlock(resourceIdMMAPV1Flush));
        }
        return true;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::restoreLockState(const Locker::LockSnapshot& state) {
        // We expect to be able to unlock each lock 'recursiveCount' number of times.
        // So, we relock each lock that number of times.

        for (size_t i = 0; i < state.globalRecursiveCount; ++i) {
            lockGlobal(state.globalMode);
        }

        for (size_t i = 0; i < state.locks.size(); ++i) {
            for (size_t j = 0; j < state.locks[i].recursiveCount; ++j) {
                invariant(LOCK_OK == lock(state.locks[i].resourceId, state.locks[i].mode));
            }
        }
    }

    template<bool IsForMMAPV1>
    LockRequest* LockerImpl<IsForMMAPV1>::_find(const ResourceId& resId) const {
        LockRequestsMap::const_iterator it = _requests.find(resId);

        if (it == _requests.end()) return NULL;
        return it->second;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::_freeRequest(const ResourceId& resId, LockRequest* request) {
        _lock.lock();
        const int numErased = _requests.erase(resId);
        _lock.unlock();

        invariant(numErased == 1);

        // TODO: At some point we might want to cache a couple of these at least for the locks
        // which are acquired frequently (Global/Flush/DB) in order to reduce the number of
        // memory allocations.
        delete request;
    }

    template<bool IsForMMAPV1>
    void LockerImpl<IsForMMAPV1>::_yieldFlushLockForMMAPV1() {
        if (!inAWriteUnitOfWork()) {
            invariant(unlock(resourceIdMMAPV1Flush));
            invariant(LOCK_OK ==
                lock(resourceIdMMAPV1Flush, getLockMode(resourceIdGlobal), UINT_MAX));
        }
    }


    //
    // Auto classes
    //

    AutoYieldFlushLockForMMAPV1Commit::AutoYieldFlushLockForMMAPV1Commit(Locker* locker)
        : _locker(locker) {

        invariant(_locker->unlock(resourceIdMMAPV1Flush));
    }

    AutoYieldFlushLockForMMAPV1Commit::~AutoYieldFlushLockForMMAPV1Commit() {
        invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush,
                                           _locker->getLockMode(resourceIdGlobal),
                                           UINT_MAX));
    }


    AutoAcquireFlushLockForMMAPV1Commit::AutoAcquireFlushLockForMMAPV1Commit(Locker* locker)
        : _locker(locker) {

        invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush, MODE_X, UINT_MAX));
    }

    AutoAcquireFlushLockForMMAPV1Commit::~AutoAcquireFlushLockForMMAPV1Commit() {
        invariant(_locker->unlock(resourceIdMMAPV1Flush));
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
