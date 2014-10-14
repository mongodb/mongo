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

#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>

#include "mongo/db/concurrency/resource_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/cstdint.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/timer.h"


/**
 * Event-driven LockManager implementation.
 */

namespace mongo {

    class Locker;
    struct LockHead;

    /**
     * Lock modes.
     *
     * Compatibility Matrix
     *                                          Granted mode
     *   ---------------.--------------------------------------------------------.
     *   Requested Mode | MODE_NONE  MODE_IS   MODE_IX  MODE_S   MODE_X  |
     *     MODE_IS      |      +        +         +        +        -    |
     *     MODE_IX      |      +        +         +        -        -    |
     *     MODE_S       |      +        +         -        +        -    |
     *     MODE_X       |      +        -         -        -        -    |
     */
    enum LockMode {
        MODE_NONE       = 0,
        MODE_IS         = 1,
        MODE_IX         = 2,
        MODE_S          = 3,
        MODE_X          = 4,
    };

    enum {
        // Counts the entries in the LockMode enumeration. Used for array size allocations, etc.
        LockModesCount   = 5,
    };


    /**
     * Return values for the locking functions of the lock manager.
     */
    enum LockResult {

        /**
         * The lock request was granted and is now on the granted list for the specified resource.
         */
        LOCK_OK,

        /**
         * The lock request was not granted because of conflict. If this value is returned, the
         * request was placed on the conflict queue of the specified resource and a call to the
         * LockGrantNotification::notify callback should be expected with the resource whose lock
         * was requested.
         */
        LOCK_WAITING,

        /**
         * The lock request waited, but timed out before it could be granted. This value is never
         * returned by the LockManager methods here, but by the Locker class, which offers
         * capability to block while waiting for locks.
         */
        LOCK_TIMEOUT,

        /**
         * The lock request was not granted because it would result in a deadlock. No changes to
         * the state of the Locker would be made if this value is returned (i.e., it will not be
         * killed due to deadlock). It is up to the caller to decide how to recover from this
         * return value - could be either release some locks and try again, or just bail with an
         * error and have some upper code handle it.
         */
        LOCK_DEADLOCK,

        /**
         * This is used as an initialiser value. Should never be returned.
         */
        LOCK_INVALID
    };


    /**
     * Interface on which granted lock requests will be notified. See the contract for the notify
     * method for more information and also the LockManager::lock call.
     *
     * The default implementation of this method would simply block on an event until notify has
     * been invoked (see CondVarLockGrantNotification).
     *
     * Test implementations could just count the number of notifications and their outcome so that
     * they can validate locks are granted as desired and drive the test execution.
     */
    class LockGrantNotification {
    public:
        virtual ~LockGrantNotification() {}

        /**
         * This method is invoked at most once for each lock request and indicates the outcome of
         * the lock acquisition for the specified resource id.
         *
         * Cases where it won't be called are if a lock acquisition (be it in waiting or converting
         * state) is cancelled through a call to unlock.
         *
         * IMPORTANT: This callback runs under a spinlock for the lock manager, so the work done
         *            inside must be kept to a minimum and no locks or operations which may block
         *            should be run. Also, no methods which call back into the lock manager should
         *            be invoked from within this methods (LockManager is not reentrant).
         *
         * @resId ResourceId for which a lock operation was previously called.
         * @result Outcome of the lock operation.
         */
        virtual void notify(const ResourceId& resId, LockResult result) = 0;
    };


    /**
     * There is one of those entries per each request for a lock. They hang on a linked list off
     * the LockHead and also are in a map for each Locker. This structure is not thread-safe.
     *
     * The lifetime of a LockRequest is managed by the Locker class.
     */
    struct LockRequest {

        enum Status {
            STATUS_NEW,
            STATUS_GRANTED,
            STATUS_WAITING,
            STATUS_CONVERTING,
        };

        /**
         * Used for initialization of a LockRequest, which might have been retrieved from cache.
         */
        void initNew(Locker* locker, LockGrantNotification* notify);

        //
        // These fields are maintained by the Locker class
        //

        // This is the Locker, which created this LockRequest. Pointer is not owned, just
        // referenced. Must outlive the LockRequest.
        Locker* locker;

        // Not owned, just referenced. If a request is in the WAITING or CONVERTING state, must
        // live at least until LockManager::unlock is cancelled or the notification has been
        // invoked.
        LockGrantNotification* notify;


        //
        // These fields are owned and maintained by the LockManager class
        //

        // Pointer to the lock to which this request belongs, or null if this request has not yet
        // been assigned to a lock. The LockHead should be alive as long as there are LockRequests
        // on it, so it is safe to have this pointer hanging around.
        LockHead* lock;

        // The reason intrusive linked list is used instead of the std::list class is to allow
        // for entries to be removed from the middle of the list in O(1) time, if they are known
        // instead of having to search for them and we cannot persist iterators, because the list
        // can be modified while an iterator is held.
        LockRequest* prev;
        LockRequest* next;

        // Current status of this request.
        Status status;

        // If not granted, the mode which has been requested for this lock. If granted, the mode
        // in which it is currently granted.
        LockMode mode;

        // This value is different from MODE_NONE only if a conversion is requested for a lock and
        // that conversion cannot be immediately granted.
        LockMode convertMode;

        // How many times has LockManager::lock been called for this request. Locks are released
        // when their recursive count drops to zero.
        unsigned recursiveCount;
    };


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
        LockResult lock(const ResourceId& resId, LockRequest* request, LockMode mode);

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


        //
        // Test-only methods
        //

        void setNoCheckForLeakedLocksTestOnly(bool newValue);

    private:

        typedef unordered_map<ResourceId, LockHead*> LockHeadMap;
        typedef LockHeadMap::value_type LockHeadPair;

        struct LockBucket {
            SpinLock mutex;
            LockHeadMap data;
        };

        /**
         * Retrieves the bucket in which the particular resource must reside. There is no need to
         * hold a lock when calling this function.
         */
        LockBucket* _getBucket(const ResourceId& resId);

        /**
         * Prints the contents of a bucket to the log.
         */
        void _dumpBucket(const LockBucket* bucket) const;

        /**
         * Should be invoked when the state of a lock changes in a way, which could potentially
         * allow other blocked requests to proceed.
         *
         * MUST be called under the lock bucket's spin lock.
         *
         * @param lock Lock whose grant state should be recalculated.
         * @param checkConflictQueue Whether to go through the conflict queue. This is an
         *          optimisation in that we only need to check the conflict queue if one of the
         *          granted modes, which was conflicting before became zero.
         */
        void _onLockModeChanged(LockHead* lock, bool checkConflictQueue);

        unsigned _numLockBuckets;
        LockBucket* _lockBuckets;
    };

} // namespace mongo
