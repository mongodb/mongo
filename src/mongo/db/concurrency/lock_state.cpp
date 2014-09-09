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

#include "mongo/db/namespace_string.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"


namespace mongo {

    // Counts the Locker instance identifiers
    static AtomicUInt64 idCounter(0);



    LockState::LockState() : Locker(idCounter.addAndFetch(1)) {

    }

namespace newlm {

    bool Locker::isRW() const { 
        return _threadState == 'R' || _threadState == 'W'; 
    }

    bool Locker::isW() const { 
        return _threadState == 'W'; 
    }

    bool Locker::hasAnyReadLock() const { 
        return _threadState == 'r' || _threadState == 'R';
    }

    bool Locker::isLocked() const {
        return threadState() != 0;
    }

    bool Locker::isWriteLocked() const {
        return (threadState() == 'W' || threadState() == 'w');
    }

    bool Locker::isWriteLocked(const StringData& ns) const {
        if (isWriteLocked()) {
            return true;
        }

        const StringData db = nsToDatabaseSubstring(ns);
        const newlm::ResourceId resIdNs(newlm::RESOURCE_DATABASE, db);

        return isLockHeldForMode(resIdNs, newlm::MODE_X);
    }

    bool Locker::isAtLeastReadLocked(const StringData& ns) const {
        if (threadState() == 'R' || threadState() == 'W')
            return true; // global
        if (threadState() == 0)
            return false;

        const StringData db = nsToDatabaseSubstring(ns);
        const newlm::ResourceId resIdNs(newlm::RESOURCE_DATABASE, db);

        return isLockHeldForMode(resIdNs, newlm::MODE_S);
    }

    bool Locker::isLockedForCommitting() const {
        return threadState() == 'R' || threadState() == 'W';
    }

    bool Locker::isRecursive() const {
        return recursiveCount() > 1;
    }

    void Locker::assertWriteLocked(const StringData& ns) const {
        if (!isWriteLocked(ns)) {
            dump();
            msgasserted(
                16105, mongoutils::str::stream() << "expected to be write locked for " << ns);
        }
    }

    void Locker::assertAtLeastReadLocked(const StringData& ns) const {
        if (!isAtLeastReadLocked(ns)) {
            log() << "error expected " << ns << " to be locked " << std::endl;
            dump();
            msgasserted(
                16104, mongoutils::str::stream() << "expected to be read locked for " << ns);
        }
    }

    void Locker::lockedStart( char newState ) {
        _threadState = newState;
    }

    void Locker::unlocked() {
        _threadState = 0;
    }

    void Locker::changeLockState( char newState ) {
        fassert( 16169 , _threadState != 0 );
        _threadState = newState;
    }

    BSONObj Locker::reportState() {
        BSONObjBuilder b;
        reportState(&b);

        return b.obj();
    }
    
    /** Note: this is is called by the currentOp command, which is a different 
              thread. So be careful about thread safety here. For example reading 
              this->otherName would not be safe as-is!
    */
    void Locker::reportState(BSONObjBuilder* res) {
        BSONObjBuilder b;
        if( _threadState ) {
            char buf[2];
            buf[0] = _threadState; 
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

    void Locker::dump() const {
        char s = threadState();
        StringBuilder ss;
        ss << "lock status: ";
        if( s == 0 ){
            ss << "unlocked"; 
        }
        else {
            // SERVER-14978: Dump lock stats information
        }
        log() << ss.str() << std::endl;
    }

    void Locker::enterScopedLock(Lock::ScopedLock* lock) {
        _recursive++;
        if (_recursive == 1) {
            invariant(_scopedLk == NULL);
            _scopedLk = lock;
        }
    }

    Lock::ScopedLock* Locker::getCurrentScopedLock() const {
        invariant(_recursive == 1);
        return _scopedLk;
    }

    void Locker::leaveScopedLock(Lock::ScopedLock* lock) {
        if (_recursive == 1) {
            // Sanity check we are releasing the same lock
            invariant(_scopedLk == lock);
            _scopedLk = NULL;
        }
        _recursive--;
    }


    /**
     * Global lock manager instance.
     */
    static LockManager globalLockManagerInstance;
    static LockManager* globalLockManagerPtr = &globalLockManagerInstance;


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

    Locker::Locker(uint64_t id) 
        : _id(id),
          _batchWriter(false),
          _lockPendingParallelWriter(false),
          _recursive(0),
          _threadState(0),
          _scopedLk(NULL),
          _lockPending(false) {

    }

    Locker::~Locker() {
        // Cannot delete the Locker while there are still outstanding requests, because the
        // LockManager may attempt to access deleted memory. Besides it is probably incorrect
        // to delete with unaccounted locks anyways.
        invariant(_requests.empty());
    }

    LockResult Locker::lock(const ResourceId& resId, LockMode mode, unsigned timeoutMs) {
        return _lockImpl(resId, mode, timeoutMs);
    }

    bool Locker::unlock(const ResourceId& resId) {
        _lock.lock();
        LockRequest* request = _find(resId);
        invariant(request != NULL);
        invariant(request->mode != MODE_NONE);
        _lock.unlock();

        // Methods on the Locker class are always called single-threadly, so it is safe to release
        // the spin lock, which protects the Locker here. The only thing which could alter the
        // state of the request is deadlock detection, which however would synchronize on the
        // LockManager calls.

        return _unlockAndUpdateRequestsList(resId, request);
    }

    LockMode Locker::getLockMode(const ResourceId& resId) const {
        scoped_spinlock scopedLock(_lock);

        const LockRequest* request = _find(resId);
        if (request == NULL) return MODE_NONE;

        return request->mode;
    }

    bool Locker::isLockHeldForMode(const ResourceId& resId, LockMode mode) const {
        return getLockMode(resId) >= mode;
    }

    // Static
    void Locker::dumpGlobalLockManager() {
        globalLockManagerPtr->dump();
    }

    LockRequest* Locker::_find(const ResourceId& resId) const {
        LockRequestsMap::const_iterator it = _requests.find(resId);

        if (it == _requests.end()) return NULL;
        return it->second;
    }

    LockResult Locker::_lockImpl(const ResourceId& resId, LockMode mode, unsigned timeoutMs) {
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
            result = _notify.wait(timeoutMs);
        }

        if (result != LOCK_OK) {
            // Can only be LOCK_TIMEOUT, because the lock manager does not return any other errors
            // at this point. Could be LOCK_DEADLOCK, when deadlock detection is implemented.
            invariant(result == LOCK_TIMEOUT);
            invariant(_unlockAndUpdateRequestsList(resId, request));
        }

        return result;
    }

    bool Locker::_unlockAndUpdateRequestsList(const ResourceId& resId, LockRequest* request) {
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

    // Static
    void Locker::changeGlobalLockManagerForTestingOnly(LockManager* newLockMgr) {
        if (newLockMgr != NULL) {
            globalLockManagerPtr = newLockMgr;
        }
        else {
            globalLockManagerPtr = &globalLockManagerInstance;
        }
    }
    
} // namespace newlm
} // namespace mongo
