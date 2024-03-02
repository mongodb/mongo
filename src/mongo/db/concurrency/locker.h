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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/db/concurrency/cond_var_lock_grant_notification.h"
#include "mongo/db/concurrency/fast_map_noalloc.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;
class TicketHolderManager;

/**
 * Interface for acquiring locks. One of those objects will have to be instantiated for each
 * request (transaction).
 *
 * Lock/unlock methods must always be called from a single thread.
 */

// DO NOT ADD ANY SUBCLASSES, even though there are virtual methods.
//
// TODO (SERVER-77213): There are still virtual methods here, even though the class is final. They
// are required because of cyclic dependency between the service_context and write_unit_of_work
// libraries, due to Locker being currently owned by the OperationContext. This will go away once
// SERVER-77213 is done which will move locker entirely under TransactionResources (shard_role_api).
class Locker {
public:
    Locker(ServiceContext* serviceContext);
    virtual ~Locker();

    // Non-copyable, non-movable
    Locker(const Locker&) = delete;
    Locker& operator=(const Locker&) = delete;

    LockerId getId() const {
        return _id;
    }

    /**
     * Returns the platform-specific thread identifier of the thread which currently owns the this
     * locker, for diagnostics purposes.
     */
    stdx::thread::id getThreadId() const {
        return _threadId;
    }
    void updateThreadIdToCurrentThread();
    void unsetThreadId();

    std::string getDebugInfo() const;
    void setDebugInfo(const std::string& info);

    /**
     * State for reporting the number of active and queued reader and writer clients.
     */
    enum ClientState { kInactive, kActiveReader, kActiveWriter, kQueuedReader, kQueuedWriter };

    /**
     * Return whether client is holding any locks (active), or is queued on any locks or waiting
     * for a ticket (throttled).
     */
    ClientState getClientState() const;

    bool shouldWaitForTicket(OperationContext* opCtx) const {
        return AdmissionContext::get(opCtx).getPriority() != AdmissionContext::Priority::kExempt;
    }

    /**
     * Acquire a flow control admission ticket into the system. Flow control is used as a
     * backpressure mechanism to limit replication majority point lag.
     */
    void getFlowControlTicket(OperationContext* opCtx, LockMode lockMode);

    /**
     * If tracked by an implementation, returns statistics on effort spent acquiring a flow control
     * ticket.
     */
    FlowControlTicketholder::CurOp getFlowControlStats() const;

    /**
     * Reacquires a ticket for the Locker. This must only be called after releaseTicket(). It
     * restores the ticket under its previous LockMode.
     *
     * Requires that all locks granted to this locker are modes IS or IX.
     *
     * Note that this ticket acquisition will not time out due to a max lock timeout set on the
     * locker. However, it may time out if a potential deadlock scenario is detected due to ticket
     * exhaustion and pending S or X locks.
     */
    void reacquireTicket(OperationContext* opCtx);

    /**
     * Releases the ticket associated with the Locker. This allows locks to be held without
     * contributing to reader/writer throttling.
     */
    void releaseTicket();

    /**
     * Returns true if a read ticket is held for the Locker.
     */
    bool hasReadTicket() const {
        return _modeForTicket == MODE_IS || _modeForTicket == MODE_S;
    }

    /**
     * Returns true if a write ticket is held for the Locker.
     */
    bool hasWriteTicket() const {
        return _modeForTicket == MODE_IX || _modeForTicket == MODE_X;
    }

    Microseconds getTimeQueuedForTicketMicros() const {
        return _timeQueuedForTicketMicros;
    }

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
    void lockGlobal(OperationContext* opCtx, LockMode mode, Date_t deadline = Date_t::max());

    /**
     * Decrements the reference count on the global lock.  If the reference count on the
     * global lock hits zero, the transaction is over, and unlockGlobal unlocks all other locks
     * except for RESOURCE_MUTEX and RESOURCE_DDL_* locks.
     *
     * @return true if this is the last endTransaction call (i.e., the global lock was
     *          released); false if there are still references on the global lock. This value
     *          should not be relied on and is only used for assertion purposes.
     *
     * @return false if the global lock is still held.
     */
    bool unlockGlobal();

    /**
     * Requests the RSTL to be acquired in the requested mode (typically mode X) . This should only
     * be called inside ReplicationStateTransitionLockGuard.
     *
     * See the comments for _lockBegin/Complete for more information on the semantics.
     */
    LockResult lockRSTLBegin(OperationContext* opCtx, LockMode mode);

    using LockTimeoutCallback = std::function<void()>;
    /**
     * Waits for the completion of acquiring the RSTL. This should only be called inside
     * ReplicationStateTransitionLockGuard.
     *
     * It may throw an exception if it is interrupted.
     */
    void lockRSTLComplete(OperationContext* opCtx,
                          LockMode mode,
                          Date_t deadline,
                          const LockTimeoutCallback& onTimeout = nullptr);

    /**
     * Unlocks the RSTL when the transaction becomes prepared. This is used to bypass two-phase
     * locking and unlock the RSTL immediately, rather than at the end of the WUOW.
     *
     * @return true if the RSTL is unlocked; false if we fail to unlock the RSTL or if it was
     * already unlocked.
     */
    bool unlockRSTLforPrepare();

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
    void lock(OperationContext* opCtx,
              ResourceId resId,
              LockMode mode,
              Date_t deadline = Date_t::max());

    /**
     * Downgrades the specified resource's lock mode without changing the reference count.
     */
    void downgrade(ResourceId resId, LockMode newMode);

    /**
     * Releases a lock previously acquired through a lock call. It is an error to try to
     * release lock which has not been previously acquired (invariant violation).
     *
     * @return true if the lock was actually released; false if only the reference count was
     *              decremented, but the lock is still held.
     */
    bool unlock(ResourceId resId);

    /**
     * beginWriteUnitOfWork/endWriteUnitOfWork are called at the start and end of WriteUnitOfWorks.
     * They can be used to implement two-phase locking. Each call to begin or restore should be
     * matched with an eventual call to end or release.
     *
     * endWriteUnitOfWork, if not called in a nested WUOW, will release all two-phase locking held
     * lock resources.
     */
    virtual void beginWriteUnitOfWork();
    virtual void endWriteUnitOfWork();

    bool inAWriteUnitOfWork() const {
        return _wuowNestingLevel > 0;
    }

    /**
     * Returns the resource that this locker is waiting/blocked on (if any). If the locker is
     * not waiting for a resource the returned value will be invalid (isValid() == false).
     */
    ResourceId getWaitingResource() const;

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
     * Returns information and locking statistics for this instance of the locker. Used to support
     * the db.currentOp view. This structure is not thread-safe and ideally should be used only for
     * obtaining the necessary information and then discarded instead of reused.
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
    void getLockerInfo(LockerInfo* lockerInfo,
                       const boost::optional<SingleThreadedLockStats>& alreadyCountedStats) const;

    /**
     * Returns diagnostics information for the locker.
     */
    LockerInfo getLockerInfo(
        const boost::optional<SingleThreadedLockStats>& alreadyCountedStats) const;

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
     * Determines if this operation can safely release its locks for yielding. This must precede a
     * call to saveLockStateAndUnlock() at the risk of failing any invariants.
     *
     * Returns false when no locks are held.
     */
    bool canSaveLockState();

    /**
     * Retrieves all locks held by this transaction, other than RESOURCE_MUTEX and RESOURCE_DDL_*
     * locks, and what mode they're held in.
     *
     * Unlocks all locks held by this transaction, and stores them in 'stateOut'. This functionality
     * is used for yielding, which is voluntary/cooperative lock release and reacquisition in order
     * to allow for interleaving of otherwise conflicting long-running operations. The LockSnapshot
     * can then be passed to restoreLockState() after yielding to reacquire all released locks.
     *
     * This functionality is also used for releasing locks on databases and collections
     * when cursors are dormant and waiting for a getMore request.
     *
     * Callers are expected to check if locks are yieldable first by calling canSaveLockState(),
     * otherwise this function will invariant.
     */
    void saveLockStateAndUnlock(LockSnapshot* stateOut);

    /**
     * Re-locks all locks whose state was stored in 'stateToRestore'.
     *
     * @param opCtx An operation context that enables the restoration to be interrupted.
     */
    void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore);

    /**
     * releaseWriteUnitOfWorkAndUnlock opts out of two-phase locking and yields the locks after a
     * WUOW has been released. restoreWriteUnitOfWorkAndLock reacquires the locks and resumes the
     * two-phase locking behavior of WUOW.
     */
    void releaseWriteUnitOfWorkAndUnlock(LockSnapshot* stateOut);
    void restoreWriteUnitOfWorkAndLock(OperationContext* opCtx, const LockSnapshot& stateToRestore);

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
     * releaseWriteUnitOfWork opts out of two-phase locking of the current locks held but keeps
     * holding these locks.
     * restoreWriteUnitOfWork resumes the two-phase locking behavior of WUOW.
     */
    void releaseWriteUnitOfWork(WUOWLockSnapshot* stateOut);
    void restoreWriteUnitOfWork(const WUOWLockSnapshot& stateToRestore);

    /**
     * Indicate that shared locks should participate in two-phase locking for this Locker instance.
     */
    void setSharedLocksShouldTwoPhaseLock(bool sharedLocksShouldTwoPhaseLock) {
        _sharedLocksShouldTwoPhaseLock = sharedLocksShouldTwoPhaseLock;
    }

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
     *
     * Note that this max lock timeout will not apply to ticket acquisition.
     */
    void setMaxLockTimeout(Milliseconds maxTimeout) {
        _maxLockTimeout = maxTimeout;
    }

    /**
     * Returns whether this Locker has a maximum lock timeout set.
     */
    bool hasMaxLockTimeout() const {
        return static_cast<bool>(_maxLockTimeout);
    }

    /**
     * Clears the max lock timeout override set by setMaxLockTimeout() above.
     */
    void unsetMaxLockTimeout() {
        _maxLockTimeout = boost::none;
    }

    /**
     * Retrieves the mode in which a lock is held or checks whether the lock held for a particular
     * resource covers the specified mode.
     *
     * For example isLockHeldForMode will return true for MODE_S, if MODE_X is already held, because
     * MODE_X covers MODE_S.
     */
    LockMode getLockMode(ResourceId resId) const;

    /**
     * These are shortcut methods for the above calls. They however check that the entire hierarchy
     * is properly locked and because of this they are very expensive to call.
     *
     * Do not use them in performance critical code paths.
     */
    bool isDbLockedForMode(const DatabaseName& dbName, LockMode mode) const;
    bool isCollectionLockedForMode(const NamespaceString& nss, LockMode mode) const;

    bool isLockHeldForMode(ResourceId resId, LockMode mode) const {
        return isModeCovered(mode, getLockMode(resId));
    }

    bool isGlobalLockedRecursively() const;

    /**
     * Returns whether we have ever taken a global lock in X or IX mode in this operation.
     * Should only be called on the thread owning the locker.
     */
    bool wasGlobalLockTakenForWrite() const {
        return _globalLockMode & ((1 << MODE_IX) | (1 << MODE_X));
    }

    /**
     * Returns whether we have ever taken a global lock in S, X, or IX mode in this operation.
     */
    bool wasGlobalLockTakenInModeConflictingWithWrites() const {
        return _wasGlobalLockTakenInModeConflictingWithWrites.load();
    }

    /**
     * Returns whether we have ever taken a global lock in this operation.
     * Should only be called on the thread owning the locker.
     */
    bool wasGlobalLockTaken() const {
        return _globalLockMode != (1 << MODE_NONE);
    }

    /**
     * Sets the mode bit in _globalLockMode. Once a mode bit is set, we won't clear it. Also sets
     * _wasGlobalLockTakenInModeConflictingWithWrites to true if the mode is S, X, or IX.
     */
    void setGlobalLockTakenInMode(LockMode mode);

    /**
     * Returns true if uninterruptible locks were requested for the Locker.
     */
    bool uninterruptibleLocksRequested() const {
        return _uninterruptibleLocksRequested > 0;
    }

    /**
     * Pending means we are currently trying to get a lock.
     */
    bool hasLockPending() const {
        return getWaitingResource().isValid();
    }

    /**
     * Returns a vector with the lock information from the given resource lock holders.
     */
    std::vector<LogDegugInfo> getLockInfoFromResourceHolders(ResourceId resId) const;

    void dump() const;

    bool isLocked() const {
        return getLockMode(resourceIdGlobal) != MODE_NONE;
    }

    bool isW() const {
        return getLockMode(resourceIdGlobal) == MODE_X;
    }

    bool isR() const {
        return getLockMode(resourceIdGlobal) == MODE_S;
    }

    bool isReadLocked() const {
        return isLockHeldForMode(resourceIdGlobal, MODE_IS);
    }

    bool isWriteLocked() const {
        return isLockHeldForMode(resourceIdGlobal, MODE_IX);
    }

    bool isRSTLLocked() const {
        return getLockMode(resourceIdReplicationStateTransitionLock) != MODE_NONE;
    }

    bool isRSTLExclusive() const {
        return getLockMode(resourceIdReplicationStateTransitionLock) == MODE_X;
    }

    void setTicketQueueTime(Milliseconds queueTime) {
        _timeQueuedForTicketMicros = duration_cast<Microseconds>(queueTime);
    }

    void setFlowControlTicketQueueTime(Milliseconds queueTime) {
        _flowControlStats.timeAcquiringMicros =
            durationCount<Microseconds>(duration_cast<Microseconds>(queueTime));
    }

    //
    // Below functions are for unit-testing only
    //

    unsigned numResourcesToUnlockAtEndUnitOfWorkForTest() const {
        return _numResourcesToUnlockAtEndUnitOfWork;
    }

    FastMapNoAlloc<ResourceId, LockRequest> getRequestsForTest() const {
        scoped_spinlock scopedLock(_lock);
        return _requests;
    }

    LockResult lockBeginForTest(OperationContext* opCtx, ResourceId resId, LockMode mode) {
        return _lockBegin(opCtx, resId, mode);
    }

    void lockCompleteForTest(OperationContext* opCtx,
                             ResourceId resId,
                             LockMode mode,
                             Date_t deadline) {
        _lockComplete(opCtx, resId, mode, deadline, nullptr);
    }

protected:
    using LockRequestsMap = FastMapNoAlloc<ResourceId, LockRequest>;

    friend class UninterruptibleLockGuard;
    friend class InterruptibleLockGuard;
    friend class AllowLockAcquisitionOnTimestampedUnitOfWork;

    /**
     * Allows for lock requests to be requested in a non-blocking way. There can be only one
     * outstanding pending lock request per locker object.
     *
     * _lockBegin posts a request to the lock manager for the specified lock to be acquired,
     * which either immediately grants the lock, or puts the requestor on the conflict queue
     * and returns immediately with the result of the acquisition. The result can be one of:
     *
     * LOCK_OK - Nothing more needs to be done. The lock is granted.
     * LOCK_WAITING - The request has been queued up and will be granted as soon as the lock
     *      is free. If this result is returned, typically _lockComplete needs to be called in
     *      order to wait for the actual grant to occur. If the caller no longer needs to wait
     *      for the grant to happen, unlock needs to be called with the same resource passed
     *      to _lockBegin.
     *
     * In other words for each call to _lockBegin, which does not return LOCK_OK, there needs to
     * be a corresponding call to either _lockComplete or unlock.
     *
     * If an operation context is provided that represents an interrupted operation, _lockBegin will
     * throw an exception whenever it would have been possible to grant the lock with LOCK_OK. This
     * behavior can be disabled with an UninterruptibleLockGuard.
     *
     * NOTE: These methods are not public and should only be used inside the class
     * implementation and for unit-tests and not called directly.
     */
    LockResult _lockBegin(OperationContext* opCtx, ResourceId resId, LockMode mode);

    /**
     * Waits for the completion of a lock, previously requested through _lockBegin/
     * Must only be called, if _lockBegin returned LOCK_WAITING.
     *
     * @param opCtx Operation context that, if not null, will be used to allow interruptible lock
     * acquisition.
     * @param resId Resource id which was passed to an earlier _lockBegin call. Must match.
     * @param mode Mode which was passed to an earlier _lockBegin call. Must match.
     * @param deadline The absolute time point when this lock acquisition will time out, if not yet
     * granted.
     * @param onTimeout Callback which will run if the lock acquisition is about to time out.
     *
     * Throws an exception if it is interrupted.
     */
    void _lockComplete(OperationContext* opCtx,
                       ResourceId resId,
                       LockMode mode,
                       Date_t deadline,
                       const LockTimeoutCallback& onTimeout);

    /**
     * Acquires a ticket for the Locker under 'mode'.
     * Returns true   if a ticket is successfully acquired.
     *         false  if it cannot acquire a ticket within 'deadline'.
     * It may throw an exception when it is interrupted.
     */
    bool _acquireTicket(OperationContext* opCtx, LockMode mode, Date_t deadline);

    /**
     * The main functionality of the unlock method, except accepts iterator in order to avoid
     * additional lookups during unlockGlobal. Frees locks immediately, so must not be called from
     * inside a WUOW.
     */
    bool _unlockImpl(LockRequestsMap::Iterator* it);

    /**
     * Releases the ticket for the Locker.
     */
    void _releaseTicket();

    /**
     * Called if a lock acquisition has blocked.
     */
    void _setWaitingResource(ResourceId resId);

    /**
     * Whether we should use two phase locking. Returns true if the particular lock's release should
     * be delayed until the end of the operation.
     *
     * We delay release of write operation locks (X, IX) in order to ensure that the data changes
     * they protect are committed successfully. endWriteUnitOfWork will release them afterwards.
     * This protects other threads from seeing inconsistent in-memory state.
     *
     * Shared locks (S, IS) will also participate in two-phase locking if
     * '_sharedLocksShouldTwoPhaseLock' is true. This will protect open storage engine transactions
     * across network calls.
     */
    bool _shouldDelayUnlock(ResourceId resId, LockMode mode) const;

    /**
     * Determines whether global and tenant lock state implies that some database or lower level
     * resource, such as a collection, belonging to a tenant identified by 'tenantId' is locked in
     * 'lockMode'.
     *
     * Returns:
     *   true, if the global and tenant locks imply that the resource is locked for 'mode';
     *   false, if the global and tenant locks imply that the resource is not locked for 'mode';
     *   boost::none, if the global and tenant lock state does not imply either outcome and lower
     * level locks should be consulted.
     */
    boost::optional<bool> _globalAndTenantLocksImplyDBOrCollectionLockedForMode(
        const boost::optional<TenantId>& tenantId, LockMode lockMode) const;

    /**
     * Calls dump() on this locker instance and the lock manager.
     */
    void _dumpLockerAndLockManagerRequests();

    // Used to disambiguate different lockers
    const LockerId _id;

    // The global lock manager of the service context.
    LockManager* const _lockManager;

    // The global ticketholders of the service context.
    TicketHolderManager* const _ticketHolderManager;

    // The only reason we have this spin lock here is for the diagnostic tools, which could iterate
    // through the LockRequestsMap on a separate thread and need it to be stable. Apart from that,
    // all accesses to the locker object are always from a single thread.
    //
    // This needs to be locked inside const methods, hence the mutable.
    mutable SpinLock _lock;

    // Track the thread that currently owns the lock, for debugging purposes
    stdx::thread::id _threadId;

    // Extra info about this locker for debugging purposes
    std::string _debugInfo;

    // Reuse the notification object across requests so we don't have to create a new mutex
    // and condition variable every time.
    CondVarLockGrantNotification _notify;

    // Note: this data structure must always guarantee the continued validity of pointers/references
    // to its contents (LockRequests). The LockManager maintains a LockRequestList of pointers to
    // the LockRequests managed by this data structure.
    LockRequestsMap _requests;

    // Per-locker locking statistics. Reported in the slow-query log message and through
    // db.currentOp. Complementary to the per-instance locking statistics.
    AtomicLockStats _stats;

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

    // If set to true, this opts out of a fatal assertion where operations which are holding open an
    // oplog hole cannot try to acquire subsequent locks.
    bool _shouldAllowLockAcquisitionOnTimestampedUnitOfWork = false;

    /**
     * The number of LockRequests to unlock at the end of this WUOW. This is used for locks
     * participating in two-phase locking.
     */
    unsigned _numResourcesToUnlockAtEndUnitOfWork = 0;

    // Delays release of exclusive/intent-exclusive locked resources until the write unit of
    // work completes. Value of 0 means we are not inside a write unit of work.
    int _wuowNestingLevel{0};

    // Mode for which the Locker acquired a ticket, or MODE_NONE if no ticket was acquired.
    LockMode _modeForTicket = MODE_NONE;

    // Indicates whether the client is active reader/writer or is queued.
    AtomicWord<ClientState> _clientState{kInactive};

    // If true, shared locks will participate in two-phase locking.
    bool _sharedLocksShouldTwoPhaseLock = false;

    // If this is set, dictates the max number of milliseconds that we will wait for lock
    // acquisition. Effectively resets lock acquisition deadlines to time out sooner. If set to 0,
    // for example, lock attempts will time out immediately if the lock is not immediately
    // available. Note this will be ineffective if uninterruptible lock guard is set.
    boost::optional<Milliseconds> _maxLockTimeout;

    // A structure for accumulating time spent getting flow control tickets.
    FlowControlTicketholder::CurOp _flowControlStats;

    // This will only be valid when holding a ticket.
    boost::optional<Ticket> _ticket;

    // Tracks accumulated time spent waiting for a ticket.
    Microseconds _timeQueuedForTicketMicros{0};

    // Tracks the global lock modes ever acquired in this Locker's life. This value should only ever
    // be accessed from the thread that owns the Locker.
    unsigned char _globalLockMode = (1 << MODE_NONE);

    // Tracks whether this operation should be killed on step down.
    AtomicWord<bool> _wasGlobalLockTakenInModeConflictingWithWrites{false};

    // If isValid(), the ResourceId of the resource currently waiting for the lock. If not valid,
    // there is no resource currently waiting.
    ResourceId _waitingResource;
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
    /**
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

    UninterruptibleLockGuard(const UninterruptibleLockGuard& other) = delete;
    UninterruptibleLockGuard& operator=(const UninterruptibleLockGuard&) = delete;

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

    InterruptibleLockGuard(const InterruptibleLockGuard& other) = delete;
    InterruptibleLockGuard& operator=(const InterruptibleLockGuard&) = delete;

    ~InterruptibleLockGuard() {
        invariant(_locker->_keepInterruptibleRequests > 0);
        _locker->_keepInterruptibleRequests -= 1;
    }

private:
    Locker* const _locker;
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
public:
    explicit AllowLockAcquisitionOnTimestampedUnitOfWork(Locker* locker)
        : _locker(locker),
          _originalValue(_locker->_shouldAllowLockAcquisitionOnTimestampedUnitOfWork) {
        _locker->_shouldAllowLockAcquisitionOnTimestampedUnitOfWork = true;
    }

    AllowLockAcquisitionOnTimestampedUnitOfWork(
        const AllowLockAcquisitionOnTimestampedUnitOfWork&) = delete;
    AllowLockAcquisitionOnTimestampedUnitOfWork& operator=(
        const AllowLockAcquisitionOnTimestampedUnitOfWork&) = delete;

    ~AllowLockAcquisitionOnTimestampedUnitOfWork() {
        _locker->_shouldAllowLockAcquisitionOnTimestampedUnitOfWork = _originalValue;
    }

private:
    Locker* const _locker;
    bool _originalValue;
};

}  // namespace mongo
