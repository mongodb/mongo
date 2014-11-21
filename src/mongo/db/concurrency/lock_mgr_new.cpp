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

#include "mongo/db/concurrency/lock_mgr_new.h"

#include "mongo/db/concurrency/locker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace {

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

    // Ensure we do not add new modes without updating the conflicts table
    BOOST_STATIC_ASSERT(
        (sizeof(LockConflictsTable) / sizeof(LockConflictsTable[0])) == LockModesCount);


    /**
     * Maps the mode id to a string.
     */
    static const char* LockModeNames[] = {
        "NONE", "IS", "IX", "S", "X"
    };

    static const char* LegacyLockModeNames[] = {
        "", "r", "w", "R", "W"
    };

    // Ensure we do not add new modes without updating the names array
    BOOST_STATIC_ASSERT((sizeof(LockModeNames) / sizeof(LockModeNames[0])) == LockModesCount);
    BOOST_STATIC_ASSERT(
        (sizeof(LegacyLockModeNames) / sizeof(LegacyLockModeNames[0])) == LockModesCount);


    // Helper functions for the lock modes
    bool conflicts(LockMode newMode, uint32_t existingModesMask) {
        return (LockConflictsTable[newMode] & existingModesMask) != 0;
    }

    uint32_t modeMask(LockMode mode) {
        return 1 << mode;
    }


    /**
     * Maps the resource id to a human-readable string.
     */
    static const char* ResourceTypeNames[] = {
        "Invalid",
        "Global",
        "MMAPV1Flush",
        "Database",
        "Collection",
        "Metadata",
    };

    // Ensure we do not add new types without updating the names array
    BOOST_STATIC_ASSERT(
        (sizeof(ResourceTypeNames) / sizeof(ResourceTypeNames[0])) == ResourceTypesCount);

} // namespace


    /**
     * There is one of these objects per each resource which has a lock on it. Empty objects (i.e.
     * LockHead with no requests) are allowed to exist on the lock manager's hash table.
     *
     * The memory and lifetime is controlled entirely by the LockManager class.
     *
     * Not thread-safe and should only be accessed under the LockManager's bucket lock.
     */
    struct LockHead {

        /**
         * Used for initialization of a LockHead, which might have been retrieved from cache and
         * also in order to keep the LockHead structure a POD.
         */
        void initNew(ResourceId resId) {
            resourceId = resId;

            grantedList.reset();
            memset(grantedCounts, 0, sizeof(grantedCounts));
            grantedModes = 0;

            conflictList.reset();
            memset(conflictCounts, 0, sizeof(conflictCounts));
            conflictModes = 0;

            conversionsCount = 0;
            compatibleFirstCount = 0;
        }

        /**
         * Locates the request corresponding to the particular locker or returns NULL. Must be
         * called with the bucket holding this lock head locked.
         */
        LockRequest* findRequest(LockerId lockerId) const {
            // Check the granted queue first
            for (LockRequest* it = grantedList._front; it != NULL; it = it->next) {
                if (it->locker->getId() == lockerId) {
                    return it;
                }
            }

            // Check the conflict queue second
            for (LockRequest* it = conflictList._front; it != NULL; it = it->next) {
                if (it->locker->getId() == lockerId) {
                    return it;
                }
            }

            return NULL;
        }

        // Methods to maintain the granted queue
        void incGrantedModeCount(LockMode mode) {
            invariant(grantedCounts[mode] >= 0);
            if (++grantedCounts[mode] == 1) {
                invariant((grantedModes & modeMask(mode)) == 0);
                grantedModes |= modeMask(mode);
            }
        }

        void decGrantedModeCount(LockMode mode) {
            invariant(grantedCounts[mode] >= 1);
            if (--grantedCounts[mode] == 0) {
                invariant((grantedModes & modeMask(mode)) == modeMask(mode));
                grantedModes &= ~modeMask(mode);
            }
        }

        // Methods to maintain the conflict queue
        void incConflictModeCount(LockMode mode) {
            invariant(conflictCounts[mode] >= 0);
            if (++conflictCounts[mode] == 1) {
                invariant((conflictModes & modeMask(mode)) == 0);
                conflictModes |= modeMask(mode);
            }
        }

        void decConflictModeCount(LockMode mode) {
            invariant(conflictCounts[mode] >= 1);
            if (--conflictCounts[mode] == 0) {
                invariant((conflictModes & modeMask(mode)) == modeMask(mode));
                conflictModes &= ~modeMask(mode);
            }
        }


        // Id of the resource which this lock protects
        ResourceId resourceId;

        //
        // Granted queue
        //

        // Doubly-linked list of requests, which have been granted. Newly granted requests go to
        // the end of the queue. Conversion requests are granted from the beginning forward.
        LockRequestList grantedList;

        // Counts the grants and coversion counts for each of the supported lock modes. These
        // counts should exactly match the aggregated modes on the granted list.
        uint32_t grantedCounts[LockModesCount];

        // Bit-mask of the granted + converting modes on the granted queue. Maintained in lock-step
        // with the grantedCounts array.
        uint32_t grantedModes;


        //
        // Conflict queue
        //

        // Doubly-linked list of requests, which have not been granted yet because they conflict
        // with the set of granted modes. Requests are queued at the end of the queue and are
        // granted from the beginning forward, which gives these locks FIFO ordering. Exceptions
        // are high-priorty locks, such as the MMAP V1 flush lock.
        LockRequestList conflictList;

        // Counts the conflicting requests for each of the lock modes. These counts should exactly
        // match the aggregated modes on the conflicts list.
        uint32_t conflictCounts[LockModesCount];

        // Bit-mask of the conflict modes on the conflict queue. Maintained in lock-step with the
        // conflictCounts array.
        uint32_t conflictModes;


        //
        // Conversion
        //

        // Counts the number of requests on the granted queue, which have requested any kind of
        // conflicting conversion and are blocked (i.e. all requests which are currently
        // STATUS_CONVERTING). This is an optimization for unlocking in that we do not need to
        // check the granted queue for requests in STATUS_CONVERTING if this count is zero. This
        // saves cycles in the regular case and only burdens the less-frequent lock upgrade case.
        uint32_t conversionsCount;

        // Counts the number of requests on the granted queue, which have requested that the policy
        // be switched to compatible-first. As long as this value is > 0, the policy will stay
        // compatible-first.
        uint32_t compatibleFirstCount;
    };


    //
    // LockManager
    //

    // Have more buckets than CPUs to reduce contention on lock and caches
    const unsigned LockManager::_numLockBuckets(128);

    LockManager::LockManager() {
        _lockBuckets = new LockBucket[_numLockBuckets];
    }

    LockManager::~LockManager() {
        cleanupUnusedLocks();

        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];

            // TODO: dump more information about the non-empty bucket to see what locks were leaked
            invariant(bucket->data.empty());
        }

        delete[] _lockBuckets;
    }

    LockResult LockManager::lock(ResourceId resId, LockRequest* request, LockMode mode) {
        // Sanity check that requests are not being reused without proper cleanup
        invariant(request->status == LockRequest::STATUS_NEW);

        LockBucket* bucket = _getBucket(resId);
        SimpleMutex::scoped_lock scopedLock(bucket->mutex);

        LockHead* lock;

        LockHeadMap::iterator it = bucket->data.find(resId);
        if (it == bucket->data.end()) {
            lock = new LockHead();
            lock->initNew(resId);

            bucket->data.insert(LockHeadPair(resId, lock));
        }
        else {
            lock = it->second;
        }

        request->lock = lock;
        request->recursiveCount = 1;

        // New lock request. Queue after all granted modes and after any already requested
        // conflicting modes.
        if (conflicts(mode, lock->grantedModes) ||
                (!lock->compatibleFirstCount && conflicts(mode, lock->conflictModes))) {

            request->status = LockRequest::STATUS_WAITING;
            request->mode = mode;

            // Put it on the conflict queue. Conflicts are granted front to back.
            if (request->enqueueAtFront) {
                lock->conflictList.push_front(request);
            }
            else {
                lock->conflictList.push_back(request);
            }

            lock->incConflictModeCount(mode);

            return LOCK_WAITING;
        }
        else {
            // No conflict, new request
            request->status = LockRequest::STATUS_GRANTED;
            request->mode = mode;

            lock->grantedList.push_back(request);
            lock->incGrantedModeCount(mode);

            if (request->compatibleFirst) {
                lock->compatibleFirstCount++;
            }

            return LOCK_OK;
        }
    }

    LockResult LockManager::convert(ResourceId resId, LockRequest* request, LockMode newMode) {
        // If we are here, we already hold the lock in some mode. In order to keep it simple, we do
        // not allow requesting a conversion while a lock is already waiting or pending conversion.
        invariant(request->status == LockRequest::STATUS_GRANTED);
        invariant(request->recursiveCount > 0);

        request->recursiveCount++;

        // Fast path for acquiring the same lock multiple times in modes, which are already covered
        // by the current mode. It is safe to do this without locking, because 1) all calls for the
        // same lock request must be done on the same thread and 2) if there are lock requests
        // hanging off a given LockHead, then this lock will never disappear.
        if ((LockConflictsTable[request->mode] | LockConflictsTable[newMode]) ==
                                                        LockConflictsTable[request->mode]) {
            return LOCK_OK;
        }

        // TODO: For the time being we do not need conversions between unrelated lock modes (i.e.,
        // modes which both add and remove to the conflicts set), so these are not implemented yet
        // (e.g., S -> IX).
        invariant((LockConflictsTable[request->mode] | LockConflictsTable[newMode]) ==
                                                        LockConflictsTable[newMode]);

        LockBucket* bucket = _getBucket(resId);
        SimpleMutex::scoped_lock scopedLock(bucket->mutex);

        LockHeadMap::iterator it = bucket->data.find(resId);
        invariant(it != bucket->data.end());

        LockHead* const lock = it->second;

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
        // Because the check does not look into the conflict modes bitmap, it will grant L to
        // T1 in S mode, instead of block, which would otherwise cause deadlock.
        if (conflicts(newMode, grantedModesWithoutCurrentRequest)) {
            request->status = LockRequest::STATUS_CONVERTING;
            request->convertMode = newMode;

            lock->conversionsCount++;
            lock->incGrantedModeCount(request->convertMode);

            return LOCK_WAITING;
        }
        else {  // No conflict, existing request
            lock->incGrantedModeCount(newMode);
            lock->decGrantedModeCount(request->mode);
            request->mode = newMode;

            return LOCK_OK;
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
        SimpleMutex::scoped_lock scopedLock(bucket->mutex);

        if (request->status == LockRequest::STATUS_WAITING) {
            // This cancels a pending lock request
            invariant(request->recursiveCount == 0);

            lock->conflictList.remove(request);
            lock->decConflictModeCount(request->mode);
        }
        else if (request->status == LockRequest::STATUS_CONVERTING) {
            // This cancels a pending convert request
            invariant(request->recursiveCount > 0);

            // Lock only goes from GRANTED to CONVERTING, so cancelling the conversion request
            // brings it back to the previous granted mode.
            request->status = LockRequest::STATUS_GRANTED;

            lock->conversionsCount--;
            lock->decGrantedModeCount(request->convertMode);

            request->convertMode = MODE_NONE;

            _onLockModeChanged(lock, lock->grantedCounts[request->convertMode] == 0);
        }
        else if (request->status == LockRequest::STATUS_GRANTED) {
            // This releases a currently held lock and is the most common path, so it should be
            // as efficient as possible.
            invariant(request->recursiveCount == 0);

            // Remove from the granted list
            lock->grantedList.remove(request);
            lock->decGrantedModeCount(request->mode);

            if (request->compatibleFirst) {
                lock->compatibleFirstCount--;
            }

            _onLockModeChanged(lock, lock->grantedCounts[request->mode] == 0);
        }
        else {
            // Invalid request status
            invariant(false);
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
        SimpleMutex::scoped_lock scopedLock(bucket->mutex);

        lock->incGrantedModeCount(newMode);
        lock->decGrantedModeCount(request->mode);
        request->mode = newMode;

        _onLockModeChanged(lock, true);
    }

    void LockManager::cleanupUnusedLocks() {
        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];
            SimpleMutex::scoped_lock scopedLock(bucket->mutex);

            LockHeadMap::iterator it = bucket->data.begin();
            while (it != bucket->data.end()) {
                LockHead* lock = it->second;
                if (lock->grantedModes == 0) {
                    invariant(lock->grantedModes == 0);
                    invariant(lock->grantedList._front == NULL);
                    invariant(lock->grantedList._back == NULL);
                    invariant(lock->conflictModes == 0);
                    invariant(lock->conflictList._front == NULL);
                    invariant(lock->conflictList._back == NULL);
                    invariant(lock->conversionsCount == 0);
                    invariant(lock->compatibleFirstCount == 0);

                    bucket->data.erase(it++);
                    delete lock;
                }
                else {
                    it++;
                }
            }
        }
    }

    void LockManager::_onLockModeChanged(LockHead* lock, bool checkConflictQueue) {
        // Unblock any converting requests (because conversions are still counted as granted and
        // are on the granted queue).
        for (LockRequest* iter = lock->grantedList._front;
            (iter != NULL) && (lock->conversionsCount > 0);
            iter = iter->next) {

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
                    lock->conversionsCount--;
                    lock->decGrantedModeCount(iter->mode);
                    iter->status = LockRequest::STATUS_GRANTED;
                    iter->mode = iter->convertMode;
                    iter->convertMode = MODE_NONE;

                    iter->notify->notify(lock->resourceId, LOCK_OK);
                }
            }
        }

        // Grant any conflicting requests, which might now be unblocked
        LockRequest* iterNext = NULL;

        for (LockRequest* iter = lock->conflictList._front;
             (iter != NULL) && checkConflictQueue;
             iter = iterNext) {

            invariant(iter->status == LockRequest::STATUS_WAITING);

            // Store the actual next pointer, because we muck with the iter below and move it to
            // the granted queue.
            iterNext = iter->next;

            if (conflicts(iter->mode, lock->grantedModes)) continue;

            iter->status = LockRequest::STATUS_GRANTED;

            lock->conflictList.remove(iter);
            lock->grantedList.push_back(iter);

            lock->incGrantedModeCount(iter->mode);
            lock->decConflictModeCount(iter->mode);

            if (iter->compatibleFirst) {
                lock->compatibleFirstCount++;
            }

            iter->notify->notify(lock->resourceId, LOCK_OK);
        }

        // This is a convenient place to check that the state of the two request queues is in sync
        // with the bitmask on the modes.
        invariant((lock->grantedModes == 0) ^ (lock->grantedList._front != NULL));
        invariant((lock->conflictModes == 0) ^ (lock->conflictList._front != NULL));
    }

    LockManager::LockBucket* LockManager::_getBucket(ResourceId resId) const {
        return &_lockBuckets[resId % _numLockBuckets];
    }

    void LockManager::dump() const {
        log() << "Dumping LockManager @ " << static_cast<const void*>(this) << '\n';

        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];
            SimpleMutex::scoped_lock scopedLock(bucket->mutex);

            if (!bucket->data.empty()) {
                _dumpBucket(bucket);
            }
        }
    }

    void LockManager::_dumpBucket(const LockBucket* bucket) const {
        for (LockHeadMap::const_iterator it = bucket->data.begin();
             it != bucket->data.end();
             it++) {

            const LockHead* lock = it->second;

            if (lock->grantedList.empty()) {
                // If there are no granted requests, this lock is empty, so no need to print it
                continue;
            }

            StringBuilder sb;
            sb << "Lock @ " << lock << ": " << lock->resourceId.toString() << '\n';

            sb << "GRANTED:\n";
            for (const LockRequest* iter = lock->grantedList._front;
                 iter != NULL;
                 iter = iter->next) {

                sb << '\t'
                    << "LockRequest " << iter->locker->getId() << " @ " << iter->locker << ": "
                    << "Mode = " << modeName(iter->mode) << "; "
                    << "ConvertMode = " << modeName(iter->convertMode) << "; "
                    << "EnqueueAtFront = " << iter->enqueueAtFront << "; "
                    << "CompatibleFirst = " << iter->compatibleFirst << "; "
                    << '\n';
            }

            sb << '\n';

            sb << "PENDING:\n";
            for (const LockRequest* iter = lock->conflictList._front;
                 iter != NULL;
                 iter = iter->next) {

                sb << '\t'
                    << "LockRequest " << iter->locker->getId() << " @ " << iter->locker << ": "
                    << "Mode = " << modeName(iter->mode) << "; "
                    << "ConvertMode = " << modeName(iter->convertMode) << "; "
                    << "EnqueueAtFront = " << iter->enqueueAtFront << "; "
                    << "CompatibleFirst = " << iter->compatibleFirst << "; "
                    << '\n';
            }

            log() << sb.str();
        }
    }


    //
    // DeadlockDetector
    //

    DeadlockDetector::DeadlockDetector(const LockManager& lockMgr, const Locker* initialLocker)
            : _lockMgr(lockMgr),
              _initialLockerId(initialLocker->getId()),
              _foundCycle(false) {

        const ResourceId resId = initialLocker->getWaitingResource();

        // If there is no resource waiting there is nothing to do
        if (resId.isValid()) {
            _queue.push_front(UnprocessedNode(_initialLockerId, resId));
        }
    }

    bool DeadlockDetector::next() {
        if (_queue.empty()) return false;

        UnprocessedNode front = _queue.front();
        _queue.pop_front();

        _processNextNode(front);

        return !_queue.empty();
    }

    bool DeadlockDetector::hasCycle() const {
        invariant(_queue.empty());

        return _foundCycle;
    }

    string DeadlockDetector::toString() const {
        StringBuilder sb;

        for (WaitForGraph::const_iterator it = _graph.begin(); it != _graph.end(); it++) {
            sb << "Locker " << it->first << " waits for resource " << it->second.resId.toString()
               << " held by [";

            const ConflictingOwnersList owners = it->second.owners;
            for (ConflictingOwnersList::const_iterator itW = owners.begin();
                 itW != owners.end();
                 itW++) {

                sb << *itW << ", ";
            }

            sb << "]\n";
        }

        return sb.str();
    }

    void DeadlockDetector::_processNextNode(const UnprocessedNode& node) {
        // Locate the request
        LockManager::LockBucket* bucket = _lockMgr._getBucket(node.resId);
        SimpleMutex::scoped_lock scopedLock(bucket->mutex);

        LockManager::LockHeadMap::const_iterator iter = bucket->data.find(node.resId);
        if (iter == bucket->data.end()) {
            return;
        }

        const LockHead* lock = iter->second;

        LockRequest* request = lock->findRequest(node.lockerId);

        // It is possible that a request which was thought to be waiting suddenly became
        // granted, so check that before proceeding
        if (!request || (request->status == LockRequest::STATUS_GRANTED)) {
            return;
        }

        std::pair<WaitForGraph::iterator, bool> val =
            _graph.insert(WaitForGraphPair(node.lockerId, Edges(node.resId)));
        if (!val.second) {
            // We already saw this locker id, which means we have a cycle.
            if (!_foundCycle) {
                _foundCycle = (node.lockerId == _initialLockerId);
            }

            return;
        }

        Edges& edges = val.first->second;

        bool seen = false;
        for (LockRequest* it = lock->grantedList._back; it != NULL; it = it->prev) {
            // We can't conflict with ourselves
            if (it == request) {
                seen = true;
                continue;
            }

            // If we are a regular conflicting request, both granted and conversion modes need to
            // be checked for conflict, since conversions will be granted first.
            if (request->status == LockRequest::STATUS_WAITING) {
                if (conflicts(request->mode, modeMask(it->mode)) ||
                    conflicts(request->mode, modeMask(it->convertMode))) {

                    const LockerId lockerId = it->locker->getId();
                    const ResourceId waitResId = it->locker->getWaitingResource();

                    if (waitResId.isValid()) {
                        _queue.push_front(UnprocessedNode(lockerId, waitResId));
                        edges.owners.push_back(lockerId);
                    }
                }

                continue;
            }

            // If we are a conversion request, only requests, which are before us need to be
            // accounted for.
            invariant(request->status == LockRequest::STATUS_CONVERTING);

            if (conflicts(request->convertMode, modeMask(it->mode)) ||
                (seen && conflicts(request->convertMode, modeMask(it->convertMode)))) {

                const LockerId lockerId = it->locker->getId();
                const ResourceId waitResId = it->locker->getWaitingResource();

                if (waitResId.isValid()) {
                    _queue.push_front(UnprocessedNode(lockerId, waitResId));
                    edges.owners.push_back(lockerId);
                }
            }
        }

        // All conflicting waits, which would be granted before us
        for (LockRequest* it = request->prev;
             (request->status == LockRequest::STATUS_WAITING) &&  (it != NULL);
             it = it->prev) {

            // We started from the previous element, so we should never see ourselves
            invariant(it != request);

            if (conflicts(request->mode, modeMask(it->mode))) {
                const LockerId lockerId = it->locker->getId();
                const ResourceId waitResId = it->locker->getWaitingResource();

                if (waitResId.isValid()) {
                    _queue.push_front(UnprocessedNode(lockerId, waitResId));
                    edges.owners.push_back(lockerId);
                }
            }
        }
    }


    //
    // ResourceId
    //

    static const StringData::Hasher stringDataHashFunction = StringData::Hasher();

    uint64_t ResourceId::fullHash(ResourceType type, uint64_t hashId) {
        return (static_cast<uint64_t>(type) << (64 - resourceTypeBits))
                + (hashId & (std::numeric_limits<uint64_t>::max() >> resourceTypeBits));
    }

    ResourceId::ResourceId(ResourceType type, const StringData& ns)
        : _fullHash(fullHash(type, stringDataHashFunction(ns))) {
#ifdef _DEBUG
        _nsCopy = ns.toString();
#endif
    }

    ResourceId::ResourceId(ResourceType type, const string& ns)
        : _fullHash(fullHash(type, stringDataHashFunction(ns))) {
#ifdef _DEBUG
        _nsCopy = ns;
#endif
    }

    ResourceId::ResourceId(ResourceType type, uint64_t hashId)
        : _fullHash(fullHash(type, hashId)) { }

    string ResourceId::toString() const {
        StringBuilder ss;
        ss << "{" << _fullHash << ": " << resourceTypeName(getType())
           << ", " << getHashId();

#ifdef _DEBUG
        ss << ", " << _nsCopy;
#endif

        ss << "}";

        return ss.str();
    }


    //
    // LockRequest
    //

    void LockRequest::initNew(Locker* locker, LockGrantNotification* notify) {
        this->locker = locker;
        this->notify = notify;

        enqueueAtFront = false;
        compatibleFirst = false;
        recursiveCount = 0;

        lock = NULL;
        prev = NULL;
        next = NULL;
        status = STATUS_NEW;
        mode = MODE_NONE;
        convertMode = MODE_NONE;
    }


    //
    // Helper calls
    //

    const char* modeName(LockMode mode) {
        return LockModeNames[mode];
    }

    const char* legacyModeName(LockMode mode) {
        return LegacyLockModeNames[mode];
    }

    bool isModeCovered(LockMode mode, LockMode coveringMode) {
        return (LockConflictsTable[coveringMode] | LockConflictsTable[mode]) ==
                                                        LockConflictsTable[coveringMode];
    }

    const char* resourceTypeName(ResourceType resourceType) {
        return ResourceTypeNames[resourceType];
    }

} // namespace mongo
