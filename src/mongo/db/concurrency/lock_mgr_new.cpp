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

#include "mongo/db/concurrency/lock_mgr_new.h"

#include <boost/thread/locks.hpp>

#include "mongo/db/concurrency/lock_state.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/timer.h"


namespace mongo {
namespace newlm {

    /**
     * Map of conflicts. 'LockConflictsTable[newMode] & existingMode != 0' means that a new request
     * with the given 'newMode' conflicts with an existing request with mode 'existingMode'.
     */
    static const int LockConflictsTable[] = {
        // MODE_NONE
        0,

        // MODE_IS
        (1 << MODE_X),

        // MODE_IX
        (1 << MODE_S) | (1 << MODE_X),

        // MODE_S
        (1 << MODE_IX) | (1 << MODE_X),

        // MODE_X
        (1 << MODE_S) | (1 << MODE_X) | (1 << MODE_IS) | (1 << MODE_IX),
    };

    /**
     * Maps the mode id to a string.
     */
    static const char* LockNames[] = {
        "NONE", "IS", "IX", "S", "X"
    };

    // Helper functions for the lock modes
    inline bool conflicts(LockMode newMode, uint32_t existingModesMask) {
        return (LockConflictsTable[newMode] & existingModesMask) != 0;
    }

    inline uint32_t modeMask(LockMode mode) {
        return 1 << mode;
    }

    inline const char* modeName(LockMode mode) {
        return LockNames[mode];
    }


    //
    // LockManager
    //

    LockManager::LockManager() : _noCheckForLeakedLocksTestOnly(false) {
        // TODO: Generate this based on the # of CPUs. For now, use 1 bucket to make debugging
        // easier.
        _numLockBuckets = 1;
        _lockBuckets = new LockBucket[_numLockBuckets];
    }

    LockManager::~LockManager() {
        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];

            // TODO: dump more information about the non-empty bucket to see what locks were leaked
            if (!_noCheckForLeakedLocksTestOnly) {
                invariant(bucket->data.empty());
            }
        }
    }

    LockResult LockManager::lock(const ResourceId& resId, LockRequest* request, LockMode mode) {
        LockBucket* bucket = _getBucket(resId);
        scoped_spinlock scopedLock(bucket->mutex);

        LockHeadMap::iterator it = bucket->data.find(resId);

        if (it == bucket->data.end()) {
            // Lock is free (not on the map)
            invariant(request->status == LockRequest::STATUS_NEW);

            request->prev = NULL;
            request->next = NULL;
            request->status = LockRequest::STATUS_GRANTED;
            request->mode = mode;
            request->convertMode = MODE_NONE;
            request->recursiveCount = 1;

            LockHead* lock = new LockHead(resId);
            lock->grantedQueue = request;
            lock->grantedModes = modeMask(mode);
            lock->conflictQueueBegin = NULL;
            lock->conflictQueueEnd = NULL;

            bucket->data.insert(LockHeadPair(resId, lock));
            return LOCK_OK;
        }

        // Lock not free case (still under the spin-lock for the bucket)
        LockHead* lock = it->second;

        invariant(lock->grantedQueue != NULL);
        invariant(lock->grantedModes != 0);

        if (request->status == LockRequest::STATUS_NEW) {
            // New lock request
            if (conflicts(mode, lock->grantedModes)) {
                // Put it on the wait queue. This is the place where various policies could be
                // applied for where in the wait queue does a request go.
                if (lock->conflictQueueBegin == NULL) {
                    invariant(lock->conflictQueueEnd == NULL);

                    request->prev = NULL;
                    request->next = NULL;
                    request->status = LockRequest::STATUS_WAITING;
                    request->mode = mode;
                    request->convertMode = MODE_NONE;
                    request->recursiveCount = 1;

                    lock->conflictQueueBegin = request;
                    lock->conflictQueueEnd = request;
                }
                else {
                    invariant(lock->conflictQueueEnd != NULL);

                    request->prev = lock->conflictQueueEnd;
                    request->next = NULL;
                    request->status = LockRequest::STATUS_WAITING;
                    request->mode = mode;
                    request->convertMode = MODE_NONE;
                    request->recursiveCount = 1;

                    lock->conflictQueueEnd->next = request;
                    lock->conflictQueueEnd = request;
                }

                return LOCK_WAITING;
            }
            else {  // No conflict, new request
                request->prev = NULL;
                request->next = lock->grantedQueue;
                request->status = LockRequest::STATUS_GRANTED;
                request->mode = mode;
                request->convertMode = MODE_NONE;
                request->recursiveCount = 1;

                if (lock->grantedQueue != NULL) {
                    lock->grantedQueue->prev = request;
                }

                lock->grantedQueue = request;
                lock->grantedModes |= modeMask(mode);

                return LOCK_OK;
            }
        }
        else {
            // Relock or conversion. To keep it simple, cannot request a conversion while a lock
            // is waiting or pending conversion.
            invariant(request->status == LockRequest::STATUS_GRANTED);

            if (conflicts(mode, lock->grantedModes)) {
                request->status = LockRequest::STATUS_CONVERTING;
                request->convertMode = mode;
                request->recursiveCount++;

                // This will update the grantedModes on the lock
                _recalcAndGrant(lock, NULL, request);

                return (request->status == LockRequest::STATUS_GRANTED ? LOCK_OK : LOCK_WAITING);
            }
            else {  // No conflict, existing request
                request->mode = std::max(request->mode, mode);
                request->recursiveCount++;

                lock->grantedModes |= modeMask(mode);
                return LOCK_OK;
            }
        }
    }

    void LockManager::unlock(LockRequest* request) {
        LockBucket* bucket = _getBucket(request->resourceId);
        scoped_spinlock scopedLock(bucket->mutex);

        LockHeadMap::iterator it = bucket->data.find(request->resourceId);

        // We should not have empty locks in the LockManager and should never try to unlock a lock
        // that's not been acquired previously
        invariant(it != bucket->data.end());

        LockHead* lock = it->second;

        invariant(lock->grantedQueue != NULL);
        invariant(lock->grantedModes != 0);

        request->recursiveCount--;

        bool recalcGrantedModes = false;

        if (request->status == LockRequest::STATUS_WAITING) {
            // This cancels a pending lock request
            invariant(request->recursiveCount == 0);
            recalcGrantedModes = true;
        }
        else if (request->status == LockRequest::STATUS_CONVERTING) {
            // This cancels a pending convert request
            invariant(request->recursiveCount > 0);

            // Lock only goes from GRANTED to CONVERTING, so cancelling the conversion request
            // brings it back to the previous granted mode.
            request->status = LockRequest::STATUS_GRANTED;
            request->convertMode = MODE_NONE;

            recalcGrantedModes = true;
        }
        else {
            // This releases an existing lock
            invariant(request->status == LockRequest::STATUS_GRANTED);
            invariant(request->recursiveCount >= 0);

            if (request->recursiveCount == 0) {
                recalcGrantedModes = true;
            }
        }

        if (!recalcGrantedModes) return;

        _recalcAndGrant(lock, request, NULL);

        // If some locks have been granted then there should be something on the grantedQueue and
        // vice versa (sanity check that either one or the other is true).
        invariant((lock->grantedModes == 0) ^ (lock->grantedQueue != NULL));

        // This lock is no longer in use
        if (lock->grantedModes == 0) {
            bucket->data.erase(it);

            // TODO: As an optimization, we could keep a cache of pre-allocated LockHead objects
            delete lock;
        }
    }

    void LockManager::downgrade(LockRequest* request, LockMode newMode) {
        invariant(request->status == LockRequest::STATUS_GRANTED);
        invariant(request->recursiveCount > 0);

        // The conflict set of the newMode should be a subset of the conflict set of the old mode.
        // Can't downgrade from S -> IX for example.
        invariant((LockConflictsTable[request->mode] | LockConflictsTable[newMode]) 
                                == LockConflictsTable[request->mode]);

        LockBucket* bucket = _getBucket(request->resourceId);
        scoped_spinlock scopedLock(bucket->mutex);

        LockHeadMap::iterator it = bucket->data.find(request->resourceId);

        // We should not have empty locks in the LockManager and should never try to unlock a lock
        // that's not been acquired previously
        invariant(it != bucket->data.end());

        LockHead* lock = it->second;

        invariant(lock->grantedQueue != NULL);
        invariant(lock->grantedModes != 0);

        request->mode = newMode;

        _recalcAndGrant(it->second, NULL, NULL);
    }

    void LockManager::setNoCheckForLeakedLocksTestOnly(bool newValue) {
        _noCheckForLeakedLocksTestOnly = newValue;
    }

    void LockManager::_recalcAndGrant(LockHead* lock,
                                      LockRequest* unlocked,
                                      LockRequest* converting) {

        // Either unlocked or converting must be specified, but not both. There are fixed callers
        // of this method, so dassert.
        dassert(!unlocked || !converting);

        // Resets the granted modes mask
        lock->grantedModes = 0;

        uint32_t convertModes = 0;
        
        // First iterate through all owners of the Lock in order to determine the existing granted
        // mode. The way this loop is implemented would mean linear complexity of the removal for
        // each removal, which might turn out to be expensive. There are ways to fix it throgh
        // keeping counts per lock-type, but those will be done once we have confirmed correctness.
        for (LockRequest* iter = lock->grantedQueue; iter != NULL; iter = iter->next) {

            // If the recursiveCount of this request is zero, this should be the lock that was just
            // unlocked by the LockManager::unlock call. There should be exactly one for the unlock
            // case and none for the recalc on downgrade case (where changed != NULL).
            if (iter->recursiveCount == 0) {
                invariant(iter == unlocked);

                // If recursive count is zero, this request must be removed from the granted list
                // and it does not participate in the granted mode anymore.
                if (iter->prev != NULL) {
                    iter->prev->next = iter->next;
                }
                else {
                    lock->grantedQueue = iter->next;
                }

                if (iter->next != NULL) {
                    iter->next->prev = iter->prev;
                }

                continue;
            }

            // Conversion requests are going in a separate queue
            if (iter->status == LockRequest::STATUS_CONVERTING) {
                invariant(iter->convertMode != 0);
                convertModes |= iter->convertMode;

                continue;
            }

            invariant(iter->convertMode == 0);

            // All the others, just add them to the mask
            lock->grantedModes |= modeMask(iter->mode);
        }

        // Potentially grant any conversion requests by doing another pass on the granted queue. We
        // cannot do this pass together with the loop above, because there could be a conversion
        // request which would conflict with an already granted request later in the queue, but we
        // will see it first. E.g., consider this:
        //
        // GrantedQueue -> (S converts to X) -> (S) -> (S) -> NULL
        //
        // Now, if the first S is converted to X, since the grantedModes on the lock are reset to
        // zero, it will be granted, because there is no conflict yet, which is a bug.
        if (convertModes) {
            for (LockRequest* iter = lock->grantedQueue; iter != NULL; iter = iter->next) {
                // Grant any conversion requests, which are compatible with the lock mask which has
                // been granted so far.
                if ((iter->status == LockRequest::STATUS_CONVERTING) &&
                     !conflicts(iter->convertMode, lock->grantedModes)) {

                    iter->status = LockRequest::STATUS_GRANTED;

                    // Locks are in strictly increasing strictness, so the convert mode would be
                    // higher than the existing mode
                    iter->mode = std::max(iter->mode, iter->convertMode);
                    iter->convertMode = MODE_NONE;

                    lock->grantedModes |= modeMask(iter->mode);

                    // The caller can infer that the lock was granted from the new mode and from
                    // the status changing to granted.
                    if (iter != converting) {
                        iter->notify->notify(lock->resourceId, LOCK_OK);
                    }
                }
            }

            // Throw in all the convert modes so that no new requests would be granted if there is
            // a pending conflicting conversion request
            lock->grantedModes |= convertModes;
        }

        // Grant all other requests next
        LockRequest* iterNext = NULL;

        for (LockRequest* iter = lock->conflictQueueBegin; iter != NULL; iter = iterNext) {
            invariant(iter != converting);
            invariant(iter->status == LockRequest::STATUS_WAITING);

            // Store the actual next pointer, because we muck with the iter below and move it to
            // the granted queue.
            iterNext = iter->next;

            const bool cancelWaiting = (iter == unlocked);
            const bool conflict = conflicts(iter->mode, lock->grantedModes);

            // Remove eligible entries from the conflict queue (cancelled we will just drop,
            // non-conflicting we will add on the granted queue).
            if (cancelWaiting || !conflict) {
                // This is equivalent to the expression cancelWaiting => iter->recursiveCount == 0
                // i.e., if cancelling a waiting request, the recursive count should be zero.
                invariant(!cancelWaiting || iter->recursiveCount == 0);

                if (iter->prev != NULL) {
                    iter->prev->next = iter->next;
                }
                else {
                    lock->conflictQueueBegin = iter->next;
                }

                if (iter->next != NULL) {
                    iter->next->prev = iter->prev;
                }
                else {
                    lock->conflictQueueEnd = iter->prev;
                }

                // iter is detached at this point
                iter->next = NULL;
                iter->prev = NULL;
            }

            // This breaks the FIFO order and may potentially starve conflicters. Here is where the
            // fairness policy should come into play.
            if (cancelWaiting || conflict) continue;

            iter->next = lock->grantedQueue;
            iter->status = LockRequest::STATUS_GRANTED;

            if (lock->grantedQueue != NULL) {
                lock->grantedQueue->prev = iter;
            }
            lock->grantedQueue = iter;
            lock->grantedModes |= modeMask(iter->mode);

            iter->notify->notify(lock->resourceId, LOCK_OK);
        }
    }

    LockManager::LockBucket* LockManager::_getBucket(const ResourceId& resId) {
        return &_lockBuckets[resId % _numLockBuckets];
    }

    void LockManager::dump() const {
        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];
            scoped_spinlock scopedLock(bucket->mutex);

            _dumpBucket(bucket);
        }
    }

    void LockManager::_dumpBucket(const LockBucket* bucket) const {
        LockHeadMap::const_iterator it = bucket->data.begin();
        while (it != bucket->data.end()) {
            const LockHead* lock = it->second;
            StringBuilder sb;
            sb << '\n' << "Lock " << lock << ": " << lock->resourceId.toString() << '\n';

            sb << "GRANTED:\n";
            for (const LockRequest* iter = lock->grantedQueue; iter != NULL; iter = iter->next) {
                sb << '\t'
                    << iter->locker->getId() << " @ " << iter->locker << ": "
                    << "Mode = " << modeName(iter->mode) << "; "
                    << "ConvertMode = " << modeName(iter->convertMode) << "; "
                    << '\n';
            }

            sb << '\n';

            sb << "PENDING:\n";
            for (const LockRequest* iter = lock->conflictQueueBegin;
                 iter != NULL;
                 iter = iter->next) {

                sb << '\t'
                    << iter->locker->getId() << " @ " << iter->locker << ": "
                    << "Mode = " << modeName(iter->mode) << "; "
                    << "ConvertMode = " << modeName(iter->convertMode) << "; "
                    << '\n';
            }

            log() << sb.str();

            it++;
        }
    }


    //
    // ResourceId
    //

    static const StringData::Hasher stringDataHashFunction = StringData::Hasher();

    ResourceId::ResourceId(ResourceType type, const StringData& ns) {
        _type = type;
        _hashId = stringDataHashFunction(ns) % 0x1fffffffffffffffULL;

        _nsCopy = ns.toString();
    }

    ResourceId::ResourceId(ResourceType type, const std::string& ns) {
        _type = type;
        _hashId = stringDataHashFunction(ns) % 0x1fffffffffffffffULL;

        _nsCopy = ns;
    }

    ResourceId::ResourceId(ResourceType type, uint64_t hashId) {
        _type = type;
        _hashId = hashId;
    }

    std::string ResourceId::toString() const {
        StringBuilder ss;
        ss << "{" << _fullHash << ": " << _type << ", " << _hashId << ", " << _nsCopy << "}";

        return ss.str();
    }


    //
    // LockHead
    //

    LockHead::LockHead(const ResourceId& resId)
        : resourceId(resId) {

    }

    LockHead::~LockHead() {
        invariant(grantedQueue == NULL);
        invariant(grantedModes == 0);
        invariant(conflictQueueBegin == NULL);
        invariant(conflictQueueEnd == NULL);
    }


    //
    // LockRequest
    //

    void LockRequest::initNew(const ResourceId& resourceId,
                              Locker* locker,
                              LockGrantNotification* notify) {
        this->resourceId = resourceId;
        this->locker = locker;
        this->notify = notify;        

        prev = NULL;
        next = NULL;
        status = STATUS_NEW;
        mode = MODE_NONE;
        convertMode = MODE_NONE;
        recursiveCount = 0;
    }

} // namespace newlm
} // namespace mongo
