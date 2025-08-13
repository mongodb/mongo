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


#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/lock_manager/resource_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/database_sharding_state.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <cstdlib>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const auto ddlLockManagerDecorator = ServiceContext::declareDecoration<DDLLockManager>();

MONGO_FAIL_POINT_DEFINE(overrideDDLLockTimeout);

}  // namespace

const Minutes DDLLockManager::ScopedBaseDDLLock::kDefaultLockTimeout(5);
const Milliseconds DDLLockManager::kSingleLockAttemptTimeout(25);

DDLLockManager* DDLLockManager::get(ServiceContext* service) {
    return &ddlLockManagerDecorator(service);
}

DDLLockManager* DDLLockManager::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void DDLLockManager::setRecoverable(Recoverable* recoverable) {
    _recoverable = recoverable;
}

void DDLLockManager::_lock(OperationContext* opCtx,
                           Locker* locker,
                           StringData ns,
                           const ResourceId& resId,
                           StringData reason,
                           LockMode mode,
                           Date_t deadline,
                           bool waitForRecovery) {
    tassert(9037198,
            "Operation context must be interruptable on stepup/stepdown",
            opCtx->shouldAlwaysInterruptAtStepDownOrUp());

    Timer waitingTime;
    if (waitForRecovery) {
        tassert(9037199,
                fmt::format(
                    "Failed to wait for recovery on acquire DDL lock for namespace '{}' in mode {} "
                    "with reason '{}': No recoverable object set",
                    ns,
                    modeName(mode),
                    reason),
                _recoverable);
        try {
            opCtx->runWithDeadline(
                deadline, opCtx->getTimeoutError(), [_recoverable = this->_recoverable, &opCtx]() {
                    _recoverable->waitForRecovery(opCtx);
                });
        } catch (const ExceptionFor<ErrorCategory::ExceededTimeLimitError>&) {
            uasserted(ErrorCodes::LockTimeout,
                      fmt::format(
                          "Failed to acquire DDL lock for namespace '{}' in mode {} after {} with "
                          "reason '{}' while waiting recovery of DDLCoordinatorService",
                          ns,
                          modeName(mode),
                          waitingTime.elapsed().toString(),
                          reason));
        }
    }

    tassert(7742100,
            "No hierarchy lock (Global/DB/Coll) must be held when acquiring a DDL lock outside"
            "a transaction (transactions hold at least the global lock in IX mode)",
            opCtx->inMultiDocumentTransaction() || !locker->isLocked());

    _registerResourceName(resId, ns);

    ScopeGuard unregisterResourceExitGuard(
        [&] { _unregisterResourceNameIfNoLongerNeeded(resId, ns); });

    if (locker->getDebugInfo().empty()) {
        locker->setDebugInfo(std::string{reason});
    }

    try {
        locker->lock(opCtx, resId, mode, deadline);
    } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {

        const auto& lockHolders = locker->getLockInfoFromResourceHolders(resId);
        std::stringstream lockHoldersDebugInfo;
        lockHoldersDebugInfo << "Failed to acquire DDL lock for '" << std::string{ns}
                             << "' in mode " << modeName(mode) << " after "
                             << duration_cast<Milliseconds>(waitingTime.elapsed()).toString()
                             << " that is currently locked by '[";
        for (std::size_t i = 0; i < lockHolders.size(); i++) {
            const auto& lock = lockHolders[i];
            if (i > 0)
                lockHoldersDebugInfo << ", ";
            lockHoldersDebugInfo << "{ operation: " << lock.debugInfo << ", "
                                 << "lock mode: " << modeName(lock.mode) << " }";
        }
        lockHoldersDebugInfo << "]'";
        uasserted(ErrorCodes::LockBusy, lockHoldersDebugInfo.str());
    }

    unregisterResourceExitGuard.dismiss();

    // TODO SERVER-99655, SERVER-99552: update once gSnapshotFCVInDDLCoordinators is enabled on the
    // lastLTS and the OFCV is snapshotted for DDLs that do not pass by coordinators.
    logv2::DynamicAttributes attrs;
    attrs.add("resource", ns);
    attrs.add("reason", reason);
    attrs.add("mode", modeName(mode));
    if (auto& vCtx = VersionContext::getDecoration(opCtx); vCtx.isInitialized()) {
        attrs.add("versionContext", vCtx);
    }
    LOGV2(6855301, "Acquired DDL lock", attrs);
}

void DDLLockManager::_unlock(
    Locker* locker, StringData ns, const ResourceId& resId, StringData reason, LockMode mode) {
    dassert(locker);
    locker->unlock(resId);

    _unregisterResourceNameIfNoLongerNeeded(resId, ns);

    LOGV2(6855302,
          "Released DDL lock",
          "resource"_attr = ns,
          "reason"_attr = reason,
          "mode"_attr = modeName(mode));
}

void DDLLockManager::_registerResourceName(ResourceId resId, StringData resName) {
    stdx::lock_guard<stdx::mutex> guard{_mutex};
    const auto currentNumHolders = _numHoldersPerResource[resId]++;
    if (currentNumHolders == 0) {
        ResourceCatalog::get().add(resId, DDLResourceName(resName));
    }
}

void DDLLockManager::_unregisterResourceNameIfNoLongerNeeded(ResourceId resId, StringData resName) {
    stdx::lock_guard<stdx::mutex> guard{_mutex};
    const auto currentNumHolders = --_numHoldersPerResource[resId];
    if (currentNumHolders <= 0) {
        _numHoldersPerResource.erase(resId);
        ResourceCatalog::get().remove(resId, DDLResourceName(resName));
    }
}


DDLLockManager::ScopedDatabaseDDLLock::ScopedDatabaseDDLLock(
    OperationContext* opCtx,
    const DatabaseName& db,
    StringData reason,
    LockMode mode,
    boost::optional<BackoffStrategy&> backoffStrategy) {
    if (backoffStrategy) {
        if (_tryLock(opCtx, db, reason, mode, *backoffStrategy)) {
            return;
        }
    }
    _lock(opCtx, db, reason, mode, boost::none);
}

bool DDLLockManager::ScopedDatabaseDDLLock::_tryLock(OperationContext* opCtx,
                                                     const DatabaseName& db,
                                                     StringData reason,
                                                     LockMode mode,
                                                     BackoffStrategy& backoffStrategy) {
    return backoffStrategy.execute(
        [&]() {
            try {
                _lock(opCtx, db, reason, mode, kSingleLockAttemptTimeout);
            } catch (const DBException& ex) {
                if (ex.code() != ErrorCodes::LockTimeout && ex.code() != ErrorCodes::LockBusy) {
                    throw;
                }
                return false;
            }
            return true;
        },
        [&](Milliseconds millis) { opCtx->sleepFor(millis); });
}

void DDLLockManager::ScopedDatabaseDDLLock::_lock(OperationContext* opCtx,
                                                  const DatabaseName& db,
                                                  StringData reason,
                                                  LockMode mode,
                                                  boost::optional<Milliseconds> timeout) {
    try {
        _dbLock.emplace(opCtx,
                        shard_role_details::getLocker(opCtx),
                        db,
                        reason,
                        mode,
                        true /*waitForRecovery*/,
                        timeout);

        // Check under the DDL dbLock if this is the primary shard for the database
        const auto scopedDss = DatabaseShardingState::acquire(opCtx, db);
        scopedDss->assertIsPrimaryShardForDb(opCtx);
    } catch (...) {
        _dbLock.reset();
        throw;
    }
}

DDLLockManager::ScopedCollectionDDLLock::ScopedCollectionDDLLock(
    OperationContext* opCtx,
    const NamespaceString& ns,
    StringData reason,
    LockMode mode,
    boost::optional<BackoffStrategy&> backoffStrategy) {
    if (backoffStrategy) {
        if (_tryLock(opCtx, ns, reason, mode, *backoffStrategy)) {
            return;
        }
    }
    _lock(opCtx, ns, reason, mode, boost::none);
}

bool DDLLockManager::ScopedCollectionDDLLock::_tryLock(OperationContext* opCtx,
                                                       const NamespaceString& ns,
                                                       StringData reason,
                                                       LockMode mode,
                                                       BackoffStrategy& backoffStrategy) {
    return backoffStrategy.execute(
        [&]() {
            try {
                _lock(opCtx, ns, reason, mode, kSingleLockAttemptTimeout);
            } catch (const DBException& ex) {
                if (ex.code() != ErrorCodes::LockTimeout && ex.code() != ErrorCodes::LockBusy) {
                    throw;
                }
                return false;
            }
            return true;
        },
        [&](Milliseconds millis) { opCtx->sleepFor(millis); });
}

void DDLLockManager::ScopedCollectionDDLLock::_lock(OperationContext* opCtx,
                                                    const NamespaceString& ns,
                                                    StringData reason,
                                                    LockMode mode,
                                                    boost::optional<Milliseconds> timeout) {
    try {
        _dbLock.emplace(opCtx,
                        shard_role_details::getLocker(opCtx),
                        ns.dbName(),
                        reason,
                        isSharedLockMode(mode) ? MODE_IS : MODE_IX,
                        true /*waitForRecovery*/,
                        timeout);

        // Check under the DDL db lock if this is the primary shard for the database
        {
            const auto scopedDss = DatabaseShardingState::acquire(opCtx, ns.dbName());
            scopedDss->assertIsPrimaryShardForDb(opCtx);
        }

        // Acquire the collection DDL lock.
        // If the ns represents a timeseries buckets collection, translate to its corresponding
        // view ns.
        _collLock.emplace(opCtx,
                          shard_role_details::getLocker(opCtx),
                          ns.isTimeseriesBucketsCollection() ? ns.getTimeseriesViewNamespace() : ns,
                          reason,
                          mode,
                          true /*waitForRecovery*/,
                          timeout);
    } catch (...) {
        _dbLock.reset();
        _collLock.reset();
        throw;
    }
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     Locker* locker,
                                                     StringData resName,
                                                     const ResourceId& resId,
                                                     StringData reason,
                                                     LockMode mode,
                                                     bool waitForRecovery,
                                                     Milliseconds timeout)
    : _resourceName(std::string{resName}),
      _resourceId(resId),
      _reason(std::string{reason}),
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
                        Date_t::now() + timeout,
                        waitForRecovery);
    _result = LockResult::LOCK_OK;
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     Locker* locker,
                                                     const NamespaceString& nss,
                                                     StringData reason,
                                                     LockMode mode,
                                                     bool waitForRecovery,
                                                     boost::optional<Milliseconds> timeout)
    : ScopedBaseDDLLock(opCtx,
                        locker,
                        NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()),
                        ResourceId{RESOURCE_DDL_COLLECTION, nss},
                        reason,
                        mode,
                        waitForRecovery,
                        timeout.value_or(_getTimeout())) {}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(OperationContext* opCtx,
                                                     Locker* locker,
                                                     const DatabaseName& db,
                                                     StringData reason,
                                                     LockMode mode,
                                                     bool waitForRecovery,
                                                     boost::optional<Milliseconds> timeout)
    : ScopedBaseDDLLock(opCtx,
                        locker,
                        DatabaseNameUtil::serialize(db, SerializationContext::stateDefault()),
                        ResourceId{RESOURCE_DDL_DATABASE, db},
                        reason,
                        mode,
                        waitForRecovery,
                        timeout.value_or(_getTimeout())) {}

DDLLockManager::ScopedBaseDDLLock::~ScopedBaseDDLLock() {
    if (_lockManager && _result == LockResult::LOCK_OK) {
        _lockManager->_unlock(_locker, _resourceName, _resourceId, _reason, _mode);
    }
}

DDLLockManager::ScopedBaseDDLLock::ScopedBaseDDLLock(ScopedBaseDDLLock&& other)
    : _resourceName(std::move(other._resourceName)),
      _resourceId(other._resourceId),
      _reason(std::move(other._reason)),
      _mode(other._mode),
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
