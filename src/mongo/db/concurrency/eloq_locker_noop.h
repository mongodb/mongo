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

#include <utility>

#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/util/time_support.h"

namespace mongo {
// Global lock. Every server operation, which uses the Locker must acquire this lock at least
// once. See comments in the header file (begin/endTransaction) for more information.
const ResourceId resourceIdGlobalForEloq =
    ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);


/**
 * Locker, which cannot be used to lock/unlock resources and just returns true for checks for
 * whether a particular resource is locked. Do not use it for cases where actual locking
 * behaviour is expected or locking is performed.
 */
class EloqLockerNoop final : public Locker {
public:
    EloqLockerNoop() = default;

    void reset() override {
        _lockMap.clear();
        _globalLockMode = LockMode::MODE_NONE;
        _recursiveCount = 0;
        _wuowNestingLevel = 0;
    }

    bool isNoop() const override {
        return true;
    }

    ClientState getClientState() const override {
        // Return fake data
        return ClientState::kInactive;
    }

    LockerId getId() const override {
        return {};
    }

    stdx::thread::id getThreadId() const override {
        return {};
    }

    void updateThreadIdToCurrentThread() override {
        // No-op
    }

    void unsetThreadId() override {
        // No-op
    }

    void setSharedLocksShouldTwoPhaseLock(bool sharedLocksShouldTwoPhaseLock) override {
        MONGO_UNREACHABLE;
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

    LockResult lockGlobal(OperationContext* opCtx, LockMode mode) override {
        return lockGlobalBegin(opCtx, mode, Date_t{});
    }

    LockResult lockGlobal(LockMode mode) override {
        return lockGlobal(nullptr, mode);
    }

    LockResult lockBegin(OperationContext* opCtx, const ResourceId& resId, LockMode mode) {
        if (resId == resourceIdGlobalForEloq) {
            _recursiveCount++;
            if (!isModeCovered(mode, _globalLockMode)) {
                _globalLockMode = mode;
            }
        } else {
            if (!isModeCovered(mode, _lockMap[resId])) {
                _lockMap[resId] = mode;
            }
        }
        return LockResult::LOCK_OK;
    }

    LockResult lockGlobalBegin(OperationContext* opCtx, LockMode mode, Date_t deadline) override {
        return lockBegin(opCtx, resourceIdGlobalForEloq, mode);
    }

    LockResult lockGlobalBegin(LockMode mode, Date_t deadline) override {
        return lockGlobalBegin(nullptr, mode, deadline);
    }

    LockResult lockGlobalComplete(OperationContext* opCtx, Date_t deadline) override {
        return LockResult::LOCK_OK;
    }

    LockResult lockGlobalComplete(Date_t deadline) override {
        return LockResult::LOCK_OK;
    }

    void lockMMAPV1Flush() override {}

    bool unlockGlobal() override {
        unlock(resourceIdGlobalForEloq);
        return true;
    }

    void downgradeGlobalXtoSForMMAPV1() override {
        // MONGO_UNREACHABLE;
    }

    void beginWriteUnitOfWork() override {
        _wuowNestingLevel++;
    }

    void endWriteUnitOfWork() override {
        _wuowNestingLevel--;
    }

    bool inAWriteUnitOfWork() const override {
        return _wuowNestingLevel > 0;
    }

    LockResult lock(OperationContext* opCtx,
                    ResourceId resId,
                    LockMode mode,
                    Date_t deadline,
                    bool checkDeadlock) override {
        return lockBegin(opCtx, resId, mode);
    }

    LockResult lock(ResourceId resId, LockMode mode, Date_t deadline, bool checkDeadlock) override {
        return lock(nullptr, resId, mode, deadline, checkDeadlock);
    }

    void downgrade(ResourceId resId, LockMode newMode) override {
        MONGO_UNREACHABLE;
    }

    bool unlock(ResourceId resId) override {
        if (resId == resourceIdGlobalForEloq) {
            _recursiveCount--;
            if (_recursiveCount == 0) {
                _globalLockMode = LockMode::MODE_NONE;
            }
        } else {
            _lockMap[resId] = LockMode::MODE_NONE;
        }
        return true;
    }

    LockMode getLockMode(ResourceId resId) const override {
        if (resId == resourceIdGlobalForEloq) {
            return _globalLockMode;
        } else {
            auto iter = _lockMap.find(resId);
            if (iter == _lockMap.end()) {
                return LockMode::MODE_NONE;
            } else {
                return iter->second;
            }
        }
    }

    bool isLockHeldForMode(ResourceId resId, LockMode mode) const override {
        return isModeCovered(mode, getLockMode(resId));
    }

    bool isDbLockedForMode(StringData dbName, LockMode mode) const override {
        if (isW()) {
            return true;
        }
        if (isR() && isSharedLockMode(mode)) {
            return true;
        }
        return true;
    }

    bool isCollectionLockedForMode(StringData ns, LockMode mode) const override {
        return true;
    }

    ResourceId getWaitingResource() const override {
        MONGO_UNREACHABLE;
    }

    void getLockerInfo(LockerInfo* lockerInfo) const override {
        invariant(lockerInfo);

        // Zero-out the contents
        lockerInfo->locks.clear();
        lockerInfo->waitingResource = ResourceId();
        lockerInfo->stats.reset();
    }

    boost::optional<LockerInfo> getLockerInfo() const override {
        // Return a default-constructed LockerInfo object instead of boost::none.
        return {LockerInfo{}};
    }

    // Refer to LockerImpl<IsForMMAPV1>::saveLockStateAndUnlock
    bool saveLockStateAndUnlock(LockSnapshot* stateOut) override {
        stateOut->locks.clear();
        stateOut->globalMode = LockMode::MODE_NONE;

        // If there's no global lock there isn't really anything to do.
        if (_globalLockMode == LockMode::MODE_NONE) {
            return false;
        }
        // If the global lock has been acquired more than once, we're probably somewhere in a
        // DBDirectClient call.  It's not safe to release and reacquire locks -- the context using
        // the DBDirectClient is probably not prepared for lock release.
        if (_recursiveCount > 1) {
            return false;
        }

        stateOut->globalMode = _globalLockMode;
        unlock(resourceIdGlobalForEloq);
        _recursiveCount = 0;

        for (auto it = _lockMap.begin(); it != _lockMap.end();) {
            const ResourceId resId = it->first;
            const ResourceType resType = resId.getType();
            const LockMode mode = it->second;
            if (resType == RESOURCE_MUTEX) {
                it++;
                continue;
            }

            // We should never have to save and restore metadata locks.
            invariant(RESOURCE_DATABASE == resId.getType() ||
                      RESOURCE_COLLECTION == resId.getType() ||
                      (RESOURCE_GLOBAL == resId.getType() && isSharedLockMode(mode)));

            OneLock info;
            info.resourceId = resId;
            info.mode = mode;

            stateOut->locks.push_back(std::move(info));
            it = _lockMap.erase(it);
        }
        invariant(!isLocked());

        // Sort locks by ResourceId. They'll later be acquired in this canonical locking order.
        std::sort(stateOut->locks.begin(), stateOut->locks.end());

        return true;
    }

    // Refer to LockerImpl<IsForMMAPV1>::restoreLockState
    void restoreLockState(OperationContext* opCtx, const LockSnapshot& stateToRestore) override {
        invariant(LOCK_OK == lockGlobal(opCtx, stateToRestore.globalMode));
        for (const auto& lockState : stateToRestore.locks) {
            invariant(LOCK_OK == lock(lockState.resourceId, lockState.mode, Date_t{}, false));
        }
    }

    void restoreLockState(const LockSnapshot& stateToRestore) override {
        restoreLockState(nullptr, stateToRestore);
    }

    void releaseTicket() override {
        // No-op
    }

    void reacquireTicket(OperationContext* opCtx) override {
        // No-op
    }

    void dump() const override {
        MONGO_UNREACHABLE;
    }

    bool isW() const override {
        return _globalLockMode == LockMode::MODE_X;
    }

    bool isR() const override {
        return _globalLockMode == LockMode::MODE_S;
    }

    bool isLocked() const override {
        // This is necessary because replication makes decisions based on the answer to this, and
        // we wrote unit tests to test the behavior specifically when this returns "false".
        return _globalLockMode != LockMode::MODE_NONE;
    }

    bool isWriteLocked() const override {
        return _globalLockMode == LockMode::MODE_IX;
    }

    bool isReadLocked() const override {
        return _globalLockMode == LockMode::MODE_IS;
    }

    bool hasLockPending() const override {
        MONGO_UNREACHABLE;
        return false;
    }

    bool isGlobalLockedRecursively() override {
        return _recursiveCount > 1;
    }

private:
    std::unordered_map<ResourceId, LockMode> _lockMap;
    LockMode _globalLockMode{LockMode::MODE_NONE};
    int _recursiveCount{0};
    // Delays release of exclusive/intent-exclusive locked resources until the write unit of
    // work completes. Value of 0 means we are not inside a write unit of work.
    int _wuowNestingLevel{0};

    // If this is set, dictates the max number of milliseconds that we will wait for lock
    // acquisition. Effectively resets lock acquisition deadlines to time out sooner. If set to 0,
    // for example, lock attempts will time out immediately if the lock is not immediately
    // available.
    // This variable is and serves no purpose for Eloq.
    boost::optional<Milliseconds> _maxLockTimeout;

    // Indicates whether the client is active reader/writer or is queued.
    // AtomicWord<ClientState> _clientState{kInactive};
};

}  // namespace mongo
