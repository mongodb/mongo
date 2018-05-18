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

#include <climits>  // For UINT_MAX
#include <vector>

#include "mongo/db/concurrency/lock_manager.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/thread.h"

namespace mongo {

/**
 * Interface for acquiring locks. One of those objects will have to be instantiated for each
 * request (transaction).
 *
 * Lock/unlock methods must always be called from a single thread.
 */
class Locker {
    MONGO_DISALLOW_COPYING(Locker);

    friend class UninterruptibleLockGuard;

public:
    virtual ~Locker() {}

    /**
     * Returns true if this is an instance of LockerNoop. Because LockerNoop doesn't implement many
     * methods, some users may need to check this first to find out what is safe to call. LockerNoop
     * is only used in unittests and for a brief period at startup, so you can assume you hold the
     * equivalent of a MODE_X lock when using it.
     *
     * TODO get rid of this once we kill LockerNoop.
     */
    virtual bool isNoop() const {
        return false;
    }

    /**
     * Require global lock attempts to obtain tickets from 'reading' (for MODE_S and MODE_IS),
     * and from 'writing' (for MODE_IX), which must have static lifetimes. There is no throttling
     * for MODE_X, as there can only ever be a single locker using this mode. The throttling is
     * intended to defend against arge drops in throughput under high load due to too much
     * concurrency.
     */
    static void setGlobalThrottling(class TicketHolder* reading, class TicketHolder* writing);

    /**
     * State for reporting the number of active and queued reader and writer clients.
     */
    enum ClientState { kInactive, kActiveReader, kActiveWriter, kQueuedReader, kQueuedWriter };

    /**
     * Return whether client is holding any locks (active), or is queued on any locks or waiting
     * for a ticket (throttled).
     */
    virtual ClientState getClientState() const = 0;

    virtual LockerId getId() const = 0;

    /**
     * Get a platform-specific thread identifier of the thread which owns the this locker for
     * tracing purposes.
     */
    virtual stdx::thread::id getThreadId() const = 0;

    /**
     * Updates any cached thread id values to represent the current thread.
     */
    virtual void updateThreadIdToCurrentThread() = 0;

    /**
     * Clears any cached thread id values.
     */
    virtual void unsetThreadId() = 0;

    /**
     * Indicate that shared locks should participate in two-phase locking for this Locker instance.
     */
    virtual void setSharedLocksShouldTwoPhaseLock(bool sharedLocksShouldTwoPhaseLock) = 0;

    /**
     * This is useful to ensure that potential deadlocks do not occur.
     *
     * Overrides provided timeouts in lock requests with 'maxTimeout' if the provided timeout
     * is greater. Basically, no lock acquisition will take longer than 'maxTimeout'.
     *
     * If an UninterruptibleLockGuard is set during a lock request, the max timeout override will
     * be ignored.
     *
     * Future lock requests may throw LockTimeout errors if a lock request provides a Date_t::max()
     * deadline and 'maxTimeout' is reached. Presumably these callers do not expect to handle lock
     * acquisition failure, so this is done to ensure the caller does not proceed as if the lock
     * were successfully acquired.
     */
    virtual void setMaxLockTimeout(Milliseconds maxTimeout) = 0;

    /**
     * Returns whether this Locker has a maximum lock timeout set.
     */
    virtual bool hasMaxLockTimeout() = 0;

    /**
     * Clears the max lock timeout override set by setMaxLockTimeout() above.
     */
    virtual void unsetMaxLockTimeout() = 0;

    /**
     * This should be the first method invoked for a particular Locker object. It acquires the
     * Global lock in the specified mode and effectively indicates the mode of the operation.
     * This is what the lock modes on the global lock mean:
     *
     * IX - Regular write operation
     * IS - Regular read operation
     * S  - Stops all *write* activity. Used for administrative operations (repl, etc).
     * X  - Stops all activity. Used for administrative operations (repl state changes,
     *          shutdown, etc).
     *
     * This method can be called recursively, but each call to lockGlobal must be accompanied
     * by a call to unlockGlobal.
     *
     * @param opCtx OperationContext used to interrupt the lock waiting, if provided.
     * @param mode Mode in which the global lock should be acquired. Also indicates the intent
     *              of the operation.
     *
     * @return LOCK_OK, if the global lock (and the flush lock, for the MMAP V1 engine) were
     *          acquired within the specified time bound. Otherwise, the respective failure
     *          code and neither lock will be acquired.
     */
    virtual LockResult lockGlobal(OperationContext* opCtx, LockMode mode) = 0;
    virtual LockResult lockGlobal(LockMode mode) = 0;

    /**
     * Requests the global lock to be acquired in the specified mode.
     *
     * See the comments for lockBegin/Complete for more information on the semantics.
     * The deadline indicates the absolute time point when this lock acquisition will time out, if
     * not yet granted. The lockGlobalBegin
     * method has a deadline for use with the TicketHolder, if there is one.
     */
    virtual LockResult lockGlobalBegin(OperationContext* opCtx, LockMode mode, Date_t deadline) = 0;
    virtual LockResult lockGlobalBegin(LockMode mode, Date_t deadline) = 0;

    /**
     * Calling lockGlobalComplete without an OperationContext does not allow the lock acquisition
     * to be interrupted.
     */
    virtual LockResult lockGlobalComplete(OperationContext* opCtx, Date_t deadline) = 0;
    virtual LockResult lockGlobalComplete(Date_t deadline) = 0;

    /**
     * This method is used only in the MMAP V1 storage engine, otherwise it is a no-op. See the
     * comments in the implementation for more details on how MMAP V1 journaling works.
     */
    virtual void lockMMAPV1Flush() = 0;

    /**
     * Decrements the reference count on the global lock.  If the reference count on the
     * global lock hits zero, the transaction is over, and unlockGlobal unlocks all other locks
     * except for RESOURCE_MUTEX locks.
     *
     * @return true if this is the last endTransaction call (i.e., the global lock was
     *          released); false if there are still references on the global lock. This value
     *          should not be relied on and is only used for assertion purposes.
     *
     * @return false if the global lock is still held.
     */
    virtual bool unlockGlobal() = 0;

    /**
     * This is only necessary for the MMAP V1 engine and in particular, the fsyncLock command
     * which needs to first acquire the global lock in X-mode for truncating the journal and
     * then downgrade to S before it blocks.
     *
     * The downgrade is necessary in order to be nice and not block readers while under
     * fsyncLock.
     */
    virtual void downgradeGlobalXtoSForMMAPV1() = 0;

    /**
     * beginWriteUnitOfWork/endWriteUnitOfWork are called at the start and end of WriteUnitOfWorks.
     * They can be used to implement two-phase locking.
     */
    virtual void beginWriteUnitOfWork() = 0;
    virtual void endWriteUnitOfWork() = 0;

    virtual bool inAWriteUnitOfWork() const = 0;

    /**
     * Acquires lock on the specified resource in the specified mode and returns the outcome
     * of the operation. See the details for LockResult for more information on what the
     * different results mean.
     *
     * Each successful acquisition of a lock on a given resource increments the reference count
     * of the lock. Therefore, each call, which returns LOCK_OK must be matched with a
     * corresponding call to unlock.
     *
     * If setLockTimeoutMillis has been called, then a lock request with a Date_t::max() deadline
     * may throw a LockTimeout error. See setMaxLockTimeout() above for details.
     *
     * @param opCtx If provided, will be used to interrupt a LOCK_WAITING state.
     * @param resId Id of the resource to be locked.
     * @param mode Mode in which the resource should be locked. Lock upgrades are allowed.
     * @param deadline How long to wait for the lock to be granted, before
     *              returning LOCK_TIMEOUT. This parameter defaults to an infinite deadline.
     *              If Milliseconds(0) is passed, the request will return immediately, if
     *              the request could not be granted right away.
     * @param checkDeadlock Whether to enable deadlock detection for this acquisition. This
     *              parameter is put in place until we can handle deadlocks at all places,
     *              which acquire locks.
     *
     * @return All LockResults except for LOCK_WAITING, because it blocks.
     */
    virtual LockResult lock(OperationContext* opCtx,
                            ResourceId resId,
                            LockMode mode,
                            Date_t deadline = Date_t::max(),
                            bool checkDeadlock = false) = 0;

    /**
     * Calling lock without an OperationContext does not allow LOCK_WAITING states to be
     * interrupted.
     */
    virtual LockResult lock(ResourceId resId,
                            LockMode mode,
                            Date_t deadline = Date_t::max(),
                            bool checkDeadlock = false) = 0;

    /**
     * Downgrades the specified resource's lock mode without changing the reference count.
     */
    virtual void downgrade(ResourceId resId, LockMode newMode) = 0;

    /**
     * Releases a lock previously acquired through a lock call. It is an error to try to
     * release lock which has not been previously acquired (invariant violation).
     *
     * @return true if the lock was actually released; false if only the reference count was
     *              decremented, but the lock is still held.
     */
    virtual bool unlock(ResourceId resId) = 0;

    /**
     * Retrieves the mode in which a lock is held or checks whether the lock held for a
     * particular resource covers the specified mode.
     *
     * For example isLockHeldForMode will return true for MODE_S, if MODE_X is already held,
     * because MODE_X covers MODE_S.
     */
    virtual LockMode getLockMode(ResourceId resId) const = 0;
    virtual bool isLockHeldForMode(ResourceId resId, LockMode mode) const = 0;

    // These are shortcut methods for the above calls. They however check that the entire
    // hierarchy is properly locked and because of this they are very expensive to call.
    // Do not use them in performance critical code paths.
    virtual bool isDbLockedForMode(StringData dbName, LockMode mode) const = 0;
    virtual bool isCollectionLockedForMode(StringData ns, LockMode mode) const = 0;

    /**
     * Returns the resource that this locker is waiting/blocked on (if any). If the locker is
     * not waiting for a resource the returned value will be invalid (isValid() == false).
     */
    virtual ResourceId getWaitingResource() const = 0;

    /**
     * Describes a single lock acquisition for reporting/serialization purposes.
     */
    struct OneLock {
        // What lock resource is held?
        ResourceId resourceId;

        // In what mode is it held?
        LockMode mode;

        // Reporting/serialization order is by resourceId, which is the canonical locking order
        bool operator<(const OneLock& rhs) const {
            return resourceId < rhs.resourceId;
        }
    };

    /**
     * Returns information and locking statistics for this instance of the locker. Used to
     * support the db.currentOp view. This structure is not thread-safe and ideally should
     * be used only for obtaining the necessary information and then discarded instead of
     * reused.
     */
    struct LockerInfo {
        // List of high-level locks held by this locker, sorted by ResourceId
        std::vector<OneLock> locks;

        // If isValid(), then what lock this particular locker is sleeping on
        ResourceId waitingResource;

        // Lock timing statistics
        SingleThreadedLockStats stats;
    };

    virtual void getLockerInfo(LockerInfo* lockerInfo) const = 0;

    /**
     * Returns boost::none if this is an instance of LockerNoop, or a populated LockerInfo
     * otherwise.
     */
    virtual boost::optional<LockerInfo> getLockerInfo() const = 0;

    /**
     * LockSnapshot captures the state of all resources that are locked, what modes they're
     * locked in, and how many times they've been locked in that mode.
     */
    struct LockSnapshot {
        // The global lock is handled differently from all other locks.
        LockMode globalMode;

        // The non-global non-flush locks held, sorted by granularity.  That is, locks[i] is
        // coarser or as coarse as locks[i + 1].
        std::vector<OneLock> locks;
    };

    /**
     * Retrieves all locks held by this transaction, other than RESOURCE_MUTEX locks, and what mode
     * they're held in.
     * Stores these locks in 'stateOut', destroying any previous state.  Unlocks all locks
     * held by this transaction.  This functionality is used for yielding, which is
     * voluntary/cooperative lock release and reacquisition in order to allow for interleaving
     * of otherwise conflicting long-running operations.
     *
     * This functionality is also used for releasing locks on databases and collections
     * when cursors are dormant and waiting for a getMore request.
     *
     * Returns true if locks are released.  It is expected that restoreLockerImpl will be called
     * in the future.
     *
     * Returns false if locks are not released.  restoreLockState(...) does not need to be
     * called in this case.
     */
    virtual bool saveLockStateAndUnlock(LockSnapshot* stateOut) = 0;

    /**
     * Re-locks all locks whose state was stored in 'stateToRestore'.
     * @param opCtx An operation context that enables the restoration to be interrupted.
     */
    virtual void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore) = 0;
    virtual void restoreLockState(const LockSnapshot& stateToRestore) = 0;

    /**
     * Releases the ticket associated with the Locker. This allows locks to be held without
     * contributing to reader/writer throttling.
     */
    virtual void releaseTicket() = 0;

    /**
     * Reacquires a ticket for the Locker. This must only be called after releaseTicket(). It
     * restores the ticket under its previous LockMode.
     * An OperationContext is required to interrupt the ticket acquisition to prevent deadlocks.
     * A dead lock is possible when a ticket is reacquired while holding a lock.
     */
    virtual void reacquireTicket(OperationContext* opCtx) = 0;

    //
    // These methods are legacy from LockerImpl and will eventually go away or be converted to
    // calls into the Locker methods
    //

    virtual void dump() const = 0;

    virtual bool isW() const = 0;
    virtual bool isR() const = 0;

    virtual bool isLocked() const = 0;
    virtual bool isWriteLocked() const = 0;
    virtual bool isReadLocked() const = 0;
    virtual bool isGlobalLockedRecursively() = 0;

    /**
     * Pending means we are currently trying to get a lock (could be the parallel batch writer
     * lock).
     */
    virtual bool hasLockPending() const = 0;

    /**
     * If set to false, this opts out of conflicting with replication's use of the
     * ParallelBatchWriterMode lock. Code that opts-out must be ok with seeing an inconsistent view
     * of data because within a batch, secondaries apply operations in a different order than on the
     * primary. User operations should *never* opt out.
     */
    void setShouldConflictWithSecondaryBatchApplication(bool newValue) {
        _shouldConflictWithSecondaryBatchApplication = newValue;
    }
    bool shouldConflictWithSecondaryBatchApplication() const {
        return _shouldConflictWithSecondaryBatchApplication;
    }

    /**
     * If set to false, this opts out of the ticket mechanism. This should be used sparingly
     * for special purpose threads, such as FTDC.
     */
    void setShouldAcquireTicket(bool newValue) {
        invariant(!isLocked());
        _shouldAcquireTicket = newValue;
    }
    bool shouldAcquireTicket() const {
        return _shouldAcquireTicket;
    }
    /**
     * This function is for unit testing only.
     */
    unsigned numResourcesToUnlockAtEndUnitOfWorkForTest() const {
        return _numResourcesToUnlockAtEndUnitOfWork;
    }


protected:
    Locker() {}

    /**
     * The number of callers that are guarding from lock interruptions.
     * When 0, all lock acquisitions are interruptible. When positive, no lock acquisitions
     * are interruptible. This is only true for database and global locks. Collection locks are
     * never interruptible.
     */
    int _uninterruptibleLocksRequested = 0;
    /**
     * The number of LockRequests to unlock at the end of this WUOW. This is used for locks
     * participating in two-phase locking.
     */
    unsigned _numResourcesToUnlockAtEndUnitOfWork = 0;

private:
    bool _shouldConflictWithSecondaryBatchApplication = true;
    bool _shouldAcquireTicket = true;
};

/**
 * This class prevents lock acquisitions from being interrupted when it is in scope.
 * The default behavior of acquisitions depends on the type of lock that is being requested.
 * Use this in the unlikely case that waiting for a lock can't be interrupted.
 *
 * Lock acquisitions can still return LOCK_TIMEOUT, just not if the parent operation
 * context is killed first.
 *
 * It is possible that multiple callers are requesting uninterruptible behavior, so the guard
 * increments a counter on the Locker class to indicate how may guards are active.
 */
class UninterruptibleLockGuard {
public:
    /*
     * Accepts a Locker, and increments the _uninterruptibleLocksRequested. Decrements the
     * counter when destoyed.
     */
    explicit UninterruptibleLockGuard(Locker* locker) : _locker(locker) {
        invariant(_locker);
        invariant(_locker->_uninterruptibleLocksRequested >= 0);
        invariant(_locker->_uninterruptibleLocksRequested < std::numeric_limits<int>::max());
        _locker->_uninterruptibleLocksRequested += 1;
    }

    ~UninterruptibleLockGuard() {
        invariant(_locker->_uninterruptibleLocksRequested > 0);
        _locker->_uninterruptibleLocksRequested -= 1;
    }

private:
    Locker* const _locker;
};

/**
 * RAII-style class to opt out of replication's use of ParallelBatchWriterMode.
 */
class ShouldNotConflictWithSecondaryBatchApplicationBlock {
    MONGO_DISALLOW_COPYING(ShouldNotConflictWithSecondaryBatchApplicationBlock);

public:
    explicit ShouldNotConflictWithSecondaryBatchApplicationBlock(Locker* lockState)
        : _lockState(lockState),
          _originalShouldConflict(_lockState->shouldConflictWithSecondaryBatchApplication()) {
        _lockState->setShouldConflictWithSecondaryBatchApplication(false);
    }

    ~ShouldNotConflictWithSecondaryBatchApplicationBlock() {
        _lockState->setShouldConflictWithSecondaryBatchApplication(_originalShouldConflict);
    }

private:
    Locker* const _lockState;
    const bool _originalShouldConflict;
};

}  // namespace mongo
