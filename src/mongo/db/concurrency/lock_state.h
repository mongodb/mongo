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

#include <queue>

#include "mongo/db/concurrency/fast_map_noalloc.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/ticketholder.h"

namespace mongo {

/**
 * Notfication callback, which stores the last notification result and signals a condition
 * variable, which can be waited on.
 */
class CondVarLockGrantNotification : public LockGrantNotification {
    CondVarLockGrantNotification(const CondVarLockGrantNotification&) = delete;
    CondVarLockGrantNotification& operator=(const CondVarLockGrantNotification&) = delete;

public:
    CondVarLockGrantNotification();

    /**
     * Clears the object so it can be reused.
     */
    void clear();

    /**
     * Uninterruptible blocking method, which waits for the notification to fire.
     *
     * @param timeout How many milliseconds to wait before returning LOCK_TIMEOUT.
     */
    LockResult wait(Milliseconds timeout);

    /**
     * Interruptible blocking method, which waits for the notification to fire or an interrupt from
     * the operation context.
     *
     * @param opCtx OperationContext to wait on for an interrupt.
     * @param timeout How many milliseconds to wait before returning LOCK_TIMEOUT.
     */
    LockResult wait(OperationContext* opCtx, Milliseconds timeout);

private:
    virtual void notify(ResourceId resId, LockResult result);

    // These two go together to implement the conditional variable pattern.
    Mutex _mutex = MONGO_MAKE_LATCH("CondVarLockGrantNotification::_mutex");
    stdx::condition_variable _cond;

    // Result from the last call to notify
    LockResult _result;
};

/**
 * Interface for acquiring locks. One of those objects will have to be instantiated for each
 * request (transaction).
 *
 * Lock/unlock methods must always be called from a single thread.
 *
 * All instances reference a single global lock manager.
 *
 */
class LockerImpl : public Locker {
public:
    /**
     * Instantiates new locker. Must be given a unique identifier for disambiguation. Lockers
     * having the same identifier will not conflict on lock acquisition.
     */
    LockerImpl(ServiceContext* serviceContext);

    virtual ~LockerImpl();

    virtual ClientState getClientState() const;

    virtual LockerId getId() const {
        return _id;
    }

    stdx::thread::id getThreadId() const override;

    void updateThreadIdToCurrentThread() override;
    void unsetThreadId() override;

    void setSharedLocksShouldTwoPhaseLock(bool sharedLocksShouldTwoPhaseLock) override {
        _sharedLocksShouldTwoPhaseLock = sharedLocksShouldTwoPhaseLock;
    }

    void setMaxLockTimeout(Milliseconds maxTimeout) override {
        _maxLockTimeout = maxTimeout;
    }

    bool hasMaxLockTimeout() override {
        return static_cast<bool>(_maxLockTimeout);
    }

    void unsetMaxLockTimeout() override {
        _maxLockTimeout = boost::none;
    }

    /**
     * Acquires the ticket within the deadline (or _maxLockTimeout) and tries to grab the lock.
     */
    virtual void lockGlobal(OperationContext* opCtx,
                            LockMode mode,
                            Date_t deadline = Date_t::max());

    virtual bool unlockGlobal();

    virtual LockResult lockRSTLBegin(OperationContext* opCtx, LockMode mode);
    virtual void lockRSTLComplete(OperationContext* opCtx, LockMode mode, Date_t deadline);

    virtual bool unlockRSTLforPrepare();

    virtual void beginWriteUnitOfWork() override;
    virtual void endWriteUnitOfWork() override;

    virtual bool inAWriteUnitOfWork() const {
        return _wuowNestingLevel > 0;
    }

    bool wasGlobalLockTakenForWrite() const override;

    bool wasGlobalLockTakenInModeConflictingWithWrites() const override;

    bool wasGlobalLockTaken() const override;

    void setGlobalLockTakenInMode(LockMode mode) override;

    /**
     * Requests a lock for resource 'resId' with mode 'mode'. An OperationContext 'opCtx' must be
     * provided to interrupt waiting on the locker condition variable that indicates status of
     * the lock acquisition. A lock operation would otherwise wait until a timeout or the lock is
     * granted.
     */
    virtual void lock(OperationContext* opCtx,
                      ResourceId resId,
                      LockMode mode,
                      Date_t deadline = Date_t::max());

    virtual void lock(ResourceId resId, LockMode mode, Date_t deadline = Date_t::max()) {
        lock(nullptr, resId, mode, deadline);
    }

    virtual void downgrade(ResourceId resId, LockMode newMode);

    virtual bool unlock(ResourceId resId);

    virtual LockMode getLockMode(ResourceId resId) const;
    virtual bool isLockHeldForMode(ResourceId resId, LockMode mode) const;
    virtual bool isDbLockedForMode(const DatabaseName& dbName, LockMode mode) const;
    virtual bool isCollectionLockedForMode(const NamespaceString& nss, LockMode mode) const;

    virtual ResourceId getWaitingResource() const;

    virtual void getLockerInfo(LockerInfo* lockerInfo,
                               boost::optional<SingleThreadedLockStats> lockStatsBase) const;
    virtual boost::optional<LockerInfo> getLockerInfo(
        boost::optional<SingleThreadedLockStats> lockStatsBase) const final;

    virtual bool saveLockStateAndUnlock(LockSnapshot* stateOut);

    virtual void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore);
    virtual void restoreLockState(const LockSnapshot& stateToRestore) {
        restoreLockState(nullptr, stateToRestore);
    }

    bool releaseWriteUnitOfWorkAndUnlock(LockSnapshot* stateOut) override;
    void restoreWriteUnitOfWorkAndLock(OperationContext* opCtx,
                                       const LockSnapshot& stateToRestore) override;

    void releaseWriteUnitOfWork(WUOWLockSnapshot* stateOut) override;
    void restoreWriteUnitOfWork(const WUOWLockSnapshot& stateToRestore) override;

    virtual void releaseTicket();
    virtual void reacquireTicket(OperationContext* opCtx);

    bool hasReadTicket() const override {
        return _modeForTicket == MODE_IS || _modeForTicket == MODE_S;
    }

    bool hasWriteTicket() const override {
        return _modeForTicket == MODE_IX || _modeForTicket == MODE_X;
    }

    void getFlowControlTicket(OperationContext* opCtx, LockMode lockMode) override;

    FlowControlTicketholder::CurOp getFlowControlStats() const override;

    //
    // Below functions are for testing only.
    //

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
        _lockComplete(opCtx, resId, mode, deadline);
    }

private:
    typedef FastMapNoAlloc<ResourceId, LockRequest> LockRequestsMap;

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
     *
     * Throws an exception if it is interrupted.
     */
    void _lockComplete(OperationContext* opCtx, ResourceId resId, LockMode mode, Date_t deadline);

    void _lockComplete(ResourceId resId, LockMode mode, Date_t deadline) {
        _lockComplete(nullptr, resId, mode, deadline);
    }

    /**
     * The main functionality of the unlock method, except accepts iterator in order to avoid
     * additional lookups during unlockGlobal. Frees locks immediately, so must not be called from
     * inside a WUOW.
     */
    bool _unlockImpl(LockRequestsMap::Iterator* it);

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
     * Releases the ticket for the Locker.
     */
    void _releaseTicket();

    /**
     * Acquires a ticket for the Locker under 'mode'.
     * Returns true   if a ticket is successfully acquired.
     *         false  if it cannot acquire a ticket within 'deadline'.
     * It may throw an exception when it is interrupted.
     */
    bool _acquireTicket(OperationContext* opCtx, LockMode mode, Date_t deadline);

    void _setWaitingResource(ResourceId resId);

    /**
     * Calls dump() on this locker instance and the lock manager.
     */
    void _dumpLockerAndLockManagerRequests();

    // Used to disambiguate different lockers
    const LockerId _id;

    // The only reason we have this spin lock here is for the diagnostic tools, which could
    // iterate through the LockRequestsMap on a separate thread and need it to be stable.
    // Apart from that, all accesses to the LockerImpl are always from a single thread.
    //
    // This has to be locked inside const methods, hence the mutable.
    mutable SpinLock _lock;
    // Note: this data structure must always guarantee the continued validity of pointers/references
    // to its contents (LockRequests). The LockManager maintains a LockRequestList of pointers to
    // the LockRequests managed by this data structure.
    LockRequestsMap _requests;

    // Reuse the notification object across requests so we don't have to create a new mutex
    // and condition variable every time.
    CondVarLockGrantNotification _notify;

    // Per-locker locking statistics. Reported in the slow-query log message and through
    // db.currentOp. Complementary to the per-instance locking statistics.
    AtomicLockStats _stats;

    // Delays release of exclusive/intent-exclusive locked resources until the write unit of
    // work completes. Value of 0 means we are not inside a write unit of work.
    int _wuowNestingLevel;

    // Mode for which the Locker acquired a ticket, or MODE_NONE if no ticket was acquired.
    LockMode _modeForTicket = MODE_NONE;

    // Indicates whether the client is active reader/writer or is queued.
    AtomicWord<ClientState> _clientState{kInactive};

    // Track the thread who owns the lock for debugging purposes
    stdx::thread::id _threadId;

    // If true, shared locks will participate in two-phase locking.
    bool _sharedLocksShouldTwoPhaseLock = false;

    // If this is set, dictates the max number of milliseconds that we will wait for lock
    // acquisition. Effectively resets lock acquisition deadlines to time out sooner. If set to 0,
    // for example, lock attempts will time out immediately if the lock is not immediately
    // available. Note this will be ineffective if uninterruptible lock guard is set.
    boost::optional<Milliseconds> _maxLockTimeout;

    // A structure for accumulating time spent getting flow control tickets.
    FlowControlTicketholder::CurOp _flowControlStats;

    // The global ticketholders of the service context.
    TicketHolder* _ticketHolder;

    // This will only be valid when holding a ticket.
    boost::optional<Ticket> _ticket;

    // Tracks the global lock modes ever acquired in this Locker's life. This value should only ever
    // be accessed from the thread that owns the Locker.
    unsigned char _globalLockMode = (1 << MODE_NONE);

    // Tracks whether this operation should be killed on step down.
    AtomicWord<bool> _wasGlobalLockTakenInModeConflictingWithWrites{false};

    // If isValid(), the ResourceId of the resource currently waiting for the lock. If not valid,
    // there is no resource currently waiting.
    ResourceId _waitingResource;

    //////////////////////////////////////////////////////////////////////////////////////////
    //
    // Methods merged from LockState, which should eventually be removed or changed to methods
    // on the LockerImpl interface.
    //

public:
    virtual void dump() const;

    virtual bool isW() const;
    virtual bool isR() const;

    virtual bool isLocked() const;
    virtual bool isWriteLocked() const;
    virtual bool isReadLocked() const;

    virtual bool isRSTLExclusive() const;
    virtual bool isRSTLLocked() const;

    bool isGlobalLockedRecursively() override;

    virtual bool hasLockPending() const {
        return getWaitingResource().isValid();
    }
};

/**
 * RAII-style class to set the priority for the ticket acquisition mechanism when acquiring a global
 * lock.
 */
class SetTicketAquisitionPriorityForLock {
public:
    SetTicketAquisitionPriorityForLock(const SetTicketAquisitionPriorityForLock&) = delete;
    SetTicketAquisitionPriorityForLock& operator=(const SetTicketAquisitionPriorityForLock&) =
        delete;
    explicit SetTicketAquisitionPriorityForLock(OperationContext* opCtx,
                                                AdmissionContext::Priority priority)
        : _opCtx(opCtx), _originalPriority(opCtx->lockState()->getAcquisitionPriority()) {
        uassert(ErrorCodes::IllegalOperation,
                "It is illegal for an operation to demote a high priority to a lower priority "
                "operation",
                _originalPriority != AdmissionContext::Priority::kImmediate ||
                    priority == AdmissionContext::Priority::kImmediate);
        _opCtx->lockState()->setAdmissionPriority(priority);
    }

    ~SetTicketAquisitionPriorityForLock() {
        _opCtx->lockState()->setAdmissionPriority(_originalPriority);
    }

private:
    OperationContext* _opCtx;
    AdmissionContext::Priority _originalPriority;
};

/**
 * Retrieves the global lock manager instance.
 * Legacy global lock manager accessor for internal lock implementation * and debugger scripts
 * such as gdb/mongo_lock.py.
 * The lock manager is now a decoration on the service context and this accessor is retained for
 * startup, lock internals, and debugger scripts.
 * Using LockManager::get(ServiceContext*) where possible is preferable.
 */
LockManager* getGlobalLockManager();

}  // namespace mongo
