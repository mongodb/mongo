/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/concurrency/locker.h"

namespace mongo {

/**
 * Locker for mongos. Based on, but not inherited from, LockerNoop.
 */
class LockerMongos : public Locker {
public:
    LockerMongos() {}

    // TODO(SERVER-60229): Return false when mongos has a working lock manager.
    bool isNoop() const final {
        return true;
    }

    ClientState getClientState() const override {
        MONGO_UNREACHABLE;
    }

    LockerId getId() const override {
        MONGO_UNREACHABLE;
    }

    stdx::thread::id getThreadId() const override {
        MONGO_UNREACHABLE;
    }

    void updateThreadIdToCurrentThread() override {
        MONGO_UNREACHABLE;
    }

    void unsetThreadId() override {
        MONGO_UNREACHABLE;
    }

    void setSharedLocksShouldTwoPhaseLock(bool sharedLocksShouldTwoPhaseLock) override {
        MONGO_UNREACHABLE;
    }

    void setMaxLockTimeout(Milliseconds maxTimeout) override {
        MONGO_UNREACHABLE;
    }

    bool hasMaxLockTimeout() override {
        MONGO_UNREACHABLE;
    }

    void unsetMaxLockTimeout() override {
        MONGO_UNREACHABLE;
    }

    void lockGlobal(OperationContext* opCtx, LockMode mode, Date_t deadline) override {
        MONGO_UNREACHABLE;
    }

    bool unlockGlobal() override {
        MONGO_UNREACHABLE;
    }

    void beginWriteUnitOfWork() override {}

    void endWriteUnitOfWork() override {}

    bool inAWriteUnitOfWork() const override {
        return false;
    }

    bool wasGlobalLockTakenForWrite() const override {
        return false;
    }

    bool wasGlobalLockTakenInModeConflictingWithWrites() const override {
        return false;
    }

    bool wasGlobalLockTaken() const override {
        return false;
    }

    void setGlobalLockTakenInMode(LockMode mode) override {}

    LockResult lockRSTLBegin(OperationContext* opCtx, LockMode mode) override {
        MONGO_UNREACHABLE;
    }

    void lockRSTLComplete(OperationContext* opCtx, LockMode mode, Date_t deadline) override {
        MONGO_UNREACHABLE;
    }

    bool unlockRSTLforPrepare() override {
        MONGO_UNREACHABLE;
    }

    void lock(OperationContext* opCtx, ResourceId resId, LockMode mode, Date_t deadline) override {}

    void lock(ResourceId resId, LockMode mode, Date_t deadline) override {}

    void downgrade(ResourceId resId, LockMode newMode) override {
        MONGO_UNREACHABLE;
    }

    bool unlock(ResourceId resId) override {
        return true;
    }

    LockMode getLockMode(ResourceId resId) const override {
        MONGO_UNREACHABLE;
    }

    bool isLockHeldForMode(ResourceId resId, LockMode mode) const override {
        return true;
    }

    bool isDbLockedForMode(const DatabaseName& dbName, LockMode mode) const override {
        return true;
    }

    bool isCollectionLockedForMode(const NamespaceString& nss, LockMode mode) const override {
        return true;
    }

    ResourceId getWaitingResource() const override {
        MONGO_UNREACHABLE;
    }

    void getLockerInfo(LockerInfo* lockerInfo,
                       boost::optional<SingleThreadedLockStats> lockStatsBase) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<LockerInfo> getLockerInfo(
        boost::optional<SingleThreadedLockStats> lockStatsBase) const override {
        return boost::none;
    }

    bool saveLockStateAndUnlock(LockSnapshot* stateOut) override {
        MONGO_UNREACHABLE;
    }

    void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore) override {
        MONGO_UNREACHABLE;
    }

    void restoreLockState(const LockSnapshot& stateToRestore) override {
        MONGO_UNREACHABLE;
    }

    bool releaseWriteUnitOfWorkAndUnlock(LockSnapshot* stateOut) override {
        MONGO_UNREACHABLE;
    }

    void restoreWriteUnitOfWorkAndLock(OperationContext* opCtx,
                                       const LockSnapshot& stateToRestore) override {
        MONGO_UNREACHABLE;
    };

    void releaseWriteUnitOfWork(WUOWLockSnapshot* stateOut) override {
        MONGO_UNREACHABLE;
    }

    void restoreWriteUnitOfWork(const WUOWLockSnapshot& stateToRestore) override {
        MONGO_UNREACHABLE;
    };

    void releaseTicket() override {
        MONGO_UNREACHABLE;
    }

    void reacquireTicket(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    virtual bool hasReadTicket() const {
        MONGO_UNREACHABLE;
    }

    virtual bool hasWriteTicket() const {
        MONGO_UNREACHABLE;
    }

    void dump() const override {
        MONGO_UNREACHABLE;
    }

    bool isW() const override {
        return false;
    }

    bool isR() const override {
        MONGO_UNREACHABLE;
    }

    bool isLocked() const override {
        // This is necessary because replication makes decisions based on the answer to this, and
        // we wrote unit tests to test the behavior specifically when this returns "false".
        return false;
    }

    bool isWriteLocked() const override {
        return true;
    }

    bool isReadLocked() const override {
        return true;
    }

    bool isRSTLExclusive() const override {
        return true;
    }

    bool isRSTLLocked() const override {
        return true;
    }

    bool hasLockPending() const override {
        MONGO_UNREACHABLE;
    }

    bool isGlobalLockedRecursively() override {
        return false;
    }
};

}  // namespace mongo
