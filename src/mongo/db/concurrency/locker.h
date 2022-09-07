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

#include <climits>  // For UINT_MAX
#include <vector>

#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/admission_context.h"

namespace mongo {

/**
 * Interface for acquiring locks. One of those objects will have to be instantiated for each
 * request (transaction).
 *
 * Lock/unlock methods must always be called from a single thread.
 */
class Locker {
    Locker(const Locker&) = delete;
    Locker& operator=(const Locker&) = delete;

    friend class UninterruptibleLockGuard;
    friend class InterruptibleLockGuard;

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
     * @param deadline indicates the absolute time point when this lock acquisition will time out,
     * if not yet granted. Deadline will be also used for TicketHolder, if there is one.
     *
     * It may throw an exception if it is interrupted. The ticket acquisition phase can also be
     * interrupted by killOp or time out, thus throwing an exception.
     */
    virtual void lockGlobal(OperationContext* opCtx,
                            LockMode mode,
                            Date_t deadline = Date_t::max()) = 0;

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
     * Requests the RSTL to be acquired in the requested mode (typically mode X) . This should only
     * be called inside ReplicationStateTransitionLockGuard.
     *
     * See the comments for _lockBegin/Complete for more information on the semantics.
     */
    virtual LockResult lockRSTLBegin(OperationContext* opCtx, LockMode mode) = 0;

    /**
     * Waits for the completion of acquiring the RSTL. This should only be called inside
     * ReplicationStateTransitionLockGuard.
     *
     * It may throw an exception if it is interrupted.
     */
    virtual void lockRSTLComplete(OperationContext* opCtx, LockMode mode, Date_t deadline) = 0;

    /**
     * Unlocks the RSTL when the transaction becomes prepared. This is used to bypass two-phase
     * locking and unlock the RSTL immediately, rather than at the end of the WUOW.
     *
     * @return true if the RSTL is unlocked; false if we fail to unlock the RSTL or if it was
     * already unlocked.
     */
    virtual bool unlockRSTLforPrepare() = 0;

    /**
     * beginWriteUnitOfWork/endWriteUnitOfWork are called at the start and end of WriteUnitOfWorks.
     * They can be used to implement two-phase locking. Each call to begin or restore should be
     * matched with an eventual call to end or release.
     *
     * endWriteUnitOfWork, if not called in a nested WUOW, will release all two-phase locking held
     * lock resources.
     */
    virtual void beginWriteUnitOfWork() = 0;
    virtual void endWriteUnitOfWork() = 0;

    virtual bool inAWriteUnitOfWork() const = 0;

    /**
     * Returns whether we have ever taken a global lock in X or IX mode in this operation.
     * Should only be called on the thread owning the locker.
     */
    virtual bool wasGlobalLockTakenForWrite() const = 0;

    /**
     * Returns whether we have ever taken a global lock in S, X, or IX mode in this operation.
     */
    virtual bool wasGlobalLockTakenInModeConflictingWithWrites() const = 0;

    /**
     * Returns whether we have ever taken a global lock in this operation.
     * Should only be called on the thread owning the locker.
     */
    virtual bool wasGlobalLockTaken() const = 0;

    /**
     * Sets the mode bit in _globalLockMode. Once a mode bit is set, we won't clear it. Also sets
     * _wasGlobalLockTakenInModeConflictingWithWrites to true if the mode is S, X, or IX.
     */
    virtual void setGlobalLockTakenInMode(LockMode mode) = 0;

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
     * may throw a LockTimeout exception. See setMaxLockTimeout() above for details.
     *
     * @param opCtx If provided, will be used to interrupt a LOCK_WAITING state.
     * @param resId Id of the resource to be locked.
     * @param mode Mode in which the resource should be locked. Lock upgrades are allowed.
     * @param deadline How long to wait for the lock to be granted.
     *                 This parameter defaults to an infinite deadline.
     *                 If Milliseconds(0) is passed, the function will return immediately if the
     *                 request could be granted right away, or throws a LockTimeout exception
     *                 otherwise.
     *
     * It may throw an exception if it is interrupted.
     */
    virtual void lock(OperationContext* opCtx,
                      ResourceId resId,
                      LockMode mode,
                      Date_t deadline = Date_t::max()) = 0;

    /**
     * Calling lock without an OperationContext does not allow LOCK_WAITING states to be
     * interrupted.
     */
    virtual void lock(ResourceId resId, LockMode mode, Date_t deadline = Date_t::max()) = 0;

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
    virtual bool isDbLockedForMode(const DatabaseName& dbName, LockMode mode) const = 0;
    virtual bool isCollectionLockedForMode(const NamespaceString& nss, LockMode mode) const = 0;

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

    /**
     * lockStatsBase is the snapshot of the lock stats taken at the point when the operation starts.
     * The precise lock stats of a sub-operation would be the stats from the locker info minus the
     * lockStatsBase.
     */
    virtual void getLockerInfo(LockerInfo* lockerInfo,
                               boost::optional<SingleThreadedLockStats> lockStatsBase) const = 0;

    /**
     * Returns boost::none if this is an instance of LockerNoop, or a populated LockerInfo
     * otherwise.
     */
    virtual boost::optional<LockerInfo> getLockerInfo(
        boost::optional<SingleThreadedLockStats> lockStatsBase) const = 0;

    /**
     * LockSnapshot captures the state of all resources that are locked, what modes they're
     * locked in, and how many times they've been locked in that mode.
     */
    struct LockSnapshot {
        // The global lock is handled differently from all other locks.
        LockMode globalMode;

        // The non-global locks held, sorted by granularity.  That is, locks[i] is
        // coarser or as coarse as locks[i + 1].
        std::vector<OneLock> locks;
    };

    /**
     * WUOWLockSnapshot captures all resources that have pending unlocks when releasing the write
     * unit of work. If a lock has more than one pending unlock, it appears more than once here.
     */
    struct WUOWLockSnapshot {
        // Nested WUOW can be released and restored all together.
        int wuowNestingLevel = 0;

        // The order of locks doesn't matter in this vector.
        std::vector<OneLock> unlockPendingLocks;
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
     * releaseWriteUnitOfWorkAndUnlock opts out of two-phase locking and yields the locks after a
     * WUOW has been released. restoreWriteUnitOfWorkAndLock reacquires the locks and resumes the
     * two-phase locking behavior of WUOW.
     */
    virtual bool releaseWriteUnitOfWorkAndUnlock(LockSnapshot* stateOut) = 0;
    virtual void restoreWriteUnitOfWorkAndLock(OperationContext* opCtx,
                                               const LockSnapshot& stateToRestore) = 0;


    /**
     * releaseWriteUnitOfWork opts out of two-phase locking of the current locks held but keeps
     * holding these locks.
     * restoreWriteUnitOfWork resumes the two-phase locking behavior of WUOW.
     */
    virtual void releaseWriteUnitOfWork(WUOWLockSnapshot* stateOut) = 0;
    virtual void restoreWriteUnitOfWork(const WUOWLockSnapshot& stateToRestore) = 0;

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

    /**
     * Returns true if a read ticket is held for the Locker.
     */
    virtual bool hasReadTicket() const = 0;

    /**
     * Returns true if a write ticket is held for the Locker.
     */
    virtual bool hasWriteTicket() const = 0;

    /**
     * Returns true if uninterruptible locks were requested for the Locker.
     */
    bool uninterruptibleLocksRequested() const {
        return _uninterruptibleLocksRequested > 0;
    }

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

    virtual bool isRSTLExclusive() const = 0;
    virtual bool isRSTLLocked() const = 0;

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
     * If set to false, this opts out of conflicting with the barrier created by the
     * setFeatureCompatibilityVersion command. Code that opts-out must be ok with writes being able
     * to start under one FCV and complete under a different FCV.
     */
    void setShouldConflictWithSetFeatureCompatibilityVersion(bool newValue) {
        _shouldConflictWithSetFeatureCompatibilityVersion = newValue;
    }
    bool shouldConflictWithSetFeatureCompatibilityVersion() const {
        return _shouldConflictWithSetFeatureCompatibilityVersion;
    }

    /**
     * If set to true, this opts out of a fatal assertion where operations which are holding open an
     * oplog hole cannot try to acquire subsequent locks.
     */
    void setAllowLockAcquisitionOnTimestampedUnitOfWork(bool newValue) {
        _shouldAllowLockAcquisitionOnTimestampedUnitOfWork = newValue;
    }
    bool shouldAllowLockAcquisitionOnTimestampedUnitOfWork() const {
        return _shouldAllowLockAcquisitionOnTimestampedUnitOfWork;
    }

    /**
     * This will set the admission priority for the ticket mechanism.
     */
    void setAdmissionPriority(AdmissionContext::Priority priority) {
        invariant(isNoop() || getClientState() == Locker::ClientState::kInactive);
        _admCtx.setPriority(priority);
    }

    AdmissionContext::Priority getAcquisitionPriority() {
        return _admCtx.getPriority();
    }

    bool shouldAcquireTicket() const {
        return _admCtx.getPriority() != AdmissionContext::Priority::kImmediate;
    }

    /**
     * Acquire a flow control admission ticket into the system. Flow control is used as a
     * backpressure mechanism to limit replication majority point lag.
     */
    virtual void getFlowControlTicket(OperationContext* opCtx, LockMode lockMode) {}

    /**
     * If tracked by an implementation, returns statistics on effort spent acquiring a flow control
     * ticket.
     */
    virtual FlowControlTicketholder::CurOp getFlowControlStats() const {
        return FlowControlTicketholder::CurOp();
    }

    /**
     * This function is for unit testing only.
     */
    unsigned numResourcesToUnlockAtEndUnitOfWorkForTest() const {
        return _numResourcesToUnlockAtEndUnitOfWork;
    }

    std::string getDebugInfo() const {
        return _debugInfo;
    }

    void setDebugInfo(const std::string& info) {
        _debugInfo = info;
    }

protected:
    Locker() {}

    /**
     * The number of callers that are guarding from lock interruptions.
     * When 0, all lock acquisitions are interruptible. When positive, no lock acquisitions are
     * interruptible or can time out.
     */
    int _uninterruptibleLocksRequested = 0;

    /**
     * The number of callers that are guarding against uninterruptible lock requests. An int,
     * instead of a boolean, to support multiple simultaneous requests. When > 0, ensures that
     * _uninterruptibleLocksRequested above is _not_ used.
     */
    int _keepInterruptibleRequests = 0;

    /**
     * The number of LockRequests to unlock at the end of this WUOW. This is used for locks
     * participating in two-phase locking.
     */
    unsigned _numResourcesToUnlockAtEndUnitOfWork = 0;

    // Keeps state and statistics related to admission control.
    AdmissionContext _admCtx;

private:
    bool _shouldConflictWithSecondaryBatchApplication = true;
    bool _shouldConflictWithSetFeatureCompatibilityVersion = true;
    bool _shouldAllowLockAcquisitionOnTimestampedUnitOfWork = false;
    std::string _debugInfo;  // Extra info about this locker for debugging purpose
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
        invariant(_locker->_keepInterruptibleRequests == 0);
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
 * This RAII type ensures that there are no uninterruptible lock acquisitions while in scope. If an
 * UninterruptibleLockGuard is held at a higher level, or taken at a lower level, an invariant will
 * occur. This protects against UninterruptibleLockGuard uses on code paths that must be
 * interruptible. Safe to nest InterruptibleLockGuard instances.
 */
class InterruptibleLockGuard {
    InterruptibleLockGuard(const InterruptibleLockGuard& other) = delete;
    InterruptibleLockGuard(InterruptibleLockGuard&& other) = delete;

public:
    /*
     * Accepts a Locker, and increments the Locker's _keepInterruptibleRequests counter. Decrements
     * the counter when destroyed.
     */
    explicit InterruptibleLockGuard(Locker* locker) : _locker(locker) {
        invariant(_locker);
        invariant(_locker->_uninterruptibleLocksRequested == 0);
        invariant(_locker->_keepInterruptibleRequests >= 0);
        invariant(_locker->_keepInterruptibleRequests < std::numeric_limits<int>::max());
        _locker->_keepInterruptibleRequests += 1;
    }

    ~InterruptibleLockGuard() {
        invariant(_locker->_keepInterruptibleRequests > 0);
        _locker->_keepInterruptibleRequests -= 1;
    }

private:
    Locker* const _locker;
};

/**
 * RAII-style class to opt out of replication's use of the ParallelBatchWriterMode lock.
 */
class ShouldNotConflictWithSecondaryBatchApplicationBlock {
    ShouldNotConflictWithSecondaryBatchApplicationBlock(
        const ShouldNotConflictWithSecondaryBatchApplicationBlock&) = delete;
    ShouldNotConflictWithSecondaryBatchApplicationBlock& operator=(
        const ShouldNotConflictWithSecondaryBatchApplicationBlock&) = delete;

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

/**
 * RAII-style class to opt out the FeatureCompatibilityVersion lock.
 */
class ShouldNotConflictWithSetFeatureCompatibilityVersionBlock {
public:
    explicit ShouldNotConflictWithSetFeatureCompatibilityVersionBlock(Locker* lockState)
        : _lockState(lockState),
          _originalShouldConflict(_lockState->shouldConflictWithSetFeatureCompatibilityVersion()) {
        _lockState->setShouldConflictWithSetFeatureCompatibilityVersion(false);
    }

    ~ShouldNotConflictWithSetFeatureCompatibilityVersionBlock() {
        _lockState->setShouldConflictWithSetFeatureCompatibilityVersion(_originalShouldConflict);
    }

private:
    Locker* const _lockState;
    const bool _originalShouldConflict;
};

/**
 * RAII-style class to opt out of a fatal assertion where operations that set a timestamp on a
 * WriteUnitOfWork cannot try to acquire subsequent locks. When an operation is writing at a
 * specific timestamp, it creates an oplog hole at that timestamp. The oplog visibility rules only
 * makes oplog entries visible that are before the earliest oplog hole.
 *
 * Given that, the following is an example scenario that could result in a resource deadlock:
 * Op 1: Creates an oplog hole at Timestamp(5), then tries to acquire an exclusive lock.
 * Op 2: Holds the exclusive lock Op 1 is waiting for, while this operation is waiting for some
 *       operation beyond Timestamp(5) to become visible in the oplog.
 */
class AllowLockAcquisitionOnTimestampedUnitOfWork {
    AllowLockAcquisitionOnTimestampedUnitOfWork(
        const AllowLockAcquisitionOnTimestampedUnitOfWork&) = delete;
    AllowLockAcquisitionOnTimestampedUnitOfWork& operator=(
        const AllowLockAcquisitionOnTimestampedUnitOfWork&) = delete;

public:
    explicit AllowLockAcquisitionOnTimestampedUnitOfWork(Locker* lockState)
        : _lockState(lockState),
          _originalValue(_lockState->shouldAllowLockAcquisitionOnTimestampedUnitOfWork()) {
        _lockState->setAllowLockAcquisitionOnTimestampedUnitOfWork(true);
    }

    ~AllowLockAcquisitionOnTimestampedUnitOfWork() {
        _lockState->setAllowLockAcquisitionOnTimestampedUnitOfWork(_originalValue);
    }

private:
    Locker* const _lockState;
    bool _originalValue;
};

}  // namespace mongo
