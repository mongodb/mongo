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

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/config.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_request_list.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

/**
 * Entry point for the lock manager scheduling functionality. Don't use it directly, but
 * instead go through the Locker interface.
 */
class LockManager {
    MONGO_DISALLOW_COPYING(LockManager);

public:
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
      *                 If the return value is LOCK_WAITING, the notification method will be
      *                 called at some point into the future, when the lock either becomes
      *                 granted or a deadlock is discovered. If unlock is called before the
      *                 lock becomes granted, the notification will not be invoked.
      *
      *                 If the return value is LOCK_WAITING, the notification object *must*
      *                 live at least until the notfy method has been invoked or unlock has
      *                 been called for the resource it was assigned to. Failure to do so will
      *                 cause the lock manager to call into an invalid memory location.
      * @param mode Mode in which the resource should be locked. Lock upgrades are allowed.
      *
      * @return See comments for LockResult.
      */
    LockResult lock(ResourceId resId, LockRequest* request, LockMode mode);
    LockResult convert(ResourceId resId, LockRequest* request, LockMode newMode);

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
     * Downgrades the mode in which an already granted request is held, without changing the
     * reference count of the lock request. This call never blocks, will always succeed and may
     * potentially allow other blocked lock requests to proceed.
     *
     * @param request Request, already in granted mode through a previous call to lock.
     * @param newMode Mode, which is less-restrictive than the mode in which the request is
     *                  already held. I.e., the conflict set of newMode must be a sub-set of
     *                  the conflict set of the request's current mode.
     */
    void downgrade(LockRequest* request, LockMode newMode);

    /**
     * Iterates through all buckets and deletes all locks, which have no requests on them. This
     * call is kind of expensive and should only be used for reducing the memory footprint of
     * the lock manager.
     */
    void cleanupUnusedLocks();

    /**
     * Dumps the contents of all locks to the log.
     */
    void dump() const;

    /**
     * Dumps the contents of all locks into a BSON object
     * to be used in lockInfo command in the shell.
     */
    void getLockInfoBSON(const std::map<LockerId, BSONObj>& lockToClientMap,
                         BSONObjBuilder* result);

private:
    // The deadlock detector needs to access the buckets and locks directly
    friend class DeadlockDetector;

    // The lockheads need access to the partitions
    friend struct LockHead;

    // These types describe the locks hash table

    struct LockBucket {
        SimpleMutex mutex;
        typedef unordered_map<ResourceId, LockHead*> Map;
        Map data;
        LockHead* findOrInsert(ResourceId resId);
    };

    // Each locker maps to a partition that is used for resources acquired in intent modes
    // modes and potentially other modes that don't conflict with themselves. This avoids
    // contention on the regular LockHead in the lock manager.
    struct Partition {
        PartitionedLockHead* find(ResourceId resId);
        PartitionedLockHead* findOrInsert(ResourceId resId);
        typedef unordered_map<ResourceId, PartitionedLockHead*> Map;
        SimpleMutex mutex;
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
     * Prints the contents of a bucket to the log.
     */
    void _dumpBucket(const LockBucket* bucket) const;

    /**
     * Dump the contents of a bucket to the BSON.
     */
    void _dumpBucketToBSON(const std::map<LockerId, BSONObj>& lockToClientMap,
                           const LockBucket* bucket,
                           BSONObjBuilder* result);

    /**
     * Build the BSON object containing the lock info for a particular
     * bucket. The lockToClientMap is used to map the lockerId to
     * more useful client information.
     */
    void _buildBucketBSON(const LockRequest* iter,
                          const std::map<LockerId, BSONObj>& lockToClientMap,
                          const LockBucket* bucket,
                          BSONArrayBuilder* locks);

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

    static const unsigned _numLockBuckets;
    LockBucket* _lockBuckets;

    static const unsigned _numPartitions;
    Partition* _partitions;
};


/**
 * Iteratively builds the wait-for graph, starting from a given blocked Locker and stops either
 * when all reachable nodes have been checked or if a cycle is detected. This class is
 * thread-safe. Because locks may come and go in parallel with deadlock detection, it may
 * report false positives, but if there is a stable cycle it will be discovered.
 *
 * Implemented as a separate class in order to facilitate diagnostics and also unit-testing for
 * cases where locks come and go in parallel with deadlock detection.
 */
class DeadlockDetector {
public:
    /**
     * Initializes the wait-for graph builder with the LM to operate on and a locker object
     * from which to start the search. Deadlock will only be reported if there is a wait cycle
     * in which the initial locker participates.
     */
    DeadlockDetector(const LockManager& lockMgr, const Locker* initialLocker);

    DeadlockDetector& check() {
        while (next()) {
        }

        return *this;
    }

    /**
     * Processes the next wait for node and queues up its set of owners to the unprocessed
     * queue.
     *
     * @return true if there are more unprocessed nodes and no cycle has been discovered yet;
     *          false if either all reachable nodes have been processed or
     */
    bool next();

    /**
     * Checks whether a cycle exists in the wait-for graph, which has been built so far. It's
     * only useful to call this after next() has returned false.
     */
    bool hasCycle() const;

    /**
     * Produces a string containing the wait-for graph that has been built so far.
     */
    std::string toString() const;

private:
    // An entry in the owners list below means that some locker L is blocked on some resource
    // resId, which is currently held by the given set of owners. The reason to store it in
    // such form is in order to avoid storing pointers to the lockers or to have to look them
    // up by id, both of which require some form of synchronization other than locking the
    // bucket for the resource. Instead, given the resId, we can lock the bucket for the lock
    // and find the respective LockRequests and continue our scan forward.
    typedef std::vector<LockerId> ConflictingOwnersList;

    struct Edges {
        explicit Edges(ResourceId resId) : resId(resId) {}

        // Resource id indicating the lock node
        ResourceId resId;

        // List of lock owners/pariticipants with which the initial locker conflicts for
        // obtaining the lock
        ConflictingOwnersList owners;
    };

    typedef std::map<LockerId, Edges> WaitForGraph;
    typedef WaitForGraph::value_type WaitForGraphPair;


    // We don't want to hold locks between iteration cycles, so just store the resourceId and
    // the lockerId so we can directly find them from the lock manager.
    struct UnprocessedNode {
        UnprocessedNode(LockerId lockerId, ResourceId resId) : lockerId(lockerId), resId(resId) {}

        LockerId lockerId;
        ResourceId resId;
    };

    typedef std::deque<UnprocessedNode> UnprocessedNodesQueue;


    void _processNextNode(const UnprocessedNode& node);


    // Not owned. Lifetime must be longer than that of the graph builder.
    const LockManager& _lockMgr;
    const LockerId _initialLockerId;

    UnprocessedNodesQueue _queue;
    WaitForGraph _graph;

    bool _foundCycle;
};

}  // namespace mongo
