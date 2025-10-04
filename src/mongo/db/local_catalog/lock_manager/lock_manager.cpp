/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/local_catalog/lock_manager/lock_manager.h"

#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/local_catalog/lock_manager/lock_request_list.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <fmt/format.h>
#include <fmt/ostream.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

template <typename T>
std::string formatHex(T&& x) {
    return fmt::format("{:#x}", x);
}

std::string formatPtr(const void* x) {
    return formatHex(reinterpret_cast<uintptr_t>(x));
}

std::string formatThreadId(stdx::thread::id x) {
    return fmt::format("{}", fmt::streamed(x));
}


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
MONGO_STATIC_ASSERT((sizeof(LockConflictsTable) / sizeof(LockConflictsTable[0])) == LockModesCount);


/**
 * Maps the mode id to a string.
 */
static const char* LockModeNames[] = {"NONE", "IS", "IX", "S", "X"};

static const char* LegacyLockModeNames[] = {"", "r", "w", "R", "W"};

// Ensure we do not add new modes without updating the names array
MONGO_STATIC_ASSERT((sizeof(LockModeNames) / sizeof(LockModeNames[0])) == LockModesCount);
MONGO_STATIC_ASSERT((sizeof(LegacyLockModeNames) / sizeof(LegacyLockModeNames[0])) ==
                    LockModesCount);

// Helper functions for the lock modes
bool conflicts(LockMode newMode, uint32_t existingModesMask) {
    return (LockConflictsTable[newMode] & existingModesMask) != 0;
}

uint32_t modeMask(LockMode mode) {
    return 1 << mode;
}

/**
 * Maps the LockRequest status to a human-readable string.
 */
static const char* LockRequestStatusNames[] = {
    "new",
    "granted",
    "waiting",
};

// Ensure we do not add new status types without updating the names array
MONGO_STATIC_ASSERT((sizeof(LockRequestStatusNames) / sizeof(LockRequestStatusNames[0])) ==
                    LockRequest::StatusCount);

auto getLockManager = ServiceContext::declareDecoration<LockManager>();

}  // namespace

/**
 * There is one of these objects for each resource that has a lock request. Empty objects (i.e.
 * LockHead with no requests) are allowed to exist on the lock manager's hash table.
 *
 * The memory and lifetime is controlled entirely by the LockManager class.
 *
 * Not thread-safe and should only be accessed under the LockManager's bucket lock. Must be locked
 * before locking a partition, not after.
 */
struct LockHead {

    /**
     * Used for initialization of a LockHead, which might have been retrieved from cache and also in
     * order to keep the LockHead structure a POD.
     */
    void initNew(ResourceId resId) {
        resourceId = resId;

        grantedList.reset();
        memset(grantedCounts, 0, sizeof(grantedCounts));
        grantedModes = 0;

        conflictList.reset();
        memset(conflictCounts, 0, sizeof(conflictCounts));
        conflictModes = 0;

        compatibleFirstCount = 0;
    }

    /**
     * True iff there may be partitions with granted requests for this resource.
     */
    bool partitioned() const {
        return !partitions.empty();
    }

    /**
     * Locates the request corresponding to the particular locker or returns nullptr. Must be called
     * with the bucket holding this lock head locked.
     */
    LockRequest* findRequest(LockerId lockerId) const {
        // Check the granted queue first
        for (LockRequest* it = grantedList._front; it != nullptr; it = it->next) {
            if (it->locker->getId() == lockerId) {
                return it;
            }
        }

        // Check the conflict queue second
        for (LockRequest* it = conflictList._front; it != nullptr; it = it->next) {
            if (it->locker->getId() == lockerId) {
                return it;
            }
        }

        return nullptr;
    }

    /**
     * Finish creation of request and put it on the LockHead's conflict or granted queues. Returns
     * LOCK_WAITING for conflict case and LOCK_OK otherwise.
     */
    LockResult newRequest(LockRequest* request) {
        invariant(!request->partitionedLock);
        request->lock = this;

        // We cannot set request->partitioned to false, as this might be a migration, in which case
        // access to that field is not protected. The 'partitioned' member instead indicates if a
        // request was initially partitioned.

        // New lock request. Queue after all granted modes and after any already requested
        // conflicting modes
        if (conflicts(request->mode, grantedModes) ||
            (!compatibleFirstCount && conflicts(request->mode, conflictModes))) {
            request->status = LockRequest::STATUS_WAITING;

            // Put it on the conflict queue. Conflicts are granted front to back.
            if (request->enqueueAtFront) {
                conflictList.push_front(request);
            } else {
                conflictList.push_back(request);
            }

            incConflictModeCount(request->mode);

            return LOCK_WAITING;
        }

        // No conflict, new request
        request->status = LockRequest::STATUS_GRANTED;

        grantedList.push_back(request);
        incGrantedModeCount(request->mode);

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

    // Id of the resource which is protected by this lock. Initialized at construction time and does
    // not change.
    ResourceId resourceId;

    //
    // Granted queue
    //

    // Doubly-linked list of requests, which have been granted. Newly granted requests go to
    // the end of the queue.
    LockRequestList grantedList;

    // Counts the grants for each of the supported lock modes. These counts should exactly match the
    // aggregated modes on the granted list.
    uint32_t grantedCounts[LockModesCount];

    // Bit-mask of the granted modes on the granted queue. Maintained in lock-step with the
    // grantedCounts array.
    uint32_t grantedModes;

    //
    // Conflict queue
    //

    // Doubly-linked list of requests, which have not been granted yet because they conflict
    // with the set of granted modes. Requests are queued at the end of the queue and are
    // granted from the beginning forward, which gives these locks FIFO ordering. Exceptions to the
    // FIFO rule are strong lock requests for global resources, such as MODE_X for Global.
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
 * mode, its LockHead may reference PartitionedLockHeads. A partitioned LockHead will not have
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

    void newRequest(LockRequest* request) {
        invariant(request->partitioned);
        invariant(!request->lock);
        request->partitionedLock = this;
        request->status = LockRequest::STATUS_GRANTED;

        grantedList.push_back(request);
    }

    // Doubly-linked list of requests, which have been granted. Newly granted requests go to the end
    // of the queue. The PartitionedLockHead never contains anything but granted requests with
    // intent modes.
    LockRequestList grantedList;
};

void LockHead::migratePartitionedLockHeads() {
    invariant(partitioned());

    // There can't be non-intent modes or conflicts when the lock is partitioned
    invariant(!(grantedModes & ~intentModes) && !conflictModes);

    // Migration time: lock each partition in turn and transfer its requests, if any
    while (partitioned()) {
        LockManager::Partition* partition = partitions.back();
        stdx::lock_guard<stdx::mutex> scopedLock(partition->mutex);

        LockManager::Partition::Map::iterator it = partition->data.find(resourceId);
        if (it != partition->data.end()) {
            PartitionedLockHead* partitionedLock = it->second;

            while (!partitionedLock->grantedList.empty()) {
                LockRequest* request = partitionedLock->grantedList._front;
                partitionedLock->grantedList.remove(request);
                request->partitionedLock = nullptr;
                // Ordering is important here, as the next/prev fields are shared.
                // Note that newRequest() will preserve the recursiveCount in this case
                LockResult res = newRequest(request);
                invariant(res == LOCK_OK);  // Lock must still be granted
            }
            partition->data.erase(it);
            delete partitionedLock;
        }
        // Don't pop-back to early as otherwise the lock will be considered not partitioned in
        // newRequest().
        partitions.pop_back();
    }
}

//
// LockManager
//

// static
LockManager* LockManager::get(ServiceContext* service) {
    return &getLockManager(service);
}

// static
LockManager* LockManager::get(ServiceContext& service) {
    return &getLockManager(service);
}

LockManager::LockManager() {
    _lockBuckets = new LockBucket[_numLockBuckets];
    _partitions = new Partition[_numPartitions];
}

LockManager::~LockManager() {
    cleanupUnusedLocks();

    for (unsigned i = 0; i < _numLockBuckets; i++) {
        invariant(_lockBuckets[i].data.empty());
    }

    delete[] _lockBuckets;
    delete[] _partitions;
}

LockResult LockManager::lock(ResourceId resId, LockRequest* request, LockMode mode) {
    // Sanity check that requests are not being reused without proper cleanup
    invariant(request->recursiveCount == 1);

    request->partitioned = (mode == MODE_IX || mode == MODE_IS);
    request->mode = mode;

    // For intent modes, try the PartitionedLockHead
    if (request->partitioned) {
        Partition* partition = _getPartition(request);
        stdx::lock_guard<stdx::mutex> scopedLock(partition->mutex);
        invariant(request->status == LockRequest::STATUS_NEW);

        // Fast path for intent locks
        PartitionedLockHead* partitionedLock = partition->find(resId);

        if (partitionedLock) {
            partitionedLock->newRequest(request);
            return LOCK_OK;
        }
        // Unsuccessful: there was no PartitionedLockHead yet, so use regular LockHead.
        // Must not hold any locks. It is OK for requests with intent modes to be on
        // both a PartitionedLockHead and a regular LockHead, so the race here is benign.
    }

    // Use regular LockHead, maybe start partitioning
    LockBucket* bucket = _getBucket(resId);
    stdx::lock_guard<stdx::mutex> scopedLock(bucket->mutex);
    invariant(request->status == LockRequest::STATUS_NEW);

    LockHead* lock = bucket->findOrInsert(resId);

    // Start a partitioned lock if possible
    if (request->partitioned && !(lock->grantedModes & (~intentModes)) && !lock->conflictModes) {
        Partition* partition = _getPartition(request);
        stdx::lock_guard<stdx::mutex> scopedLock(partition->mutex);
        PartitionedLockHead* partitionedLock = partition->findOrInsert(resId);
        invariant(partitionedLock);
        lock->partitions.push_back(partition);
        partitionedLock->newRequest(request);
        return LOCK_OK;
    }

    // For the first lock with a non-intent mode, migrate requests from partitioned lock heads
    if (lock->partitioned()) {
        lock->migratePartitionedLockHeads();
    }

    request->partitioned = false;
    return lock->newRequest(request);
}

bool LockManager::unlock(LockRequest* request) {
    invariant(request->recursiveCount > 0);
    request->recursiveCount--;

    if (request->partitioned) {
        // Unlocking a lock that was acquired as partitioned. The lock request may since have
        // moved to the lock head, but there is no safe way to find out without synchronizing
        // thorough the partition mutex. Migrations are expected to be rare.
        Partition* partition = _getPartition(request);
        stdx::lock_guard<stdx::mutex> scopedLock(partition->mutex);
        invariant(request->status == LockRequest::STATUS_GRANTED);

        if (request->status == LockRequest::STATUS_GRANTED && request->recursiveCount > 0)
            return false;

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
    stdx::lock_guard<stdx::mutex> scopedLock(bucket->mutex);

    if (request->status == LockRequest::STATUS_GRANTED) {
        if (request->recursiveCount > 0)
            return false;

        // This releases a currently held lock and is the most common path, so it should be
        // as efficient as possible. The fast path for decrementing multiple references did
        // already ensure request->recursiveCount == 0.

        // Remove from the granted list
        lock->grantedList.remove(request);
        lock->decGrantedModeCount(request->mode);

        if (request->compatibleFirst) {
            invariant(lock->compatibleFirstCount > 0);
            lock->compatibleFirstCount--;
            invariant(lock->compatibleFirstCount == 0 || !lock->grantedList.empty());
        }

        _onLockModeChanged(lock, lock->grantedCounts[request->mode] == 0);
    } else if (request->status == LockRequest::STATUS_WAITING) {
        // This cancels a pending lock request
        invariant(request->recursiveCount == 0);

        lock->conflictList.remove(request);
        lock->decConflictModeCount(request->mode);

        _onLockModeChanged(lock, true);
    } else {
        // Invalid request status
        MONGO_UNREACHABLE_TASSERT(10083513);
    }

    return (request->recursiveCount == 0);
}

void LockManager::cleanupUnusedLocks() {
    for (unsigned i = 0; i < _numLockBuckets; i++) {
        LockBucket* bucket = &_lockBuckets[i];
        stdx::lock_guard<stdx::mutex> scopedLock(bucket->mutex);
        _cleanupUnusedLocksInBucket(bucket);
    }
}

void LockManager::_cleanupUnusedLocksInBucket(LockBucket* bucket) {
    LockBucket::Map::iterator it = bucket->data.begin();
    while (it != bucket->data.end()) {
        LockHead* lock = it->second;

        if (lock->partitioned()) {
            lock->migratePartitionedLockHeads();
        }

        if (lock->grantedModes == 0) {
            invariant(lock->grantedModes == 0);
            invariant(lock->grantedList._front == nullptr);
            invariant(lock->grantedList._back == nullptr);
            invariant(lock->conflictModes == 0);
            invariant(lock->conflictList._front == nullptr);
            invariant(lock->conflictList._back == nullptr);
            invariant(lock->compatibleFirstCount == 0);

            bucket->data.erase(it++);
            delete lock;
        } else {
            it++;
        }
    }
}

void LockManager::_onLockModeChanged(LockHead* lock, bool checkConflictQueue) {
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

    LockRequest* iterNext = nullptr;

    bool newlyCompatibleFirst = false;  // Set on enabling compatibleFirst mode.
    for (LockRequest* iter = lock->conflictList._front; (iter != nullptr) && checkConflictQueue;
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

        // Remove from the conflicts list
        lock->conflictList.remove(iter);
        lock->decConflictModeCount(iter->mode);

        // Add to the granted list
        lock->grantedList.push_back(iter);
        lock->incGrantedModeCount(iter->mode);

        if (iter->compatibleFirst) {
            newlyCompatibleFirst |= (lock->compatibleFirstCount++ == 0);
        }

        iter->notify->notify(lock->resourceId, LOCK_OK);

        // Small optimization - nothing is compatible with a newly granted MODE_X, so no point in
        // looking further in the conflict queue. Conflicting MODE_X requests are skipped above.
        if (iter->mode == MODE_X) {
            break;
        }
    }

    // This is a convenient place to check that the state of the two request queues is in sync
    // with the bitmask on the modes.
    invariant((lock->grantedModes == 0) ^ (lock->grantedList._front != nullptr));
    invariant((lock->conflictModes == 0) ^ (lock->conflictList._front != nullptr));
}

LockManager::LockBucket* LockManager::_getBucket(ResourceId resId) const {
    return &_lockBuckets[resId % _numLockBuckets];
}

LockManager::Partition* LockManager::_getPartition(LockRequest* request) const {
    return &_partitions[request->locker->getId() % _numPartitions];
}

bool LockManager::hasConflictingRequests(ResourceId resId, const LockRequest* request) const {
    stdx::lock_guard<stdx::mutex> lk(_getBucket(resId)->mutex);
    return request->lock ? !request->lock->conflictList.empty() : false;
}

std::vector<LogDebugInfo> LockManager::getLockInfoFromResourceHolders(ResourceId resId) {
    std::vector<LogDebugInfo> locksInfo;
    for (size_t i = 0; i < _numLockBuckets; ++i) {
        LockBucket& bucket = _lockBuckets[i];
        stdx::lock_guard<stdx::mutex> scopedLock{bucket.mutex};
        const auto it = bucket.data.find(resId);
        if (it == bucket.data.end()) {
            continue;
        }
        LockHead* lock = it->second;
        for (auto iter = lock->grantedList._front; iter != nullptr; iter = iter->next) {
            locksInfo.emplace_back(iter->mode, iter->locker->getDebugInfo());
        }
    }
    return locksInfo;
}

void LockManager::getLockInfoArray(const std::map<LockerId, BSONObj>& lockToClientMap,
                                   bool forLogging,
                                   LockManager* mutableThis,
                                   BSONArrayBuilder* locks) const {
    for (size_t i = 0; i < _numLockBuckets; ++i) {
        LockBucket& bucket = _lockBuckets[i];
        stdx::lock_guard<stdx::mutex> scopedLock(bucket.mutex);
        // LockInfo cleans the unused locks as it goes, but dump doesn't.
        if (mutableThis) {
            invariant(mutableThis == this);
            mutableThis->_cleanupUnusedLocksInBucket(&bucket);
        }
        for (auto&& kv : bucket.data) {
            const auto& lock = kv.second;
            if (lock->grantedList.empty())
                continue;
            auto o = BSONObjBuilder(locks->subobjStart());
            if (forLogging)
                o.append("lockAddr", formatPtr(lock));
            o.append("resourceId", lock->resourceId.toString());
            struct {
                StringData key;
                LockRequest* iter;
            } lists[] = {
                {"granted"_sd, lock->grantedList._front},
                {"pending"_sd, lock->conflictList._front},
            };
            for (auto [key, iter] : lists) {
                auto arr = BSONArrayBuilder(o.subarrayStart(key));
                for (; iter; iter = iter->next) {
                    auto req = BSONObjBuilder(arr.subobjStart());
                    if (forLogging) {
                        req.append("lockRequest", formatHex(iter->locker->getId()));
                        req.append("lockRequestAddr", formatPtr(iter->locker));
                        req.append("thread", formatThreadId(iter->locker->getThreadId()));
                    }
                    req.append("mode", modeName(iter->mode));
                    req.append("enqueueAtFront", iter->enqueueAtFront);
                    req.append("compatibleFirst", iter->compatibleFirst);
                    req.append("debugInfo", iter->locker->getDebugInfo());
                    if (auto it = lockToClientMap.find(iter->locker->getId());
                        it != lockToClientMap.end()) {
                        req.append("clientInfo", it->second);
                    }
                }
            }
        }
    }
}

//
// LockBucket
//

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
// Partition
//

PartitionedLockHead* LockManager::Partition::find(ResourceId resId) {
    Map::iterator it = data.find(resId);
    return it == data.end() ? nullptr : it->second;
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

const char* lockRequestStatusName(LockRequest::Status status) {
    return LockRequestStatusNames[status];
}

}  // namespace mongo
