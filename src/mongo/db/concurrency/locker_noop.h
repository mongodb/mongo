
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

#include "mongo/db/concurrency/locker.h"

namespace mongo {

/**
 * Locker, which cannot be used to lock/unlock resources and just returns true for checks for
 * whether a particular resource is locked. Do not use it for cases where actual locking
 * behaviour is expected or locking is performed.
 */
class LockerNoop : public Locker {
public:
    LockerNoop() {}

    virtual bool isNoop() const {
        return true;
    }

    virtual ClientState getClientState() const {
        MONGO_UNREACHABLE;
    }

    virtual LockerId getId() const {
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

    virtual LockResult lockGlobal(OperationContext* opCtx, LockMode mode) {
        MONGO_UNREACHABLE;
    }

    virtual LockResult lockGlobal(LockMode mode) {
        MONGO_UNREACHABLE;
    }

    virtual LockResult lockGlobalBegin(OperationContext* opCtx, LockMode mode, Date_t deadline) {
        MONGO_UNREACHABLE;
    }

    virtual LockResult lockGlobalBegin(LockMode mode, Date_t deadline) {
        MONGO_UNREACHABLE;
    }

    virtual LockResult lockGlobalComplete(OperationContext* opCtx, Date_t deadline) {
        MONGO_UNREACHABLE;
    }

    virtual LockResult lockGlobalComplete(Date_t deadline) {
        MONGO_UNREACHABLE;
    }

    virtual bool unlockGlobal() {
        MONGO_UNREACHABLE;
    }

    virtual void beginWriteUnitOfWork() override {}

    virtual void endWriteUnitOfWork() override {}

    virtual bool inAWriteUnitOfWork() const {
        return false;
    }

    virtual LockResult lockRSTLBegin(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    virtual LockResult lockRSTLComplete(OperationContext* opCtx, Date_t deadline) {
        MONGO_UNREACHABLE;
    }

    virtual LockResult lock(OperationContext* opCtx,
                            ResourceId resId,
                            LockMode mode,
                            Date_t deadline) {
        return LockResult::LOCK_OK;
    }

    virtual LockResult lock(ResourceId resId, LockMode mode, Date_t deadline) {
        return LockResult::LOCK_OK;
    }

    virtual void downgrade(ResourceId resId, LockMode newMode) {
        MONGO_UNREACHABLE;
    }

    virtual bool unlock(ResourceId resId) {
        return true;
    }

    virtual LockMode getLockMode(ResourceId resId) const {
        MONGO_UNREACHABLE;
    }

    virtual bool isLockHeldForMode(ResourceId resId, LockMode mode) const {
        return true;
    }

    virtual bool isDbLockedForMode(StringData dbName, LockMode mode) const {
        return true;
    }

    virtual bool isCollectionLockedForMode(StringData ns, LockMode mode) const {
        return true;
    }

    virtual ResourceId getWaitingResource() const {
        MONGO_UNREACHABLE;
    }

    virtual void getLockerInfo(LockerInfo* lockerInfo,
                               boost::optional<SingleThreadedLockStats> lockStatsBase) const {
        MONGO_UNREACHABLE;
    }

    virtual boost::optional<LockerInfo> getLockerInfo(
        boost::optional<SingleThreadedLockStats> lockStatsBase) const {
        return boost::none;
    }

    virtual bool saveLockStateAndUnlock(LockSnapshot* stateOut) {
        MONGO_UNREACHABLE;
    }

    virtual void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore) {
        MONGO_UNREACHABLE;
    }
    virtual void restoreLockState(const LockSnapshot& stateToRestore) {
        MONGO_UNREACHABLE;
    }

    bool releaseWriteUnitOfWork(LockSnapshot* stateOut) override {
        MONGO_UNREACHABLE;
    }

    void restoreWriteUnitOfWork(OperationContext* opCtx,
                                const LockSnapshot& stateToRestore) override {
        MONGO_UNREACHABLE;
    };

    virtual void releaseTicket() {
        MONGO_UNREACHABLE;
    }

    virtual void reacquireTicket(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    virtual void dump() const {
        MONGO_UNREACHABLE;
    }

    virtual bool isW() const {
        return true;
    }

    virtual bool isR() const {
        MONGO_UNREACHABLE;
    }

    virtual bool isLocked() const {
        // This is necessary because replication makes decisions based on the answer to this, and
        // we wrote unit tests to test the behavior specifically when this returns "false".
        return false;
    }

    virtual bool isWriteLocked() const {
        return true;
    }

    virtual bool isReadLocked() const {
        return true;
    }

    virtual bool isRSTLExclusive() const {
        return true;
    }

    virtual bool isRSTLLocked() const {
        return true;
    }

    virtual bool hasLockPending() const {
        MONGO_UNREACHABLE;
    }

    bool isGlobalLockedRecursively() override {
        return false;
    }
};

}  // namespace mongo
