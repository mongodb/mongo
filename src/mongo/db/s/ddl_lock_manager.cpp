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


#include "mongo/db/s/ddl_lock_manager.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

// TODO SERVER-68551: Remove once 7.0 becomes last-lts
MONGO_FAIL_POINT_DEFINE(disableReplSetDistLockManager);

const auto ddlLockManagerDecorator = ServiceContext::declareDecoration<DDLLockManager>();

}  // namespace

const Minutes DDLLockManager::kDefaultLockTimeout(5);
const Milliseconds DDLLockManager::kSingleLockAttemptTimeout(0);

DDLLockManager* DDLLockManager::get(ServiceContext* service) {
    return &ddlLockManagerDecorator(service);
}

DDLLockManager* DDLLockManager::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void DDLLockManager::setState(const State& state) {
    stdx::unique_lock<Latch> lock(_mutex);
    _state = state;
    _stateCV.notify_all();
}

void DDLLockManager::_lock(OperationContext* opCtx,
                           StringData ns,
                           const ResourceId& resId,
                           StringData reason,
                           LockMode mode,
                           Milliseconds timeout,
                           bool waitForRecovery) {
    // TODO SERVER-77421 remove invariant
    invariant(mode == MODE_X, "DDL lock modes other than exclusive are not supported yet");

    stdx::unique_lock<Latch> lock(_mutex);

    // Wait for primary and DDL recovered state
    Timer waitingRecoveryTimer;
    if (!opCtx->waitForConditionOrInterruptFor(_stateCV, lock, timeout, [&] {
            return _state == State::kPrimaryAndRecovered || !waitForRecovery;
        })) {
        using namespace fmt::literals;
        uasserted(
            ErrorCodes::LockTimeout,
            "Failed to acquire DDL lock for namespace '{}' after {} with reason '{}' while "
            "waiting recovery of DDLCoordinatorService"_format(ns, timeout.toString(), reason));
    }

    // Subtracting from timeout the time invested on waiting for recovery
    timeout = [&] {
        const auto waitingRecoveryTime = Milliseconds(waitingRecoveryTimer.millis());
        if (timeout.compare(waitingRecoveryTime) < 0) {
            return Milliseconds::zero();
        }
        return timeout - waitingRecoveryTime;
    }();

    auto iter = _inProgressMap.find(ns);

    if (iter == _inProgressMap.end()) {
        _inProgressMap.try_emplace(ns, std::make_shared<NSLock>(reason));
    } else {
        auto nsLock = iter->second;
        nsLock->numWaiting++;
        ScopeGuard guard([&] { nsLock->numWaiting--; });
        if (!opCtx->waitForConditionOrInterruptFor(
                nsLock->cvLocked, lock, timeout, [nsLock]() { return !nsLock->isInProgress; })) {
            using namespace fmt::literals;
            uasserted(
                ErrorCodes::LockBusy,
                "Failed to acquire DDL lock for namespace '{}' after {} that is currently locked with reason '{}'"_format(
                    ns, timeout.toString(), nsLock->reason));
        }
        guard.dismiss();
        nsLock->reason = reason.toString();
        nsLock->isInProgress = true;
    }

    LOGV2(6855301, "Acquired DDL lock", "resource"_attr = ns, "reason"_attr = reason);

    // TODO SERVER-77421 Use resId variable or remove it
    (void)resId;
}

void DDLLockManager::_unlock(StringData ns, const ResourceId& resId, StringData reason) {
    stdx::unique_lock<Latch> lock(_mutex);
    auto iter = _inProgressMap.find(ns);

    iter->second->numWaiting--;
    iter->second->reason.clear();
    iter->second->isInProgress = false;
    iter->second->cvLocked.notify_one();

    if (iter->second->numWaiting == 0) {
        _inProgressMap.erase(ns);
    }
    LOGV2(6855302, "Released DDL lock", "resource"_attr = ns, "reason"_attr = reason);

    // TODO SERVER-77421 Use resId variable or remove it
    (void)resId;
}

DDLLockManager::ScopedDatabaseDDLLock::ScopedDatabaseDDLLock(OperationContext* opCtx,
                                                             const DatabaseName& db,
                                                             StringData reason,
                                                             LockMode mode,
                                                             Milliseconds timeout)
    : DDLLockManager::ScopedBaseDDLLock(
          opCtx, db, reason, mode, timeout, true /*waitForRecovery*/) {

    // Check under the DDL dbLock if this is still the primary shard for the database
    DatabaseShardingState::assertIsPrimaryShardForDb(opCtx, db);
}

DDLLockManager::ScopedCollectionDDLLock::ScopedCollectionDDLLock(OperationContext* opCtx,
                                                                 const NamespaceString& ns,
                                                                 StringData reason,
                                                                 LockMode mode,
                                                                 Milliseconds timeout)
    : DDLLockManager::ScopedBaseDDLLock(
          opCtx, ns, reason, mode, timeout, true /*waitForRecovery*/) {}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     StringData resName,
                                                     const ResourceId& resId,
                                                     StringData reason,
                                                     LockMode mode,
                                                     Milliseconds timeout,
                                                     bool waitForRecovery)
    : _resourceName(resName.toString()),
      _resourceId(resId),
      _reason(reason.toString()),
      _mode(mode),
      _lockManager(DDLLockManager::get(opCtx)) {

    invariant(_lockManager);
    _lockManager->_lock(
        opCtx, _resourceName, _resourceId, _reason, _mode, timeout, waitForRecovery);
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     const NamespaceString& ns,
                                                     StringData reason,
                                                     LockMode mode,
                                                     Milliseconds timeout,
                                                     bool waitForRecovery)
    : ScopedBaseDDLLock(opCtx,
                        NamespaceStringUtil::serialize(ns),
                        ResourceId{RESOURCE_DDL_COLLECTION, NamespaceStringUtil::serialize(ns)},
                        reason,
                        mode,
                        timeout,
                        waitForRecovery) {}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     const DatabaseName& db,
                                                     StringData reason,
                                                     LockMode mode,
                                                     Milliseconds timeout,
                                                     bool waitForRecovery)
    : ScopedBaseDDLLock(opCtx,
                        DatabaseNameUtil::serialize(db),
                        ResourceId{RESOURCE_DDL_DATABASE, DatabaseNameUtil::serialize(db)},
                        reason,
                        mode,
                        timeout,
                        waitForRecovery) {}

DDLLockManager::ScopedBaseDDLLock::~ScopedBaseDDLLock() {
    if (_lockManager) {
        _lockManager->_unlock(_resourceName, _resourceId, _reason);
    }
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(ScopedBaseDDLLock&& other)
    : _resourceName(std::move(other._resourceName)),
      _resourceId(std::move(other._resourceId)),
      _reason(std::move(other._reason)),
      _mode(std::move(other._mode)),
      _lockManager(other._lockManager) {
    other._lockManager = nullptr;
}

}  // namespace mongo
