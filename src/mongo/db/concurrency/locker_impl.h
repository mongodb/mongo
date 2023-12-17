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
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <queue>
#include <thread>

#include "mongo/db/concurrency/fast_map_noalloc.h"
#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/concurrency/lock_manager.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_stats.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/ticketholder_manager.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

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
    void notify(ResourceId resId, LockResult result) override;

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
// TODO (SERVER-77213): Get rid of LockerImpl, devirtualise Locker and make it final
class LockerImpl final : public Locker {
public:
    /**
     * Instantiates new locker. Must be given a unique identifier for disambiguation. Lockers
     * having the same identifier will not conflict on lock acquisition.
     */
    LockerImpl(ServiceContext* serviceContext);

    virtual ~LockerImpl();

    ClientState getClientState() const override;

    LockerId getId() const override {
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
    void lockGlobal(OperationContext* opCtx,
                    LockMode mode,
                    Date_t deadline = Date_t::max()) override;

    bool unlockGlobal() override;

    LockResult lockRSTLBegin(OperationContext* opCtx, LockMode mode) override;
    void lockRSTLComplete(OperationContext* opCtx,
                          LockMode mode,
                          Date_t deadline,
                          const LockTimeoutCallback& onTimeout) override;

    bool unlockRSTLforPrepare() override;

    void beginWriteUnitOfWork() override;
    void endWriteUnitOfWork() override;

    bool inAWriteUnitOfWork() const override {
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
    void lock(OperationContext* opCtx,
              ResourceId resId,
              LockMode mode,
              Date_t deadline = Date_t::max()) override;

    void downgrade(ResourceId resId, LockMode newMode) override;

    bool unlock(ResourceId resId) override;

    LockMode getLockMode(ResourceId resId) const override;
    bool isLockHeldForMode(ResourceId resId, LockMode mode) const override;
    bool isDbLockedForMode(const DatabaseName& dbName, LockMode mode) const override;
    bool isCollectionLockedForMode(const NamespaceString& nss, LockMode mode) const override;

    ResourceId getWaitingResource() const override;

    void getLockerInfo(LockerInfo* lockerInfo,
                       boost::optional<SingleThreadedLockStats> lockStatsBase) const override;
    boost::optional<LockerInfo> getLockerInfo(
        boost::optional<SingleThreadedLockStats> lockStatsBase) const override;

    void saveLockStateAndUnlock(LockSnapshot* stateOut) override;

    void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore) override;

    void releaseWriteUnitOfWorkAndUnlock(LockSnapshot* stateOut) override;
    void restoreWriteUnitOfWorkAndLock(OperationContext* opCtx,
                                       const LockSnapshot& stateToRestore) override;

    void releaseWriteUnitOfWork(WUOWLockSnapshot* stateOut) override;
    void restoreWriteUnitOfWork(const WUOWLockSnapshot& stateToRestore) override;

    void releaseTicket() override;
    void reacquireTicket(OperationContext* opCtx) override;

    void dump() const override;

    bool isW() const override;
    bool isR() const override;

    bool isLocked() const override;
    bool isWriteLocked() const override;
    bool isReadLocked() const override;

    bool isRSTLExclusive() const override;
    bool isRSTLLocked() const override;

    bool isGlobalLockedRecursively() override;
    bool canSaveLockState() override;

    bool hasReadTicket() const override {
        return _modeForTicket == MODE_IS || _modeForTicket == MODE_S;
    }

    bool hasWriteTicket() const override {
        return _modeForTicket == MODE_IX || _modeForTicket == MODE_X;
    }

    void getFlowControlTicket(OperationContext* opCtx, LockMode lockMode) override;

    FlowControlTicketholder::CurOp getFlowControlStats() const override;

    std::vector<LogDegugInfo> getLockInfoFromResourceHolders(ResourceId resId);

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
        _lockComplete(opCtx, resId, mode, deadline, nullptr);
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

    // Used to disambiguate different lockers
    const LockerId _id;

    // Track the thread that currently owns the lock, for debugging purposes
    stdx::thread::id _threadId;

    // The global lock manager of the service context.
    LockManager* const _lockManager;

    // The global ticketholders of the service context.
    TicketHolderManager* const _ticketHolderManager;

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

    // Tracks the global lock modes ever acquired in this Locker's life. This value should only ever
    // be accessed from the thread that owns the Locker.
    unsigned char _globalLockMode = (1 << MODE_NONE);

    // Tracks whether this operation should be killed on step down.
    AtomicWord<bool> _wasGlobalLockTakenInModeConflictingWithWrites{false};

    // If isValid(), the ResourceId of the resource currently waiting for the lock. If not valid,
    // there is no resource currently waiting.
    ResourceId _waitingResource;
};

}  // namespace mongo
