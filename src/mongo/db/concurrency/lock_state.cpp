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
namespace newlm {

    namespace {

        // Dispenses unique Locker instance identifiers
        AtomicUInt64 idCounter(0);

        // Global lock manager instance. We have a pointer an an instance so that they can be
        // changed and restored for unit-tests.
        LockManager globalLockManagerInstance;
        LockManager* globalLockManagerPtr = &globalLockManagerInstance;

        // Global lock. Every server operation, which uses the Locker must acquire this lock at
        // least once. See comments in the header file (begin/endTransaction) for more information
        // on its use.
        const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, string("GlobalLock"));

        // Flush lock. This is only used for the MMAP V1 storage engine and synchronizes the
        // application of journal writes to the shared view and remaps. See the comments in the
        // header for _acquireFlushLockForMMAPV1/_releaseFlushLockForMMAPV1 for more information
        // on its use.
        const ResourceId resourceIdMMAPV1Flush =
                                ResourceId(RESOURCE_MMAPV1_FLUSH, string("FlushLock"));

        const ResourceId resourceIdLocalDB =
                                ResourceId(RESOURCE_DATABASE, string("local"));
    }



    bool LockerImpl::isW() const {
        return getLockMode(resourceIdGlobal) == MODE_X;
    }

    bool LockerImpl::isR() const {
        return getLockMode(resourceIdGlobal) == MODE_S;
    }

    bool LockerImpl::hasAnyReadLock() const {
        return isLockHeldForMode(resourceIdGlobal, MODE_IS);
    }

    bool LockerImpl::isLocked() const {
        return getLockMode(resourceIdGlobal) != MODE_NONE;
    }

    bool LockerImpl::isWriteLocked() const {
        return isLockHeldForMode(resourceIdGlobal, MODE_IX);
    }

    bool LockerImpl::isWriteLocked(const StringData& ns) const {
        if (isWriteLocked()) {
            return true;
        }

        const StringData db = nsToDatabaseSubstring(ns);
        const newlm::ResourceId resIdNs(newlm::RESOURCE_DATABASE, db);

        return isLockHeldForMode(resIdNs, newlm::MODE_X);
    }

    bool LockerImpl::isAtLeastReadLocked(const StringData& ns) const {
        if (threadState() == 'R' || threadState() == 'W') {
            return true; // global
        }
        if (!isLocked()) {
            return false;
        }

        const StringData db = nsToDatabaseSubstring(ns);
        const newlm::ResourceId resIdDb(newlm::RESOURCE_DATABASE, db);

        // S on the database means we don't need to check further down the hierarchy
        if (isLockHeldForMode(resIdDb, newlm::MODE_S)) {
            return true;
        }

        if (!isLockHeldForMode(resIdDb, newlm::MODE_IS)) {
            return false;
        }

        if (nsIsFull(ns)) {
            const newlm::ResourceId resIdColl(newlm::RESOURCE_DATABASE, ns);
            return isLockHeldForMode(resIdColl, newlm::MODE_IS);
        }

        // IS on the database is not sufficient to call the database read-locked, because it won't
        // conflict with other IX operations.
        return false;
    }

    bool LockerImpl::isRecursive() const {
        return recursiveCount() > 1;
    }

    void LockerImpl::assertWriteLocked(const StringData& ns) const {
        if (!isWriteLocked(ns)) {
            dump();
            msgasserted(
                16105, mongoutils::str::stream() << "expected to be write locked for " << ns);
        }
    }

    void LockerImpl::assertAtLeastReadLocked(const StringData& ns) const {
        if (!isAtLeastReadLocked(ns)) {
            log() << "error expected " << ns << " to be locked " << std::endl;
            dump();
            msgasserted(
                16104, mongoutils::str::stream() << "expected to be read locked for " << ns);
        }
    }

    BSONObj LockerImpl::reportState() {
        BSONObjBuilder b;
        reportState(&b);

        return b.obj();
    }
    
    /** Note: this is is called by the currentOp command, which is a different 
              thread. So be careful about thread safety here. For example reading 
              this->otherName would not be safe as-is!
    */
    void LockerImpl::reportState(BSONObjBuilder* res) {
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

    char LockerImpl::threadState() const {
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

    void LockerImpl::dump() const {
        _lock.lock();
        StringBuilder ss;
        ss << "lock status: ";
        if (!isLocked()) {
            ss << "unlocked";
        }
        else {
            // SERVER-14978: Dump lock stats information
        }

        ss << " requests:";
        for (LockRequestsMap::const_iterator it = _requests.begin(); it != _requests.end(); ++it) {
            ss << " " << it->first.toString();
        }
        _lock.unlock();
        log() << ss.str() << std::endl;
    }

    void LockerImpl::enterScopedLock(Lock::ScopedLock* lock) {
        _recursive++;
        if (_recursive == 1) {
            invariant(_scopedLk == NULL);
            _scopedLk = lock;
        }
    }

    Lock::ScopedLock* LockerImpl::getCurrentScopedLock() const {
        invariant(_recursive == 1);
        return _scopedLk;
    }

    void LockerImpl::leaveScopedLock(Lock::ScopedLock* lock) {
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

    LockerImpl::LockerImpl(uint64_t id) 
        : _id(id),
          _wuowNestingLevel(0),
          _batchWriter(false),
          _lockPendingParallelWriter(false),
          _recursive(0),
          _scopedLk(NULL),
          _lockPending(false) {

    }

    LockerImpl::LockerImpl() 
        : _id(idCounter.addAndFetch(1)),
          _wuowNestingLevel(0),
          _batchWriter(false),
          _lockPendingParallelWriter(false),
          _recursive(0),
          _scopedLk(NULL),
          _lockPending(false) {

    }

    LockerImpl::~LockerImpl() {
        // Cannot delete the Locker while there are still outstanding requests, because the
        // LockManager may attempt to access deleted memory. Besides it is probably incorrect
        // to delete with unaccounted locks anyways.
        invariant(!inAWriteUnitOfWork());
        invariant(_resourcesToUnlockAtEndOfUnitOfWork.empty());
        invariant(_requests.empty());
    }

    LockResult LockerImpl::lockGlobal(LockMode mode, unsigned timeoutMs) {
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

        // Obey the requested timeout
        const unsigned elapsedTimeMs = timer.millis();
        const unsigned remainingTimeMs =
            elapsedTimeMs < timeoutMs ? (timeoutMs - elapsedTimeMs) : 0;

        if (request == NULL) {
            // Special-handling for MMAP V1.
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

    void LockerImpl::downgradeGlobalXtoSForMMAPV1() {
        invariant(!inAWriteUnitOfWork());

        // Only Global and Flush lock could be held at this point.
        invariant(_requests.size() == 2);

        LockRequest* globalLockRequest = _find(resourceIdGlobal);
        LockRequest* flushLockRequest = _find(resourceIdMMAPV1Flush);

        invariant(globalLockRequest->mode == MODE_X);
        invariant(globalLockRequest->recursiveCount == 1);
        invariant(flushLockRequest->mode == MODE_X);
        invariant(flushLockRequest->recursiveCount == 1);

        globalLockManagerPtr->downgrade(globalLockRequest, MODE_S);
        globalLockManagerPtr->downgrade(flushLockRequest, MODE_S);
    }

    bool LockerImpl::unlockGlobal() {
        if (!unlock(resourceIdGlobal)) {
            return false;
        }

        // Need to unlock the MMAPV1 flush lock, which should be the last lock held
        invariant(unlock(resourceIdMMAPV1Flush));
        invariant(_requests.empty());

        return true;
    }

    void LockerImpl::beginWriteUnitOfWork() {
        _wuowNestingLevel++;
    }

    void LockerImpl::endWriteUnitOfWork() {
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

        _yieldFlushLockForMMAPV1();
    }

    LockResult LockerImpl::lock(const ResourceId& resId, LockMode mode, unsigned timeoutMs) {
        _notify.clear();

        _lock.lock();
        LockRequest* request = _find(resId);
        if (request == NULL) {
            request = new LockRequest();
            request->initNew(resId, this, &_notify);

            _requests.insert(LockRequestsPair(resId, request));
        }
        else {
            invariant(request->recursiveCount > 0);
            request->notify = &_notify;
        }
        _lock.unlock();

        // Methods on the Locker class are always called single-threadly, so it is safe to release
        // the spin lock, which protects the Locker here. The only thing which could alter the
        // state of the request is deadlock detection, which however would synchronize on the
        // LockManager calls.

        LockResult result = globalLockManagerPtr->lock(resId, request, mode);
        if (result == LOCK_WAITING) {
            // Under MMAP V1 engine a deadlock can occur if a thread goes to sleep waiting on DB
            // lock, while holding the flush lock, so it has to be released. This is only correct
            // to do if not in a write unit of work.
            bool unlockedFlushLock = false;

            if (!inAWriteUnitOfWork() && 
                (resId != resourceIdGlobal) &&
                (resId != resourceIdMMAPV1Flush) &&
                (resId != resourceIdLocalDB)) {

                invariant(unlock(resourceIdMMAPV1Flush));
                unlockedFlushLock = true;
            }

            // Do the blocking outside of the flush lock (if not in a write unit of work)
            result = _notify.wait(timeoutMs);

            if (unlockedFlushLock) {
                // We cannot obey the timeout here, because it is not correct to return from the
                // lock request with the flush lock released.
                invariant(LOCK_OK ==
                    lock(resourceIdMMAPV1Flush, getLockMode(resourceIdGlobal), UINT_MAX));
            }
        }

        if (result != LOCK_OK) {
            // Can only be LOCK_TIMEOUT, because the lock manager does not return any other errors
            // at this point. Could be LOCK_DEADLOCK, when deadlock detection is implemented.
            invariant(result == LOCK_TIMEOUT);
            invariant(_unlockAndUpdateRequestsList(resId, request));
        }

        return result;
    }

namespace {
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
} // namespace

    bool LockerImpl::unlock(const ResourceId& resId) {
        LockRequest* request = _find(resId);
        invariant(request->mode != MODE_NONE);

        // Methods on the Locker class are always called single-threadly, so it is safe to release
        // the spin lock, which protects the Locker here. The only thing which could alter the
        // state of the request is deadlock detection, which however would synchronize on the
        // LockManager calls.

        if (inAWriteUnitOfWork() && shouldDelayUnlock(resId, request->mode)) {
            _resourcesToUnlockAtEndOfUnitOfWork.push(resId);
            return false;
        }

        return _unlockAndUpdateRequestsList(resId, request);
    }

    LockMode LockerImpl::getLockMode(const ResourceId& resId) const {
        scoped_spinlock scopedLock(_lock);

        const LockRequest* request = _find(resId);
        if (request == NULL) return MODE_NONE;

        return request->mode;
    }

    bool LockerImpl::isLockHeldForMode(const ResourceId& resId, LockMode mode) const {
        return getLockMode(resId) >= mode;
    }

    // Static
    void LockerImpl::dumpGlobalLockManager() {
        globalLockManagerPtr->dump();
    }

    LockRequest* LockerImpl::_find(const ResourceId& resId) const {
        LockRequestsMap::const_iterator it = _requests.find(resId);

        if (it == _requests.end()) return NULL;
        return it->second;
    }

    bool LockerImpl::_unlockAndUpdateRequestsList(const ResourceId& resId, LockRequest* request) {
        globalLockManagerPtr->unlock(request);

        const int recursiveCount = request->recursiveCount;

        if (recursiveCount == 0) {
            _lock.lock();

            const int numErased = _requests.erase(resId);
            invariant(numErased == 1);

            _lock.unlock();

            // TODO: At some point we might want to cache a couple of these at least for the locks
            // which are acquired frequently (Global/Flush/DB) in order to reduce the number of
            // memory allocations.
            delete request;
        }

        return recursiveCount == 0;
    }

    void LockerImpl::_yieldFlushLockForMMAPV1() {
        if (!inAWriteUnitOfWork()) {
            invariant(unlock(resourceIdMMAPV1Flush));
            invariant(LOCK_OK ==
                lock(resourceIdMMAPV1Flush, getLockMode(resourceIdGlobal), UINT_MAX));
        }
    }

    // Static
    void LockerImpl::changeGlobalLockManagerForTestingOnly(LockManager* newLockMgr) {
        if (newLockMgr != NULL) {
            globalLockManagerPtr = newLockMgr;
        }
        else {
            globalLockManagerPtr = &globalLockManagerInstance;
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

} // namespace newlm
} // namespace mongo
