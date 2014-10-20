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
         * Map of conflicts. 'LockConflictsTable[newMode] & existingMode != 0' means that a new
         * request with the given 'newMode' conflicts with an existing request with mode
         * 'existingMode'.
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

        /**
         * Maps the resource id to a human-readable string.
         */
        static const char* ResourceTypeNames[] = {
            "Invalid",
            "Global",
            "MMAPV1Flush",
            "Database",
            "Collection",
            "Document",
        };

        inline const char* resourceTypeName(ResourceType resourceType) {
            return ResourceTypeNames[resourceType];
        }
    }


    /**
     * There is one of these objects per each resource which has a lock on it.
     *
     * Not thread-safe and should only be accessed under the LockManager's bucket lock.
     */
    struct LockHead {

        LockHead(const ResourceId& resId);
        ~LockHead();

        // Used by the changeGrantedModeCount/changeConflictModeCount methods to indicate whether
        // the particular mode is coming or going away.
        enum ChangeModeCountAction {
            Increment = 1, Decrement = -1
        };

        // Methods to maintain the granted queue
        void changeGrantedModeCount(LockMode mode, ChangeModeCountAction action);
        void addToGrantedQueue(LockRequest* request);
        void removeFromGrantedQueue(LockRequest* request);

        // Methods to maintain the conflict queue
        void changeConflictModeCount(LockMode mode, ChangeModeCountAction count);
        void addToConflictQueue(LockRequest* request);
        void removeFromConflictQueue(LockRequest* request);


        // Id of the resource which this lock protects
        const ResourceId resourceId;


        //
        // Granted queue
        //

        // The head of the doubly-linked list of granted or converting requests
        LockRequest* grantedQueue;

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
        // with the set of granted modes. The reason to have both begin and end pointers is to make
        // the FIFO scheduling easier (queue at begin and take from the end).
        LockRequest* conflictQueueBegin;
        LockRequest* conflictQueueEnd;

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
    };


    //
    // LockManager
    //

    LockManager::LockManager() {
        //  Have more buckets than CPUs to reduce contention on lock and caches
        _numLockBuckets = 128;
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

            // The flush lock must be FIFO ordered or else we will starve the dur threads and not
            // block new writers. This means that if anyone is blocking on a mustFIFO resource we
            // must get in line, even if we don't conflict with the grantedModes and could take the
            // lock immediately.
            const bool mustFIFO = (resId.getType() == RESOURCE_MMAPV1_FLUSH);

            // New lock request
            if (conflicts(mode, lock->grantedModes)
                    || (mustFIFO && conflicts(mode, lock->conflictModes))) {
                request->status = LockRequest::STATUS_WAITING;
                request->mode = mode;
                request->convertMode = MODE_NONE;

                // Put it on the conflict queue. This is the place where various policies could be
                // applied for where in the wait queue does a request go.
                lock->addToConflictQueue(request);
                lock->changeConflictModeCount(mode, LockHead::Increment);

                return LOCK_WAITING;
            }
            else {  // No conflict, new request
                request->status = LockRequest::STATUS_GRANTED;
                request->mode = mode;
                request->convertMode = MODE_NONE;

                lock->addToGrantedQueue(request);
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
            // Because the check does not look into the conflict modes bitmap, it will grant L to
            // T1 in S mode, instead of block, which would otherwise cause deadlock.
            if (conflicts(mode, grantedModesWithoutCurrentRequest)) {
                request->status = LockRequest::STATUS_CONVERTING;
                request->convertMode = mode;

                lock->conversionsCount++;
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

        if (request->status == LockRequest::STATUS_WAITING) {
            // This cancels a pending lock request
            invariant(request->recursiveCount == 0);

            lock->removeFromConflictQueue(request);
            lock->changeConflictModeCount(request->mode, LockHead::Decrement);
        }
        else if (request->status == LockRequest::STATUS_CONVERTING) {
            // This cancels a pending convert request
            invariant(request->recursiveCount > 0);

            // Lock only goes from GRANTED to CONVERTING, so cancelling the conversion request
            // brings it back to the previous granted mode.
            request->status = LockRequest::STATUS_GRANTED;

            lock->conversionsCount--;
            lock->changeGrantedModeCount(request->convertMode, LockHead::Decrement);

            request->convertMode = MODE_NONE;

            _onLockModeChanged(lock, lock->grantedCounts[request->convertMode] == 0);
        }
        else if (request->status == LockRequest::STATUS_GRANTED) {
            // This releases a currently held lock and is the most common path, so it should be
            // as efficient as possible.
            invariant(request->recursiveCount == 0);

            // Remove from the granted list
            lock->removeFromGrantedQueue(request);
            lock->changeGrantedModeCount(request->mode, LockHead::Decrement);

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
        scoped_spinlock scopedLock(bucket->mutex);

        invariant(lock->grantedQueue != NULL);
        invariant(lock->grantedModes != 0);

        lock->changeGrantedModeCount(newMode, LockHead::Increment);
        lock->changeGrantedModeCount(request->mode, LockHead::Decrement);
        request->mode = newMode;

        _onLockModeChanged(lock, true);
    }

    void LockManager::cleanupUnusedLocks() {
        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];
            scoped_spinlock scopedLock(bucket->mutex);

            LockHeadMap::iterator it = bucket->data.begin();
            while (it != bucket->data.end()) {
                LockHead* lock = it->second;
                if (lock->grantedModes == 0) {
                    invariant(lock->grantedModes == 0);
                    invariant(lock->grantedQueue == NULL);
                    invariant(lock->conflictModes == 0);
                    invariant(lock->conflictQueueBegin == NULL);
                    invariant(lock->conflictQueueEnd == NULL);
                    invariant(lock->conversionsCount == 0);

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
        for (LockRequest* iter = lock->grantedQueue;
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
                    lock->changeGrantedModeCount(iter->mode, LockHead::Decrement);
                    iter->status = LockRequest::STATUS_GRANTED;
                    iter->mode = iter->convertMode;
                    iter->convertMode = MODE_NONE;

                    iter->notify->notify(lock->resourceId, LOCK_OK);
                }
            }
        }

        // Grant any conflicting requests, which might now be unblocked
        LockRequest* iterNext = NULL;

        for (LockRequest* iter = lock->conflictQueueBegin;
             (iter != NULL) && checkConflictQueue;
             iter = iterNext) {

            invariant(iter->status == LockRequest::STATUS_WAITING);

            // Store the actual next pointer, because we muck with the iter below and move it to
            // the granted queue.
            iterNext = iter->next;

            if (conflicts(iter->mode, lock->grantedModes)) continue;

            iter->status = LockRequest::STATUS_GRANTED;

            lock->removeFromConflictQueue(iter);
            lock->addToGrantedQueue(iter);

            lock->changeGrantedModeCount(iter->mode, LockHead::Increment);
            lock->changeConflictModeCount(iter->mode, LockHead::Decrement);

            iter->notify->notify(lock->resourceId, LOCK_OK);
        }

        // This is a convenient place to check that the state of the two request queues is in sync
        // with the bitmask on the modes.
        invariant((lock->grantedModes == 0) ^ (lock->grantedQueue != NULL));
        invariant((lock->conflictModes == 0) ^ (lock->conflictQueueBegin != NULL));
    }

    LockManager::LockBucket* LockManager::_getBucket(const ResourceId& resId) {
        return &_lockBuckets[resId % _numLockBuckets];
    }

    void LockManager::dump() const {
        log() << "Dumping LockManager @ " << static_cast<const void*>(this) << '\n';

        for (unsigned i = 0; i < _numLockBuckets; i++) {
            LockBucket* bucket = &_lockBuckets[i];
            scoped_spinlock scopedLock(bucket->mutex);

            _dumpBucket(bucket);
        }
    }

    void LockManager::_dumpBucket(const LockBucket* bucket) const {
        StringBuilder sb;

        LockHeadMap::const_iterator it = bucket->data.begin();
        while (it != bucket->data.end()) {
            const LockHead* lock = it->second;
            sb << "Lock @ " << lock << ": " << lock->resourceId.toString() << '\n';

            sb << "GRANTED:\n";
            for (const LockRequest* iter = lock->grantedQueue; iter != NULL; iter = iter->next) {
                sb << '\t'
                    << "LockRequest " << iter->locker->getId() << " @ " << iter->locker << ": "
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
                    << "LockRequest " << iter->locker->getId() << " @ " << iter->locker << ": "
                    << "Mode = " << modeName(iter->mode) << "; "
                    << "ConvertMode = " << modeName(iter->convertMode) << "; "
                    << '\n';
            }

            sb << '\n';

            it++;
        }

        log() << sb.str();
    }


    //
    // ResourceId
    //

    static const StringData::Hasher stringDataHashFunction = StringData::Hasher();

    ResourceId::ResourceId(ResourceType type, const StringData& ns) {
        _type = type;
        _hashId = stringDataHashFunction(ns) % 0x1fffffffffffffffULL;

#ifdef _DEBUG
        _nsCopy = ns.toString();
#endif
    }

    ResourceId::ResourceId(ResourceType type, const std::string& ns) {
        _type = type;
        _hashId = stringDataHashFunction(ns) % 0x1fffffffffffffffULL;

#ifdef _DEBUG
        _nsCopy = ns;
#endif
    }

    ResourceId::ResourceId(ResourceType type, uint64_t hashId) {
        _type = type;
        _hashId = hashId;
    }

    string ResourceId::toString() const {
        StringBuilder ss;
        ss << "{" << _fullHash << ": " << resourceTypeName(static_cast<ResourceType>(_type))
           << ", " << _hashId;

#ifdef _DEBUG
        ss << ", " << _nsCopy;
#endif

        ss << "}";

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
          conflictModes(0),
          conversionsCount(0) {

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

    void LockHead::changeConflictModeCount(LockMode mode, ChangeModeCountAction action) {
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

    void LockHead::addToGrantedQueue(LockRequest* request) {
        invariant(request->next == NULL);
        invariant(request->prev == NULL);

        request->next = grantedQueue;

        if (grantedQueue != NULL) {
            grantedQueue->prev = request;
        }

        grantedQueue = request;
    }

    void LockHead::removeFromGrantedQueue(LockRequest* request) {
        if (request->prev != NULL) {
            request->prev->next = request->next;
        }
        else {
            grantedQueue = request->next;
        }

        if (request->next != NULL) {
            request->next->prev = request->prev;
        }

        request->prev = NULL;
        request->next = NULL;
    }

    void LockHead::addToConflictQueue(LockRequest* request) {
        invariant(request->next == NULL);
        invariant(request->prev == NULL);
        if (conflictQueueBegin == NULL) {
            invariant(conflictQueueEnd == NULL);

            request->prev = NULL;
            request->next = NULL;

            conflictQueueBegin = request;
            conflictQueueEnd = request;
        }
        else {
            invariant(conflictQueueEnd != NULL);

            // durThread always jumps to the front of the queue
            const bool queueAtFront = (request->lock->resourceId.getType() == RESOURCE_MMAPV1_FLUSH)
                                   && (request->mode == MODE_X || request->mode == MODE_S);
            if (queueAtFront) {
                request->next = conflictQueueBegin;
                request->prev = NULL;

                conflictQueueBegin->prev = request;
                conflictQueueBegin = request;
            }
            else {
                request->prev = conflictQueueEnd;
                request->next = NULL;

                conflictQueueEnd->next = request;
                conflictQueueEnd = request;
            }

        }
    }

    void LockHead::removeFromConflictQueue(LockRequest* request) {
        if (request->prev != NULL) {
            request->prev->next = request->next;
        }
        else {
            conflictQueueBegin = request->next;
        }

        if (request->next != NULL) {
            request->next->prev = request->prev;
        }
        else {
            conflictQueueEnd = request->prev;
        }

        request->prev = NULL;
        request->next = NULL;
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

} // namespace mongo
