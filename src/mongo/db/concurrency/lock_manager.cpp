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

#include "mongo/db/concurrency/lock_manager.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::string;

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

// Mask of modes
const uint64_t intentModes = (1 << MODE_IS) | (1 << MODE_IX);

// Ensure we do not add new modes without updating the conflicts table
static_assert((sizeof(LockConflictsTable) / sizeof(LockConflictsTable[0])) == LockModesCount,
              "(sizeof(LockConflictsTable) / sizeof(LockConflictsTable[0])) == LockModesCount");


/**
 * Maps the mode id to a string.
 */
static const char* LockModeNames[] = {"NONE", "IS", "IX", "S", "X"};

static const char* LegacyLockModeNames[] = {"", "r", "w", "R", "W"};

// Ensure we do not add new modes without updating the names array
static_assert((sizeof(LockModeNames) / sizeof(LockModeNames[0])) == LockModesCount,
              "(sizeof(LockModeNames) / sizeof(LockModeNames[0])) == LockModesCount");
static_assert((sizeof(LegacyLockModeNames) / sizeof(LegacyLockModeNames[0])) == LockModesCount,
              "(sizeof(LegacyLockModeNames) / sizeof(LegacyLockModeNames[0])) == LockModesCount");

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
    "Invalid", "Global", "MMAPV1Journal", "Database", "Collection", "Metadata", "Mutex"};

// Ensure we do not add new types without updating the names array
static_assert((sizeof(ResourceTypeNames) / sizeof(ResourceTypeNames[0])) == ResourceTypesCount,
              "(sizeof(ResourceTypeNames) / sizeof(ResourceTypeNames[0])) == ResourceTypesCount");


/**
 * Maps the LockRequest status to a human-readable string.
 */
static const char* LockRequestStatusNames[] = {
    "new", "granted", "waiting", "converting",
};

// Ensure we do not add new status types without updating the names array
static_assert((sizeof(LockRequestStatusNames) / sizeof(LockRequestStatusNames[0])) ==
                  LockRequest::StatusCount,
              "(sizeof(LockRequestStatusNames) / sizeof(LockRequestStatusNames[0])) == "
              "LockRequest::StatusCount");

}  // namespace


/**
 * There is one of these objects for each resource that has a lock request. Empty objects
 * (i.e. LockHead with no requests) are allowed to exist on the lock manager's hash table.
 *
 * The memory and lifetime is controlled entirely by the LockManager class.
 *
 * Not thread-safe and should only be accessed under the LockManager's bucket lock.
 * Must be locked before locking a partition, not after.
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
     * True iff there may be partitions with granted requests for this
     * resource.
     */
    bool partitioned() const {
        return !partitions.empty();
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

    /**
     * Finish creation of request and put it on the lockhead's conflict or granted queues.
     * Returns LOCK_WAITING for conflict case and LOCK_OK otherwise.
     */
    LockResult newRequest(LockRequest* request, LockMode mode) {
        request->mode = mode;
        request->lock = this;
        request->partitionedLock = NULL;
        if (!partitioned()) {
            request->recursiveCount = 1;
        }
        // request->partitioned cannot be set to false, as this might be a migration, in
        // which case access to that field is not protected. The 'partitioned' member instead
        // indicates if a request was initially partitioned.

        // New lock request. Queue after all granted modes and after any already requested
        // conflicting modes.
        if (conflicts(mode, grantedModes) ||
            (!compatibleFirstCount && conflicts(mode, conflictModes))) {
            request->status = LockRequest::STATUS_WAITING;

            // Put it on the conflict queue. Conflicts are granted front to back.
            if (request->enqueueAtFront) {
                conflictList.push_front(request);
            } else {
                conflictList.push_back(request);
            }

            incConflictModeCount(mode);

            return LOCK_WAITING;
        }

        // No conflict, new request
        request->status = LockRequest::STATUS_GRANTED;

        grantedList.push_back(request);
        incGrantedModeCount(mode);

        if (request->compatibleFirst) {
            compatibleFirstCount++;
        }

        return LOCK_OK;
    }

    /**
     * Lock each partitioned LockHead in turn, and move any (granted) intent mode requests for
     * lock->resourceId to lock, which must itself already be locked.
     */
    void migratePartitionedLockHeads();

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

    // References partitions that may have PartitionedLockHeads for this LockHead.
    // Non-empty implies the lock has no conflicts and only has intent modes as grantedModes.
    // TODO: Remove this vector and make LockHead a POD
    std::vector<LockManager::Partition*> partitions;

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

/**
 * The PartitionedLockHead allows optimizing the case where requests overwhelmingly use
 * the intent lock modes MODE_IS and MODE_IX, which are compatible with each other.
 * Having to use a single LockHead causes contention where none would be needed.
 * So, each Locker is associated with a specific partition containing a mapping
 * of resourceId to PartitionedLockHead.
 *
 * As long as all lock requests for a resource have an intent mode, as opposed to a conflicting
 * mode, its LockHead may reference ParitionedLockHeads. A partitioned LockHead will not have
 * any conflicts. The total set of granted requests (with intent mode) is the union of
 * its grantedList and all grantedLists in PartitionedLockHeads.
 *
 * The existence of a PartitionedLockHead for a resource implies that its LockHead is
 * partitioned. If a conflicting request is made on a LockHead, all requests from
 * PartitionedLockHeads are migrated to that LockHead and the LockHead no longer partitioned.
 *
 * Not thread-safe, must be accessed under its partition lock.
 * May not lock a LockManager bucket while holding a partition lock.
 */
struct PartitionedLockHead {
    void initNew(ResourceId resId) {
        grantedList.reset();
    }

    void newRequest(LockRequest* request, LockMode mode) {
        request->lock = NULL;
        request->partitionedLock = this;
        request->recursiveCount = 1;
        request->status = LockRequest::STATUS_GRANTED;
        request->partitioned = true;
        request->mode = mode;

        grantedList.push_back(request);
    }

    //
    // Granted queue
    //

    // Doubly-linked list of requests, which have been granted. Newly granted requests go to
    // the end of the queue. The PartitionedLockHead never contains anything but granted
    // requests with intent modes.
    LockRequestList grantedList;
};

void LockHead::migratePartitionedLockHeads() {
    invariant(partitioned());
    // There can't be non-intent modes or conflicts when the lock is partitioned
    invariant(!(grantedModes & ~intentModes) && !conflictModes);

    // Migration time: lock each partition in turn and transfer its requests, if any
    while (partitioned()) {
        LockManager::Partition* partition = partitions.back();
        stdx::lock_guard<SimpleMutex> scopedLock(partition->mutex);

        LockManager::Partition::Map::iterator it = partition->data.find(resourceId);
        if (it != partition->data.end()) {
            PartitionedLockHead* partitionedLock = it->second;

            while (!partitionedLock->grantedList.empty()) {
                LockRequest* request = partitionedLock->grantedList._front;
                partitionedLock->grantedList.remove(request);
                // Ordering is important here, as the next/prev fields are shared.
                // Note that newRequest() will preserve the recursiveCount in this case
                LockResult res = newRequest(request, request->mode);
                invariant(res == LOCK_OK);  // Lock must still be granted
            }
            partition->data.erase(it);
            delete partitionedLock;
        }
        // Don't pop-back to early as otherwise the lock will be considered not partioned in
        // newRequest().
        partitions.pop_back();
    }
}

//
// LockManager
//

// Have more buckets than CPUs to reduce contention on lock and caches
const unsigned LockManager::_numLockBuckets(128);

// Balance scalability of intent locks against potential added cost of conflicting locks.
// The exact value doesn't appear very important, but should be power of two
const unsigned LockManager::_numPartitions = 32;

LockManager::LockManager() {
    _lockBuckets = new LockBucket[_numLockBuckets];
    _partitions = new Partition[_numPartitions];
}

LockManager::~LockManager() {
    cleanupUnusedLocks();

    for (unsigned i = 0; i < _numLockBuckets; i++) {
        // TODO: dump more information about the non-empty bucket to see what locks were leaked
        invariant(_lockBuckets[i].data.empty());
    }

    delete[] _lockBuckets;
    delete[] _partitions;
}

LockResult LockManager::lock(ResourceId resId, LockRequest* request, LockMode mode) {
    // Sanity check that requests are not being reused without proper cleanup
    invariant(request->status == LockRequest::STATUS_NEW);

    request->partitioned = (mode == MODE_IX || mode == MODE_IS);

    // For intent modes, try the PartitionedLockHead
    if (request->partitioned) {
        Partition* partition = _getPartition(request);
        stdx::lock_guard<SimpleMutex> scopedLock(partition->mutex);

        // Fast path for intent locks
        PartitionedLockHead* partitionedLock = partition->find(resId);

        if (partitionedLock) {
            partitionedLock->newRequest(request, mode);
            return LOCK_OK;
        }
        // Unsuccessful: there was no PartitionedLockHead yet, so use regular LockHead.
        // Must not hold any locks. It is OK for requests with intent modes to be on
        // both a PartitionedLockHead and a regular LockHead, so the race here is benign.
    }

    // Use regular LockHead, maybe start partitioning
    LockBucket* bucket = _getBucket(resId);
    stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);

    LockHead* lock = bucket->findOrInsert(resId);

    // Start a partitioned lock if possible
    if (request->partitioned && !(lock->grantedModes & (~intentModes)) && !lock->conflictModes) {
        Partition* partition = _getPartition(request);
        stdx::lock_guard<SimpleMutex> scopedLock(partition->mutex);
        PartitionedLockHead* partitionedLock = partition->findOrInsert(resId);
        invariant(partitionedLock);
        lock->partitions.push_back(partition);
        partitionedLock->newRequest(request, mode);
        return LOCK_OK;
    }

    // For the first lock with a non-intent mode, migrate requests from partitioned lock heads
    if (lock->partitioned()) {
        lock->migratePartitionedLockHeads();
    }

    request->partitioned = false;
    return lock->newRequest(request, mode);
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
    stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);

    LockBucket::Map::iterator it = bucket->data.find(resId);
    invariant(it != bucket->data.end());

    LockHead* const lock = it->second;

    if (lock->partitioned()) {
        lock->migratePartitionedLockHeads();
    }

    // Construct granted mask without our current mode, so that it is not counted as
    // conflicting
    uint32_t grantedModesWithoutCurrentRequest = 0;

    // We start the counting at 1 below, because LockModesCount also includes MODE_NONE
    // at position 0, which can never be acquired/granted.
    for (uint32_t i = 1; i < LockModesCount; i++) {
        const uint32_t currentRequestHolds = (request->mode == static_cast<LockMode>(i) ? 1 : 0);

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
        invariant(request->recursiveCount > 1);
        request->convertMode = newMode;

        lock->conversionsCount++;
        lock->incGrantedModeCount(request->convertMode);

        return LOCK_WAITING;
    } else {  // No conflict, existing request
        lock->incGrantedModeCount(newMode);
        lock->decGrantedModeCount(request->mode);
        request->mode = newMode;

        return LOCK_OK;
    }
}

bool LockManager::unlock(LockRequest* request) {
    // Fast path for decrementing multiple references of the same lock. It is safe to do this
    // without locking, because 1) all calls for the same lock request must be done on the same
    // thread and 2) if there are lock requests hanging of a given LockHead, then this lock
    // will never disappear.
    invariant(request->recursiveCount > 0);
    request->recursiveCount--;
    if ((request->status == LockRequest::STATUS_GRANTED) && (request->recursiveCount > 0)) {
        return false;
    }

    if (request->partitioned) {
        // Unlocking a lock that was acquired as partitioned. The lock request may since have
        // moved to the lock head, but there is no safe way to find out without synchronizing
        // thorough the partition mutex. Migrations are expected to be rare.
        invariant(request->status == LockRequest::STATUS_GRANTED ||
                  request->status == LockRequest::STATUS_CONVERTING);
        Partition* partition = _getPartition(request);
        stdx::lock_guard<SimpleMutex> scopedLock(partition->mutex);
        //  Fast path: still partitioned.
        if (request->partitionedLock) {
            request->partitionedLock->grantedList.remove(request);
            return true;
        }

        // not partitioned anymore, fall through to regular case
    }
    invariant(request->lock);

    LockHead* lock = request->lock;
    LockBucket* bucket = _getBucket(lock->resourceId);
    stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);

    if (request->status == LockRequest::STATUS_GRANTED) {
        // This releases a currently held lock and is the most common path, so it should be
        // as efficient as possible. The fast path for decrementing multiple references did
        // already ensure request->recursiveCount == 0.

        // Remove from the granted list
        lock->grantedList.remove(request);
        lock->decGrantedModeCount(request->mode);

        if (request->compatibleFirst) {
            lock->compatibleFirstCount--;
        }

        _onLockModeChanged(lock, lock->grantedCounts[request->mode] == 0);
    } else if (request->status == LockRequest::STATUS_WAITING) {
        // This cancels a pending lock request
        invariant(request->recursiveCount == 0);

        lock->conflictList.remove(request);
        lock->decConflictModeCount(request->mode);

        _onLockModeChanged(lock, true);
    } else if (request->status == LockRequest::STATUS_CONVERTING) {
        // This cancels a pending convert request
        invariant(request->recursiveCount > 0);
        invariant(lock->conversionsCount > 0);

        // Lock only goes from GRANTED to CONVERTING, so cancelling the conversion request
        // brings it back to the previous granted mode.
        request->status = LockRequest::STATUS_GRANTED;

        lock->conversionsCount--;
        lock->decGrantedModeCount(request->convertMode);

        request->convertMode = MODE_NONE;

        _onLockModeChanged(lock, lock->grantedCounts[request->convertMode] == 0);
    } else {
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
    invariant((LockConflictsTable[request->mode] | LockConflictsTable[newMode]) ==
              LockConflictsTable[request->mode]);

    LockHead* lock = request->lock;

    LockBucket* bucket = _getBucket(lock->resourceId);
    stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);

    lock->incGrantedModeCount(newMode);
    lock->decGrantedModeCount(request->mode);
    request->mode = newMode;

    _onLockModeChanged(lock, true);
}

void LockManager::cleanupUnusedLocks() {
    for (unsigned i = 0; i < _numLockBuckets; i++) {
        LockBucket* bucket = &_lockBuckets[i];
        stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);
        _cleanupUnusedLocksInBucket(bucket);
    }
}

void LockManager::_cleanupUnusedLocksInBucket(LockBucket* bucket) {
    LockBucket::Map::iterator it = bucket->data.begin();
    size_t deletedLockHeads = 0;
    while (it != bucket->data.end()) {
        LockHead* lock = it->second;
        if (lock->partitioned()) {
            lock->migratePartitionedLockHeads();
        }
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
            deletedLockHeads++;
            delete lock;
        } else {
            it++;
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

    // Grant any conflicting requests, which might now be unblocked. Note that the loop below
    // slightly violates fairness in that it will grant *all* compatible requests on the line even
    // though there might be conflicting ones interspersed between them. For example, assume that an
    // X lock was just freed and the conflict queue looks like this:
    //
    //      IS -> IS -> X -> X -> S -> IS
    //
    // In strict FIFO, we should grant the first two IS modes and then stop when we reach the first
    // X mode (the third request on the queue). However, the loop below would actually grant all IS
    // + S modes and once they all drain it will grant X. The reason for this behaviour is
    // increasing system throughput in the scenario where mutually compatible requests are
    // interspersed with conflicting ones. For example, this would be a worst-case scenario for
    // strict FIFO, because it would make the execution sequential:
    //
    //      S -> X -> S -> X -> S -> X

    LockRequest* iterNext = NULL;

    bool newlyCompatibleFirst = false;  // Set on enabling compatibleFirst mode.
    for (LockRequest* iter = lock->conflictList._front; (iter != NULL) && checkConflictQueue;
         iter = iterNext) {
        invariant(iter->status == LockRequest::STATUS_WAITING);

        // Store the actual next pointer, because we muck with the iter below and move it to
        // the granted queue.
        iterNext = iter->next;

        if (conflicts(iter->mode, lock->grantedModes)) {
            // If iter doesn't have a previous pointer, this means that it is at the front of the
            // queue. If we continue scanning the queue beyond this point, we will starve it by
            // granting more and more requests. However, if we newly transition to compatibleFirst
            // mode, grant any waiting compatible requests.
            if (!iter->prev && !newlyCompatibleFirst) {
                break;
            }
            continue;
        }

        iter->status = LockRequest::STATUS_GRANTED;

        lock->conflictList.remove(iter);
        lock->grantedList.push_back(iter);

        lock->incGrantedModeCount(iter->mode);
        lock->decConflictModeCount(iter->mode);

        if (iter->compatibleFirst) {
            newlyCompatibleFirst |= (lock->compatibleFirstCount++ == 0);
        }

        iter->notify->notify(lock->resourceId, LOCK_OK);

        // Small optimization - nothing is compatible with MODE_X, so no point in looking
        // further in the conflict queue.
        if (iter->mode == MODE_X) {
            break;
        }
    }

    // This is a convenient place to check that the state of the two request queues is in sync
    // with the bitmask on the modes.
    dassert((lock->grantedModes == 0) ^ (lock->grantedList._front != NULL));
    dassert((lock->conflictModes == 0) ^ (lock->conflictList._front != NULL));
}

LockManager::LockBucket* LockManager::_getBucket(ResourceId resId) const {
    return &_lockBuckets[resId % _numLockBuckets];
}

LockManager::Partition* LockManager::_getPartition(LockRequest* request) const {
    return &_partitions[request->locker->getId() % _numPartitions];
}

void LockManager::dump() const {
    log() << "Dumping LockManager @ " << static_cast<const void*>(this) << '\n';

    for (unsigned i = 0; i < _numLockBuckets; i++) {
        LockBucket* bucket = &_lockBuckets[i];
        stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);

        if (!bucket->data.empty()) {
            _dumpBucket(bucket);
        }
    }
}

void LockManager::_dumpBucketToBSON(const std::map<LockerId, BSONObj>& lockToClientMap,
                                    const LockBucket* bucket,
                                    BSONObjBuilder* result) {
    for (auto& bucketEntry : bucket->data) {
        const LockHead* lock = bucketEntry.second;

        if (lock->grantedList.empty()) {
            // If there are no granted requests, this lock is empty, so no need to print it
            continue;
        }

        result->append("resourceId", lock->resourceId.toString());

        BSONArrayBuilder grantedLocks;
        for (const LockRequest* iter = lock->grantedList._front; iter != nullptr;
             iter = iter->next) {
            _buildBucketBSON(iter, lockToClientMap, bucket, &grantedLocks);
        }
        result->append("granted", grantedLocks.arr());

        BSONArrayBuilder pendingLocks;
        for (const LockRequest* iter = lock->conflictList._front; iter != nullptr;
             iter = iter->next) {
            _buildBucketBSON(iter, lockToClientMap, bucket, &pendingLocks);
        }
        result->append("pending", pendingLocks.arr());
    }
}

void LockManager::_buildBucketBSON(const LockRequest* iter,
                                   const std::map<LockerId, BSONObj>& lockToClientMap,
                                   const LockBucket* bucket,
                                   BSONArrayBuilder* locks) {
    BSONObjBuilder info;
    info.append("mode", modeName(iter->mode));
    info.append("convertMode", modeName(iter->convertMode));
    info.append("enqueueAtFront", iter->enqueueAtFront);
    info.append("compatibleFirst", iter->compatibleFirst);

    LockerId lockerId = iter->locker->getId();
    std::map<LockerId, BSONObj>::const_iterator it = lockToClientMap.find(lockerId);
    if (it != lockToClientMap.end()) {
        info.appendElements(it->second);
    }
    locks->append(info.obj());
}

void LockManager::getLockInfoBSON(const std::map<LockerId, BSONObj>& lockToClientMap,
                                  BSONObjBuilder* result) {
    BSONArrayBuilder lockInfo;
    for (unsigned i = 0; i < _numLockBuckets; i++) {
        LockBucket* bucket = &_lockBuckets[i];
        stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);

        _cleanupUnusedLocksInBucket(bucket);
        if (!bucket->data.empty()) {
            BSONObjBuilder b;
            _dumpBucketToBSON(lockToClientMap, bucket, &b);
            lockInfo.append(b.obj());
        }
    }
    result->append("lockInfo", lockInfo.arr());
}

void LockManager::_dumpBucket(const LockBucket* bucket) const {
    for (LockBucket::Map::const_iterator it = bucket->data.begin(); it != bucket->data.end();
         it++) {
        const LockHead* lock = it->second;

        if (lock->grantedList.empty()) {
            // If there are no granted requests, this lock is empty, so no need to print it
            continue;
        }

        StringBuilder sb;
        sb << "Lock @ " << lock << ": " << lock->resourceId.toString() << '\n';

        sb << "GRANTED:\n";
        for (const LockRequest* iter = lock->grantedList._front; iter != NULL; iter = iter->next) {
            sb << '\t' << "LockRequest " << iter->locker->getId() << " @ " << iter->locker << ": "
               << "Mode = " << modeName(iter->mode) << "; "
               << "ConvertMode = " << modeName(iter->convertMode) << "; "
               << "EnqueueAtFront = " << iter->enqueueAtFront << "; "
               << "CompatibleFirst = " << iter->compatibleFirst << "; " << '\n';
        }

        sb << "PENDING:\n";
        for (const LockRequest* iter = lock->conflictList._front; iter != NULL; iter = iter->next) {
            sb << '\t' << "LockRequest " << iter->locker->getId() << " @ " << iter->locker << ": "
               << "Mode = " << modeName(iter->mode) << "; "
               << "ConvertMode = " << modeName(iter->convertMode) << "; "
               << "EnqueueAtFront = " << iter->enqueueAtFront << "; "
               << "CompatibleFirst = " << iter->compatibleFirst << "; " << '\n';
        }

        sb << "-----------------------------------------------------------\n";
        log() << sb.str();
    }
}

PartitionedLockHead* LockManager::Partition::find(ResourceId resId) {
    Map::iterator it = data.find(resId);
    return it == data.end() ? NULL : it->second;
}

PartitionedLockHead* LockManager::Partition::findOrInsert(ResourceId resId) {
    PartitionedLockHead* lock;
    Map::iterator it = data.find(resId);
    if (it == data.end()) {
        lock = new PartitionedLockHead();
        lock->initNew(resId);

        data.insert(Map::value_type(resId, lock));
    } else {
        lock = it->second;
    }
    return lock;
}

LockHead* LockManager::LockBucket::findOrInsert(ResourceId resId) {
    LockHead* lock;
    Map::iterator it = data.find(resId);
    if (it == data.end()) {
        lock = new LockHead();
        lock->initNew(resId);

        data.insert(Map::value_type(resId, lock));
    } else {
        lock = it->second;
    }
    return lock;
}

//
// DeadlockDetector
//

DeadlockDetector::DeadlockDetector(const LockManager& lockMgr, const Locker* initialLocker)
    : _lockMgr(lockMgr), _initialLockerId(initialLocker->getId()), _foundCycle(false) {
    const ResourceId resId = initialLocker->getWaitingResource();

    // If there is no resource waiting there is nothing to do
    if (resId.isValid()) {
        _queue.push_front(UnprocessedNode(_initialLockerId, resId));
    }
}

bool DeadlockDetector::next() {
    if (_queue.empty())
        return false;

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
        for (ConflictingOwnersList::const_iterator itW = owners.begin(); itW != owners.end();
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
    stdx::lock_guard<SimpleMutex> scopedLock(bucket->mutex);

    LockManager::LockBucket::Map::const_iterator iter = bucket->data.find(node.resId);
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
         (request->status == LockRequest::STATUS_WAITING) && (it != NULL);
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
    return (static_cast<uint64_t>(type) << (64 - resourceTypeBits)) +
        (hashId & (std::numeric_limits<uint64_t>::max() >> resourceTypeBits));
}

ResourceId::ResourceId(ResourceType type, StringData ns)
    : _fullHash(fullHash(type, stringDataHashFunction(ns))) {
#ifdef MONGO_CONFIG_DEBUG_BUILD
    _nsCopy = ns.toString();
#endif
}

ResourceId::ResourceId(ResourceType type, const string& ns)
    : _fullHash(fullHash(type, stringDataHashFunction(ns))) {
#ifdef MONGO_CONFIG_DEBUG_BUILD
    _nsCopy = ns;
#endif
}

ResourceId::ResourceId(ResourceType type, uint64_t hashId) : _fullHash(fullHash(type, hashId)) {}

string ResourceId::toString() const {
    StringBuilder ss;
    ss << "{" << _fullHash << ": " << resourceTypeName(getType()) << ", " << getHashId();

#ifdef MONGO_CONFIG_DEBUG_BUILD
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
    partitioned = false;
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

const char* lockRequestStatusName(LockRequest::Status status) {
    return LockRequestStatusNames[status];
}

}  // namespace mongo
