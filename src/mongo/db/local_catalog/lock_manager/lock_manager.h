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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"

#include <map>
#include <vector>

namespace mongo {

/**
 * Entry point for the lock manager scheduling functionality. Don't use it directly, but
 * instead go through the Locker interface.
 */
class LockManager {
    LockManager(const LockManager&) = delete;
    LockManager& operator=(const LockManager&) = delete;

public:
    /**
     * Retrieves the lock manager instance attached to this ServiceContext.
     * The lock manager is now a decoration on the service context and this is the accessor that
     * most callers should prefer outside of startup, lock internals, and debugger scripts.
     */
    static LockManager* get(ServiceContext* service);
    static LockManager* get(ServiceContext& service);

    /**
     * Default constructors are meant for unit tests only. The lock manager should generally be
     * accessed as a decorator on the ServiceContext.
     */
    LockManager();
    ~LockManager();

    /**
     * Acquires lock on the specified resource in the specified mode and returns the outcome
     * of the operation. See the details for LockResult for more information on what the
     * different results mean.
     *
     * Locking the same resource twice increments the reference count of the lock so each call
     * to lock must be matched with a call to unlock with the same resource.
     *
     * @param resId Id of the resource to be locked.
     * @param request LockRequest structure on which the state of the request will be tracked.
     *                 This value cannot be NULL and the notify value must be set. If the
     *                 return value is not LOCK_WAITING, this pointer can be freed and will
     *                 not be used any more.
     *
     *                 If the return value is LOCK_WAITING, the notification method will be called
     *                 at some point into the future, when the lock becomes granted. If unlock is
     *                 called before the lock becomes granted, the notification will not be
     *                 invoked.
     *
     *                 If the return value is LOCK_WAITING, the notification object *must*
     *                 live at least until the notify method has been invoked or unlock has
     *                 been called for the resource it was assigned to. Failure to do so will
     *                 cause the lock manager to call into an invalid memory location.
     * @param mode Mode in which the resource should be locked. Lock upgrades are allowed.
     *
     * @return See comments for LockResult.
     */
    LockResult lock(ResourceId resId, LockRequest* request, LockMode mode);

    /**
     * Decrements the reference count of a previously locked request and if the reference count
     * becomes zero, removes the request and proceeds to granting any conflicts.
     *
     * This method always succeeds and never blocks.
     *
     * @param request A previously locked request. Calling unlock more times than lock was
     *                  called for the same LockRequest is an error.
     *
     * @return true if this is the last reference for the request; false otherwise
     */
    bool unlock(LockRequest* request);

    /**
     * Iterates through all buckets and deletes all locks, which have no requests on them. This
     * call is kind of expensive and should only be used for reducing the memory footprint of
     * the lock manager.
     */
    void cleanupUnusedLocks();

    /**
     * Returns whether there are any conflicting lock requests for the given resource and lock
     * request. Note that the returned value may be immediately stale.
     */
    bool hasConflictingRequests(ResourceId resId, const LockRequest* request) const;

    /**
     * The backend of `dump` and `getLockInfoBSON`.
     * If `mutableThis`, then we also clean the unused locks in the buckets while iterating.
     * @param `mutableThis` is a nonconst `this`, but it is null if caller is const.
     */
    void getLockInfoArray(const std::map<LockerId, BSONObj>& lockToClientMap,
                          bool forLogging,
                          LockManager* mutableThis,
                          BSONArrayBuilder* buckets) const;

    /**
     * Returns a vector of those locks granted for the given resource.
     */
    std::vector<LogDebugInfo> getLockInfoFromResourceHolders(ResourceId resId);

private:
    // The lockheads need access to the partitions
    friend struct LockHead;

    // These types describe the locks hash table

    struct LockBucket {
        LockHead* findOrInsert(ResourceId resId);

        stdx::mutex mutex;
        using Map = stdx::unordered_map<ResourceId, LockHead*>;
        Map data;
    };

    // Each locker maps to a partition that is used for resources acquired in intent modes
    // modes and potentially other modes that don't conflict with themselves. This avoids
    // contention on the regular LockHead in the lock manager.
    struct Partition {
        PartitionedLockHead* find(ResourceId resId);
        PartitionedLockHead* findOrInsert(ResourceId resId);

        stdx::mutex mutex;
        using Map = stdx::unordered_map<ResourceId, PartitionedLockHead*>;
        Map data;
    };

    /**
     * Retrieves the bucket in which the particular resource must reside. There is no need to
     * hold a lock when calling this function.
     */
    LockBucket* _getBucket(ResourceId resId) const;


    /**
     * Retrieves the Partition that a particular LockRequest should use for intent locking.
     */
    Partition* _getPartition(LockRequest* request) const;

    /**
     * Should be invoked when the state of a lock changes in a way, which could potentially
     * allow other blocked requests to proceed.
     *
     * MUST be called under the lock bucket's mutex.
     *
     * @param lock Lock whose grant state should be recalculated.
     * @param checkConflictQueue Whether to go through the conflict queue. This is an
     *          optimisation in that we only need to check the conflict queue if one of the
     *          granted modes, which was conflicting before became zero.
     */
    void _onLockModeChanged(LockHead* lock, bool checkConflictQueue);

    /**
     * Helper function to delete all locks that have no request on them on a single bucket.
     * Called by cleanupUnusedLocks()
     */
    void _cleanupUnusedLocksInBucket(LockBucket* bucket);

    // Have more buckets than CPUs to reduce contention on lock and caches
    static constexpr unsigned _numLockBuckets{128};
    LockBucket* _lockBuckets;

    // Balance scalability of intent locks against potential added cost of conflicting locks. The
    // exact value doesn't appear very important, but should be power of two.
    static constexpr unsigned _numPartitions{32};
    Partition* _partitions;
};

}  // namespace mongo
