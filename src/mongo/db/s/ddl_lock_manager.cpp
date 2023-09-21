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

#include <mutex>
#include <ratio>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "mongo/base/error_codes.h"
#include "mongo/db/concurrency/resource_catalog.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

MONGO_FAIL_POINT_DEFINE(overrideDDLLockTimeout);

namespace mongo {
namespace {

const auto ddlLockManagerDecorator = ServiceContext::declareDecoration<DDLLockManager>();

}  // namespace

const Minutes DDLLockManager::ScopedBaseDDLLock::kDefaultLockTimeout(5);
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
                           Locker* locker,
                           StringData ns,
                           const ResourceId& resId,
                           StringData reason,
                           LockMode mode,
                           Date_t deadline,
                           bool waitForRecovery) {
    Timer waitingTime;

    {
        stdx::unique_lock<Latch> lock{_mutex};
        // Wait for primary and DDL recovered state
        if (!opCtx->waitForConditionOrInterruptUntil(_stateCV, lock, deadline, [&] {
                return _state == State::kPrimaryAndRecovered || !waitForRecovery;
            })) {
            using namespace fmt::literals;
            uasserted(
                ErrorCodes::LockTimeout,
                "Failed to acquire DDL lock for namespace '{}' in mode {} after {} with reason "
                "'{}' while waiting recovery of DDLCoordinatorService"_format(
                    ns, modeName(mode), waitingTime.elapsed().toString(), reason));
        }

        tassert(7742100,
                "None hierarchy lock (Global/DB/Coll) must be hold when acquiring a DDL lock",
                !locker->isLocked());

        _registerResourceName(lock, resId, ns);
    }

    ScopeGuard unregisterResourceExitGuard([&] {
        stdx::unique_lock<Latch> lock(_mutex);
        _unregisterResourceNameIfNoLongerNeeded(lock, resId, ns);
    });

    if (locker->getDebugInfo().empty()) {
        locker->setDebugInfo(reason.toString());
    }

    try {
        locker->lock(opCtx, resId, mode, deadline);
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
        using namespace fmt::literals;

        std::vector<std::string> lockHoldersArr;
        const auto& lockHolders = locker->getLockInfoFromResourceHolders(resId);
        for (const auto& lock : lockHolders) {
            lockHoldersArr.emplace_back(
                BSON("operation" << lock.debugInfo << "lock mode" << modeName(lock.mode))
                    .toString());
        }

        uasserted(
            ErrorCodes::LockBusy,
            "Failed to acquire DDL lock for '{}' in mode {} after {} that is currently locked by '{}'"_format(
                ns,
                modeName(mode),
                duration_cast<Milliseconds>(waitingTime.elapsed()).toString(),
                lockHoldersArr));

    } catch (DBException& e) {
        e.addContext("Failed to acquire DDL lock for '{}' in mode {} after {}"_format(
            ns, modeName(mode), duration_cast<Milliseconds>(waitingTime.elapsed()).toString()));
        throw;
    }

    unregisterResourceExitGuard.dismiss();

    LOGV2(6855301,
          "Acquired DDL lock",
          "resource"_attr = ns,
          "reason"_attr = reason,
          "mode"_attr = modeName(mode));
}

void DDLLockManager::_unlock(
    Locker* locker, StringData ns, const ResourceId& resId, StringData reason, LockMode mode) {
    dassert(locker);
    locker->unlock(resId);

    stdx::unique_lock<Latch> lock(_mutex);
    _unregisterResourceNameIfNoLongerNeeded(lock, resId, ns);

    LOGV2(6855302,
          "Released DDL lock",
          "resource"_attr = ns,
          "reason"_attr = reason,
          "mode"_attr = modeName(mode));
}

void DDLLockManager::_registerResourceName(WithLock lk, ResourceId resId, StringData resName) {
    const auto currentNumHolders = _numHoldersPerResource[resId]++;
    if (currentNumHolders == 0) {
        ResourceCatalog::get().add(resId, DDLResourceName(resName));
    }
}

void DDLLockManager::_unregisterResourceNameIfNoLongerNeeded(WithLock lk,
                                                             ResourceId resId,
                                                             StringData resName) {
    const auto currentNumHolders = --_numHoldersPerResource[resId];
    if (currentNumHolders <= 0) {
        _numHoldersPerResource.erase(resId);
        ResourceCatalog::get().remove(resId, DDLResourceName(resName));
    }
}


DDLLockManager::ScopedDatabaseDDLLock::ScopedDatabaseDDLLock(OperationContext* opCtx,
                                                             const DatabaseName& db,
                                                             StringData reason,
                                                             LockMode mode)
    : DDLLockManager::ScopedBaseDDLLock(
          opCtx, opCtx->lockState(), db, reason, mode, true /*waitForRecovery*/) {

    // Check under the DDL dbLock if this is the primary shard for the database
    DatabaseShardingState::assertIsPrimaryShardForDb(opCtx, db);
}

DDLLockManager::ScopedCollectionDDLLock::ScopedCollectionDDLLock(OperationContext* opCtx,
                                                                 const NamespaceString& ns,
                                                                 StringData reason,
                                                                 LockMode mode) {
    // Acquire implicitly the db DDL lock
    _dbLock.emplace(opCtx,
                    opCtx->lockState(),
                    ns.dbName(),
                    reason,
                    isSharedLockMode(mode) ? MODE_IS : MODE_IX,
                    true /*waitForRecovery*/);

    // Check under the DDL db lock if this is the primary shard for the database
    DatabaseShardingState::assertIsPrimaryShardForDb(opCtx, ns.dbName());

    // Finally, acquire the collection DDL lock
    _collLock.emplace(opCtx, opCtx->lockState(), ns, reason, mode, true /*waitForRecovery*/);
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     Locker* locker,
                                                     StringData resName,
                                                     const ResourceId& resId,
                                                     StringData reason,
                                                     LockMode mode,
                                                     bool waitForRecovery)
    : _resourceName(resName.toString()),
      _resourceId(resId),
      _reason(reason.toString()),
      _mode(mode),
      _result(LockResult::LOCK_INVALID),
      _locker(locker),
      _lockManager(DDLLockManager::get(opCtx)) {

    invariant(_lockManager);
    _lockManager->_lock(opCtx,
                        _locker,
                        _resourceName,
                        _resourceId,
                        _reason,
                        _mode,
                        Date_t::now() + _getTimeout(),
                        waitForRecovery);
    _result = LockResult::LOCK_OK;
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     Locker* locker,
                                                     const NamespaceString& ns,
                                                     StringData reason,
                                                     LockMode mode,
                                                     bool waitForRecovery)
    : ScopedBaseDDLLock(
          opCtx,
          locker,
          NamespaceStringUtil::serialize(ns, SerializationContext::stateDefault()),
          ResourceId{RESOURCE_DDL_COLLECTION,
                     NamespaceStringUtil::serialize(ns, SerializationContext::stateDefault())},
          reason,
          mode,
          waitForRecovery) {}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     Locker* locker,
                                                     const DatabaseName& db,
                                                     StringData reason,
                                                     LockMode mode,
                                                     bool waitForRecovery)
    : ScopedBaseDDLLock(opCtx,
                        locker,
                        DatabaseNameUtil::serialize(db, SerializationContext::stateDefault()),
                        ResourceId{RESOURCE_DDL_DATABASE, db},
                        reason,
                        mode,
                        waitForRecovery) {}

DDLLockManager::ScopedBaseDDLLock::~ScopedBaseDDLLock() {
    if (_lockManager && _result == LockResult::LOCK_OK) {
        _lockManager->_unlock(_locker, _resourceName, _resourceId, _reason, _mode);
    }
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(ScopedBaseDDLLock&& other)
    : _resourceName(std::move(other._resourceName)),
      _resourceId(std::move(other._resourceId)),
      _reason(std::move(other._reason)),
      _mode(std::move(other._mode)),
      _result(std::move(other._result)),
      _locker(other._locker),
      _lockManager(other._lockManager) {
    other._locker = nullptr;
    other._lockManager = nullptr;
}

Milliseconds DDLLockManager::ScopedBaseDDLLock::_getTimeout() {
    if (auto sfp = overrideDDLLockTimeout.scoped(); MONGO_unlikely(sfp.isActive())) {
        if (auto timeoutElem = sfp.getData()["timeoutMillisecs"]; timeoutElem.ok()) {
            const auto timeoutMillisecs = Milliseconds(timeoutElem.safeNumberLong());
            LOGV2(6320700, "Overriding DDL lock timeout", "timeout"_attr = timeoutMillisecs);
            return timeoutMillisecs;
        }
    }
    return kDefaultLockTimeout;
}

}  // namespace mongo
