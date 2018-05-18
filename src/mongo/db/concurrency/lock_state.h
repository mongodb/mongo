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

#include <queue>

#include "mongo/db/concurrency/fast_map_noalloc.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/spin_lock.h"

namespace mongo {

/**
 * Notfication callback, which stores the last notification result and signals a condition
 * variable, which can be waited on.
 */
class CondVarLockGrantNotification : public LockGrantNotification {
    MONGO_DISALLOW_COPYING(CondVarLockGrantNotification);

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
    stdx::mutex _mutex;
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
 * @param IsForMMAPV1 Whether to compile-in the flush lock functionality, which is specific to
 *          the way the MMAP V1 (legacy) storag engine does commit concurrency control.
 */
template <bool IsForMMAPV1>
class LockerImpl : public Locker {
public:
    /**
     * Instantiates new locker. Must be given a unique identifier for disambiguation. Lockers
     * having the same identifier will not conflict on lock acquisition.
     */
    LockerImpl();

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

    virtual LockResult lockGlobal(OperationContext* opCtx, LockMode mode);
    virtual LockResult lockGlobal(LockMode mode) {
        return lockGlobal(nullptr, mode);
    }
    virtual LockResult lockGlobalBegin(OperationContext* opCtx, LockMode mode, Date_t deadline) {
        return _lockGlobalBegin(opCtx, mode, deadline);
    }
    virtual LockResult lockGlobalBegin(LockMode mode, Date_t deadline) {
        return _lockGlobalBegin(nullptr, mode, deadline);
    }
    virtual LockResult lockGlobalComplete(OperationContext* opCtx, Date_t deadline);
    virtual LockResult lockGlobalComplete(Date_t deadline) {
        return lockGlobalComplete(nullptr, deadline);
    }
    virtual void lockMMAPV1Flush();

    virtual void downgradeGlobalXtoSForMMAPV1();
    virtual bool unlockGlobal();

    virtual void beginWriteUnitOfWork();
    virtual void endWriteUnitOfWork();

    virtual bool inAWriteUnitOfWork() const {
        return _wuowNestingLevel > 0;
    }

    /**
     * Requests a lock for resource 'resId' with mode 'mode'. An OperationContext 'opCtx' must be
     * provided to interrupt waiting on the locker condition variable that indicates status of
     * the lock acquisition. A lock operation would otherwise wait until a timeout or the lock is
     * granted.
     */
    virtual LockResult lock(OperationContext* opCtx,
                            ResourceId resId,
                            LockMode mode,
                            Date_t deadline = Date_t::max(),
                            bool checkDeadlock = false);

    virtual LockResult lock(ResourceId resId,
                            LockMode mode,
                            Date_t deadline = Date_t::max(),
                            bool checkDeadlock = false) {
        return lock(nullptr, resId, mode, deadline, checkDeadlock);
    }

    virtual void downgrade(ResourceId resId, LockMode newMode);

    virtual bool unlock(ResourceId resId);

    virtual LockMode getLockMode(ResourceId resId) const;
    virtual bool isLockHeldForMode(ResourceId resId, LockMode mode) const;
    virtual bool isDbLockedForMode(StringData dbName, LockMode mode) const;
    virtual bool isCollectionLockedForMode(StringData ns, LockMode mode) const;

    virtual ResourceId getWaitingResource() const;

    virtual void getLockerInfo(LockerInfo* lockerInfo) const;
    virtual boost::optional<LockerInfo> getLockerInfo() const final;

    virtual bool saveLockStateAndUnlock(LockSnapshot* stateOut);

    virtual void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore);
    virtual void restoreLockState(const LockSnapshot& stateToRestore) {
        restoreLockState(nullptr, stateToRestore);
    }

    virtual void releaseTicket();
    virtual void reacquireTicket(OperationContext* opCtx);

    /**
     * Allows for lock requests to be requested in a non-blocking way. There can be only one
     * outstanding pending lock request per locker object.
     *
     * lockBegin posts a request to the lock manager for the specified lock to be acquired,
     * which either immediately grants the lock, or puts the requestor on the conflict queue
     * and returns immediately with the result of the acquisition. The result can be one of:
     *
     * LOCK_OK - Nothing more needs to be done. The lock is granted.
     * LOCK_WAITING - The request has been queued up and will be granted as soon as the lock
     *      is free. If this result is returned, typically lockComplete needs to be called in
     *      order to wait for the actual grant to occur. If the caller no longer needs to wait
     *      for the grant to happen, unlock needs to be called with the same resource passed
     *      to lockBegin.
     *
     * In other words for each call to lockBegin, which does not return LOCK_OK, there needs to
     * be a corresponding call to either lockComplete or unlock.
     *
     * If an operation context is provided that represents an interrupted operation, lockBegin will
     * throw an exception whenever it would have been possible to grant the lock with LOCK_OK. This
     * behavior can be disabled with an UninterruptibleLockGuard.
     *
     * NOTE: These methods are not public and should only be used inside the class
     * implementation and for unit-tests and not called directly.
     */
    LockResult lockBegin(OperationContext* opCtx, ResourceId resId, LockMode mode);

    /**
     * Waits for the completion of a lock, previously requested through lockBegin or
     * lockGlobalBegin. Must only be called, if lockBegin returned LOCK_WAITING.
     *
     * @param opCtx Operation context that, if not null, will be used to allow interruptible lock
     * acquisition.
     * @param resId Resource id which was passed to an earlier lockBegin call. Must match.
     * @param mode Mode which was passed to an earlier lockBegin call. Must match.
     * @param deadline The absolute time point when this lock acquisition will time out, if not yet
     * granted.
     * @param checkDeadlock whether to perform deadlock detection while waiting.
     */
    LockResult lockComplete(OperationContext* opCtx,
                            ResourceId resId,
                            LockMode mode,
                            Date_t deadline,
                            bool checkDeadlock);

    LockResult lockComplete(ResourceId resId, LockMode mode, Date_t deadline, bool checkDeadlock) {
        return lockComplete(nullptr, resId, mode, deadline, checkDeadlock);
    }

    /**
     * This function is for unit testing only.
     */
    FastMapNoAlloc<ResourceId, LockRequest> getRequestsForTest() const {
        scoped_spinlock scopedLock(_lock);
        return _requests;
    }

private:
    friend class AutoYieldFlushLockForMMAPV1Commit;

    typedef FastMapNoAlloc<ResourceId, LockRequest> LockRequestsMap;

    /**
     * Like lockGlobalBegin, but accepts a deadline for acquiring a ticket.
     */
    LockResult _lockGlobalBegin(OperationContext* opCtx, LockMode, Date_t deadline);

    /**
     * The main functionality of the unlock method, except accepts iterator in order to avoid
     * additional lookups during unlockGlobal. Frees locks immediately, so must not be called from
     * inside a WUOW.
     */
    bool _unlockImpl(LockRequestsMap::Iterator* it);

    /**
     * MMAP V1 locking code yields and re-acquires the flush lock occasionally in order to
     * allow the flush thread proceed. This call returns in what mode the flush lock should be
     * acquired. It is based on the type of the operation (IS for readers, IX for writers).
     */
    LockMode _getModeForMMAPV1FlushLock() const;

    /**
     * Whether the particular lock's release should be held until the end of the operation. We delay
     * release of exclusive locks (locks that are for write operations) in order to ensure that the
     * data they protect is committed successfully. Shared locks will also participate in two-phase
     * locking if '_sharedLocksShouldTwoPhaseLock' is true.
     */
    bool _shouldDelayUnlock(ResourceId resId, LockMode mode) const;

    /**
     * Releases the ticket for the Locker.
     */
    void _releaseTicket();

    /**
     * Acquires a ticket for the Locker under 'mode'. Returns LOCK_TIMEOUT if it cannot acquire a
     * ticket within 'deadline'.
     */
    LockResult _acquireTicket(OperationContext* opCtx, LockMode mode, Date_t deadline);

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
    SingleThreadedLockStats _stats;

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
    // available.
    boost::optional<Milliseconds> _maxLockTimeout;

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
    bool isGlobalLockedRecursively() override;

    virtual bool hasLockPending() const {
        return getWaitingResource().isValid();
    }
};

typedef LockerImpl<false> DefaultLockerImpl;
typedef LockerImpl<true> MMAPV1LockerImpl;


/**
 * At global synchronization points, such as drop database we are running under a global
 * exclusive lock and without an active write unit of work, doing changes which require global
 * commit. This utility allows the flush lock to be temporarily dropped so the flush thread
 * could run in such circumstances. Should not be used where write units of work are used,
 * because these have different mechanism of yielding the flush lock.
 */
class AutoYieldFlushLockForMMAPV1Commit {
public:
    AutoYieldFlushLockForMMAPV1Commit(Locker* locker);
    ~AutoYieldFlushLockForMMAPV1Commit();

private:
    MMAPV1LockerImpl* const _locker;
};


/**
 * This explains how the MMAP V1 durability system is implemented.
 *
 * Every server operation (OperationContext), must call Locker::lockGlobal as the first lock
 * action (it is illegal to acquire any other locks without calling this first). This action
 * acquires the global and flush locks in the appropriate modes (IS for read operations, IX
 * for write operations). Having the flush lock in one of these modes indicates to the flush
 * thread that there is an active reader or writer.
 *
 * Whenever the flush thread(dur.cpp) activates, it goes through the following steps :
 *
 * Acquire the flush lock in S mode using AutoAcquireFlushLockForMMAPV1Commit. This waits until
 * all current write activity on the system completes and does not allow any new operations to
 * start.
 *
 * Once the S lock is granted, the flush thread writes the journal entries to disk (it is
 * guaranteed that there will not be any modifications) and applies them to the shared view.
 *
 * After that, it upgrades the S lock to X and remaps the private view.
 *
 * NOTE: There should be only one usage of this class and this should be in dur.cpp
 */
class AutoAcquireFlushLockForMMAPV1Commit {
public:
    AutoAcquireFlushLockForMMAPV1Commit(Locker* locker);
    ~AutoAcquireFlushLockForMMAPV1Commit();

    /**
     * We need the exclusive lock in order to do the shared view remap.
     */
    void upgradeFlushLockToExclusive();

    /**
     * Allows the acquired flush lock to be prematurely released. This is helpful for the case
     * where we know that we won't be doing a remap after gathering the write intents, so the
     * rest can be done outside of flush lock.
     */
    void release();

private:
    Locker* const _locker;
    bool _released;
};


/**
 * Retrieves the global lock manager instance.
 */
LockManager* getGlobalLockManager();

}  // namespace mongo
