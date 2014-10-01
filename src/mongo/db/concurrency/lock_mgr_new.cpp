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

#include "mongo/db/concurrency/locker.h"
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
        cleanupUnusedLocks();

        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];

            // TODO: dump more information about the non-empty bucket to see what locks were leaked
            if (!_noCheckForLeakedLocksTestOnly) {
                invariant(bucket->data.empty());
            }
        }
    }

    LockResult LockManager::lock(const ResourceId& resId, LockRequest* request, LockMode mode) {
        dassert(mode > MODE_NONE);

        // Fast path for acquiring the same lock multiple times in modes, which are already covered
        // by the current mode. It is safe to do this without locking, because 1) all calls for the
        // same lock request must be done on the same thread and 2) if there are lock requests
        // hanging off a given LockHead, then this lock will never disappear.
        if ((LockConflictsTable[request->mode] | LockConflictsTable[mode]) == 
                LockConflictsTable[request->mode]) {
            request->recursiveCount++;
            return LOCK_OK;
        }

        // TODO: For the time being we do not need conversions between unrelated lock modes (i.e.,
        // modes which both add and remove to the conflicts set), so these are not implemented yet
        // (e.g., S -> IX).
        invariant((LockConflictsTable[request->mode] | LockConflictsTable[mode]) == 
                LockConflictsTable[mode]);

        LockBucket* bucket = _getBucket(resId);
        scoped_spinlock scopedLock(bucket->mutex);

        LockHead* lock;

        LockHeadMap::iterator it = bucket->data.find(resId);
        if (it == bucket->data.end()) {
            // Lock is free (not on the map)
            invariant(request->status == LockRequest::STATUS_NEW);

            lock = new LockHead(resId);
            bucket->data.insert(LockHeadPair(resId, lock));
        }
        else {
            // Lock is not free
            lock = it->second;
        }

        // Sanity check if requests are being reused
        invariant(request->lock == NULL || request->lock == lock);

        request->lock = lock;
        request->recursiveCount++;

        if (request->status == LockRequest::STATUS_NEW) {
            invariant(request->recursiveCount == 1);

            // New lock request
            if (conflicts(mode, lock->grantedModes)) {
                request->status = LockRequest::STATUS_WAITING;
                request->mode = mode;
                request->convertMode = MODE_NONE;

                // Put it on the wait queue. This is the place where various policies could be
                // applied for where in the wait queue does a request go.
                if (lock->conflictQueueBegin == NULL) {
                    invariant(lock->conflictQueueEnd == NULL);

                    request->prev = NULL;
                    request->next = NULL;

                    lock->conflictQueueBegin = request;
                    lock->conflictQueueEnd = request;
                }
                else {
                    invariant(lock->conflictQueueEnd != NULL);

                    request->prev = lock->conflictQueueEnd;
                    request->next = NULL;

                    lock->conflictQueueEnd->next = request;
                    lock->conflictQueueEnd = request;
                }

                lock->changeRequestedModeCount(mode, LockHead::Increment);

                return LOCK_WAITING;
            }
            else {  // No conflict, new request
                request->prev = NULL;
                request->next = lock->grantedQueue;
                request->status = LockRequest::STATUS_GRANTED;
                request->mode = mode;
                request->convertMode = MODE_NONE;

                if (lock->grantedQueue != NULL) {
                    lock->grantedQueue->prev = request;
                }

                lock->grantedQueue = request;

                lock->changeGrantedModeCount(mode, LockHead::Increment);

                return LOCK_OK;
            }
        }
        else {
            // If we are here, we already hold the lock in some mode. In order to keep it simple,
            // we do not allow requesting a conversion while a lock is already waiting or pending
            // conversion, hence the assertion below.
            invariant(request->status == LockRequest::STATUS_GRANTED);
            invariant(request->recursiveCount > 1);
            invariant(request->mode != mode);

            // Construct granted mask without our current mode, so that it is not counted as
            // conflicting
            uint32_t grantedModesWithoutCurrentRequest = 0;

            // We start the counting at 1 below, because LockModesCount also includes MODE_NONE
            // at position 0, which can never be acquired/granted.
            for (uint32_t i = 1; i < LockModesCount; i++) {
                const uint32_t currentRequestHolds =
                                    (request->mode == static_cast<LockMode>(i) ? 1 : 0);

                if (lock->grantedCounts[i] > currentRequestHolds) {
                    grantedModesWithoutCurrentRequest |= modeMask(static_cast<LockMode>(i));
                }
            }

            // This check favours conversion requests over pending requests. For example:
            //
            // T1 requests lock L in IS
            // T2 requests lock L in X
            // T1 then upgrades L from IS -> S
            //
            // Because the check does not look into the requested modes bitmap, it will grant L
            // to T1 in S mode, instead of block, which would otherwise cause deadlock.
            if (conflicts(mode, grantedModesWithoutCurrentRequest)) {
                request->status = LockRequest::STATUS_CONVERTING;
                request->convertMode = mode;

                lock->changeGrantedModeCount(request->convertMode, LockHead::Increment);

                return LOCK_WAITING;
            }
            else {  // No conflict, existing request
                lock->changeGrantedModeCount(mode, LockHead::Increment);
                lock->changeGrantedModeCount(request->mode, LockHead::Decrement);
                request->mode = mode;

                return LOCK_OK;
            }
        }
    }

    bool LockManager::unlock(LockRequest* request) {
        invariant(request->lock);

        // Fast path for decrementing multiple references of the same lock. It is safe to do this
        // without locking, because 1) all calls for the same lock request must be done on the same
        // thread and 2) if there are lock requests hanging of a given LockHead, then this lock
        // will never disappear.
        request->recursiveCount--;
        if ((request->status == LockRequest::STATUS_GRANTED) && (request->recursiveCount > 0)) {
            return false;
        }

        LockHead* lock = request->lock;

        LockBucket* bucket = _getBucket(lock->resourceId);
        scoped_spinlock scopedLock(bucket->mutex);

        invariant(lock->grantedQueue != NULL);
        invariant(lock->grantedModes != 0);

        bool grantedModesChanged = false;

        if (request->status == LockRequest::STATUS_WAITING) {
            // This cancels a pending lock request
            invariant(request->recursiveCount == 0);

            // Remove from the conflict queue
            if (request->prev != NULL) {
                request->prev->next = request->next;
            }
            else {
                lock->conflictQueueBegin = request->next;
            }

            if (request->next != NULL) {
                request->next->prev = request->prev;
            }
            else {
                lock->conflictQueueEnd = request->prev;
            }

            request->prev = NULL;
            request->next = NULL;

            lock->changeRequestedModeCount(request->mode, LockHead::Decrement);
        }
        else if (request->status == LockRequest::STATUS_CONVERTING) {
            // This cancels a pending convert request
            invariant(request->recursiveCount > 0);

            // Lock only goes from GRANTED to CONVERTING, so cancelling the conversion request
            // brings it back to the previous granted mode.
            request->status = LockRequest::STATUS_GRANTED;

            lock->changeGrantedModeCount(request->convertMode, LockHead::Decrement);
            grantedModesChanged = true;

            request->convertMode = MODE_NONE;
        }
        else if (request->status == LockRequest::STATUS_GRANTED) {
            // This releases an existing lock
            invariant(request->recursiveCount == 0);

            // Remove from the granted list
            if (request->prev != NULL) {
                request->prev->next = request->next;
            }
            else {
                lock->grantedQueue = request->next;
            }

            if (request->next != NULL) {
                request->next->prev = request->prev;
            }

            lock->changeGrantedModeCount(request->mode, LockHead::Decrement);
            grantedModesChanged = true;
        }
        else {
            // Invalid request status
            invariant(false);
        }

        // Granted modes did not change, so nothing to be done
        if (grantedModesChanged) {
            _onLockModeChanged(lock);

            // If some locks have been granted then there should be something on the grantedQueue and
            // vice versa (sanity check that either one or the other is true).
            invariant((lock->grantedModes == 0) ^ (lock->grantedQueue != NULL));
        }

        return (request->recursiveCount == 0);
    }

    void LockManager::downgrade(LockRequest* request, LockMode newMode) {
        invariant(request->lock);
        invariant(request->status == LockRequest::STATUS_GRANTED);
        invariant(request->recursiveCount > 0);

        // The conflict set of the newMode should be a subset of the conflict set of the old mode.
        // Can't downgrade from S -> IX for example.
        invariant((LockConflictsTable[request->mode] | LockConflictsTable[newMode]) 
                                == LockConflictsTable[request->mode]);

        LockHead* lock = request->lock;

        LockBucket* bucket = _getBucket(lock->resourceId);
        scoped_spinlock scopedLock(bucket->mutex);

        invariant(lock->grantedQueue != NULL);
        invariant(lock->grantedModes != 0);

        lock->changeGrantedModeCount(newMode, LockHead::Increment);
        lock->changeGrantedModeCount(request->mode, LockHead::Decrement);
        request->mode = newMode;

        _onLockModeChanged(lock);
    }

    void LockManager::cleanupUnusedLocks() {
        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];
            scoped_spinlock scopedLock(bucket->mutex);

            LockHeadMap::iterator it = bucket->data.begin();
            while (it != bucket->data.end()) {
                LockHead* lock = it->second;
                if (lock->grantedModes == 0) {
                    invariant(lock->grantedQueue == NULL);
                    invariant(lock->conflictModes == 0);
                    invariant(lock->conflictQueueBegin == NULL);
                    invariant(lock->conflictQueueEnd == NULL);

                    bucket->data.erase(it++);
                    delete lock;
                }
                else {
                    it++;
                }
            }
        }
    }

    void LockManager::setNoCheckForLeakedLocksTestOnly(bool newValue) {
        _noCheckForLeakedLocksTestOnly = newValue;
    }

    void LockManager::_onLockModeChanged(LockHead* lock) {
        // Unblock any converting requests (because conversions are still counted as granted and
        // are on the granted queue).
        for (LockRequest* iter = lock->grantedQueue; iter != NULL; iter = iter->next) {
            // Conversion requests are going in a separate queue
            if (iter->status == LockRequest::STATUS_CONVERTING) {
                invariant(iter->convertMode != 0);

                // Construct granted mask without our current mode, so that it is not accounted as
                // a conflict
                uint32_t grantedModesWithoutCurrentRequest = 0;

                // We start the counting at 1 below, because LockModesCount also includes
                // MODE_NONE at position 0, which can never be acquired/granted.
                for (uint32_t i = 1; i < LockModesCount; i++) {
                    const uint32_t currentRequestHolds =
                        (iter->mode == static_cast<LockMode>(i) ? 1 : 0);

                    const uint32_t currentRequestWaits = 
                        (iter->convertMode == static_cast<LockMode>(i) ? 1 : 0);

                    // We cannot both hold and wait on the same lock mode
                    invariant(currentRequestHolds + currentRequestWaits <= 1);

                    if (lock->grantedCounts[i] > (currentRequestHolds + currentRequestWaits)) {
                        grantedModesWithoutCurrentRequest |= modeMask(static_cast<LockMode>(i));
                    }
                }

                if (!conflicts(iter->convertMode, grantedModesWithoutCurrentRequest)) {
                    lock->changeGrantedModeCount(iter->convertMode, LockHead::Increment);
                    lock->changeGrantedModeCount(iter->mode, LockHead::Decrement);
                    iter->mode = iter->convertMode;

                    iter->notify->notify(lock->resourceId, LOCK_OK);
                }
            }
            else {
                invariant(iter->status == LockRequest::STATUS_GRANTED);
            }
        }

        // Grant any conflicting requests, which might now be unblocked
        LockRequest* iterNext = NULL;

        for (LockRequest* iter = lock->conflictQueueBegin; iter != NULL; iter = iterNext) {
            invariant(iter->status == LockRequest::STATUS_WAITING);

            // Store the actual next pointer, because we muck with the iter below and move it to
            // the granted queue.
            iterNext = iter->next;

            if (conflicts(iter->mode, lock->grantedModes)) continue;

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

            // iter is detached from the granted queue at this point
            iter->next = NULL;
            iter->prev = NULL;

            iter->status = LockRequest::STATUS_GRANTED;

            iter->next = lock->grantedQueue;

            if (lock->grantedQueue != NULL) {
                lock->grantedQueue->prev = iter;
            }

            lock->grantedQueue = iter;

            lock->changeGrantedModeCount(iter->mode, LockHead::Increment);
            lock->changeRequestedModeCount(iter->mode, LockHead::Decrement);

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
        : resourceId(resId),
          grantedQueue(NULL),
          grantedModes(0),
          conflictQueueBegin(NULL),
          conflictQueueEnd(NULL),
          conflictModes(0) {

        memset(grantedCounts, 0, sizeof(grantedCounts));
        memset(conflictCounts, 0, sizeof(conflictCounts));
    }

    LockHead::~LockHead() {
        invariant(grantedQueue == NULL);
        invariant(grantedModes == 0);
        invariant(conflictQueueBegin == NULL);
        invariant(conflictQueueEnd == NULL);
        invariant(conflictModes == 0);
    }

    void LockHead::changeGrantedModeCount(LockMode mode, ChangeModeCountAction action) {
        if (action == Increment) {
            invariant(grantedCounts[mode] >= 0);
            if (++grantedCounts[mode] == 1) {
                invariant((grantedModes & modeMask(mode)) == 0);
                grantedModes |= modeMask(mode);
            }
        }
        else {
            invariant(action == Decrement);
            invariant(grantedCounts[mode] >= 1);
            if (--grantedCounts[mode] == 0) {
                invariant((grantedModes & modeMask(mode)) == modeMask(mode));
                grantedModes &= ~modeMask(mode);
            }
        }
    }

    void LockHead::changeRequestedModeCount(LockMode mode, ChangeModeCountAction action) {
        if (action == Increment) {
            invariant(conflictCounts[mode] >= 0);
            if (++conflictCounts[mode] == 1) {
                invariant((conflictModes & modeMask(mode)) == 0);
                conflictModes |= modeMask(mode);
            }
        }
        else {
            invariant(action == Decrement);
            invariant(conflictCounts[mode] >= 1);
            if (--conflictCounts[mode] == 0) {
                invariant((conflictModes & modeMask(mode)) == modeMask(mode));
                conflictModes &= ~modeMask(mode);
            }
        }
    }


    //
    // LockRequest
    //

    void LockRequest::initNew(Locker* locker, LockGrantNotification* notify) {
        this->locker = locker;
        this->notify = notify;

        lock = NULL;
        prev = NULL;
        next = NULL;
        status = STATUS_NEW;
        mode = MODE_NONE;
        convertMode = MODE_NONE;
        recursiveCount = 0;
    }

} // namespace newlm
} // namespace mongo
