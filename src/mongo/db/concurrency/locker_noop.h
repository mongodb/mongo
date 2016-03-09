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

    virtual ClientState getClientState() const {
        invariant(false);
    }

    virtual LockerId getId() const {
        invariant(false);
    }

    virtual LockResult lockGlobal(LockMode mode, unsigned timeoutMs) {
        invariant(false);
    }

    virtual LockResult lockGlobalBegin(LockMode mode) {
        invariant(false);
    }

    virtual LockResult lockGlobalComplete(unsigned timeoutMs) {
        invariant(false);
    }

    virtual void lockMMAPV1Flush() {
        invariant(false);
    }

    virtual bool unlockGlobal() {
        invariant(false);
    }

    virtual void downgradeGlobalXtoSForMMAPV1() {
        invariant(false);
    }

    virtual void beginWriteUnitOfWork() {}

    virtual void endWriteUnitOfWork() {}

    virtual bool inAWriteUnitOfWork() const {
        invariant(false);
    }

    virtual LockResult lock(ResourceId resId,
                            LockMode mode,
                            unsigned timeoutMs,
                            bool checkDeadlock) {
        invariant(false);
    }

    virtual void downgrade(ResourceId resId, LockMode newMode) {
        invariant(false);
    }

    virtual bool unlock(ResourceId resId) {
        invariant(false);
    }

    virtual LockMode getLockMode(ResourceId resId) const {
        invariant(false);
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
        invariant(false);
    }

    virtual void getLockerInfo(LockerInfo* lockerInfo) const {
        invariant(false);
    }

    virtual bool saveLockStateAndUnlock(LockSnapshot* stateOut) {
        invariant(false);
    }

    virtual void restoreLockState(const LockSnapshot& stateToRestore) {
        invariant(false);
    }

    virtual void dump() const {
        invariant(false);
    }

    virtual bool isW() const {
        invariant(false);
    }

    virtual bool isR() const {
        invariant(false);
    }

    virtual bool isLocked() const {
        return false;
    }

    virtual bool isWriteLocked() const {
        return false;
    }

    virtual bool isReadLocked() const {
        invariant(false);
    }

    virtual void assertEmptyAndReset() {
        invariant(false);
    }

    virtual bool hasLockPending() const {
        invariant(false);
    }

    virtual void setIsBatchWriter(bool newValue) {
        invariant(false);
    }

    virtual bool isBatchWriter() const {
        invariant(false);
    }
};

}  // namespace mongo
