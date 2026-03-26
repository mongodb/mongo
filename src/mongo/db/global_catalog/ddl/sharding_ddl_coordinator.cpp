/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"

#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void ShardingDDLCoordinatorMixin::_initializeLockerAndCheckAllowedToStart(ShardingCoordinator& self,
                                                                          OperationContext* opCtx) {
    tassert(10644521, "Expected _locker to be unset", !_locker);
    _locker = std::make_unique<Locker>(opCtx->getServiceContext());
    _locker->unsetThreadId();
    _locker->setDebugInfo(str::stream() << self._coordId.toBSON());

    // Check if this coordinator is allowed to start according to the user-writes blocking
    // critical section. If it is not the first execution, it means it had started already
    // and we are recovering this coordinator. In this case, let it be completed even though
    // new DDL operations may be prohibited now.
    // Coordinators that do not affect user data are allowed to start even when user writes
    // are blocked.
    if (self._firstExecution && !self.canAlwaysStartWhenUserWritesAreDisabled()) {
        self._getExternalState()->checkShardedDDLAllowedToStart(opCtx, self.originalNss());
    }
}

std::set<NamespaceString> ShardingDDLCoordinatorMixin::_getAdditionalLocksToAcquire(
    OperationContext* opCtx) {
    return {};
}

ExecutorFuture<void> ShardingDDLCoordinatorMixin::_acquireAllDDLLocksAsync(
    ShardingCoordinator& self,
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    const NamespaceString& originalNss = self.originalNss();

    // Fetching all the locks that need to be acquired. Sort them by nss to avoid deadlocks.
    // If the requested nss represents a timeseries buckets namespace, translate it to its view
    // nss.
    std::set<NamespaceString> locksToAcquire;
    locksToAcquire.insert(originalNss.isTimeseriesBucketsCollection()
                              ? originalNss.getTimeseriesViewNamespace()
                              : originalNss);

    for (const auto& additionalLock : _getAdditionalLocksToAcquire(opCtx)) {
        locksToAcquire.insert(additionalLock.isTimeseriesBucketsCollection()
                                  ? additionalLock.getTimeseriesViewNamespace()
                                  : additionalLock);
    }

    // Acquiring all DDL locks in sorted order to avoid deadlocks
    // Note that the sorted order is provided by default through the std::set container
    auto futureChain = ExecutorFuture<void>(**executor);
    boost::optional<DatabaseName> lastDb;
    for (const auto& lockNss : locksToAcquire) {
        const bool isDbOnly = lockNss.coll().empty();

        // Acquiring the database DDL lock
        const auto normalizedDbName = [&] {
            if (self._coordId.getOperationType() != CoordinatorTypeEnum::kCreateDatabase) {
                // Already existing databases are not allowed to have their names differ just on
                // case. Uses the requested database name directly.
                return lockNss.dbName();
            }
            const auto dbNameStr =
                DatabaseNameUtil::serialize(lockNss.dbName(), SerializationContext::stateDefault());
            return DatabaseNameUtil::deserialize(
                boost::none, str::toLower(dbNameStr), SerializationContext::stateDefault());
        }();
        if (lastDb != normalizedDbName) {
            const auto lockMode = (isDbOnly ? MODE_X : MODE_IX);
            futureChain =
                std::move(futureChain).then([=, this, &self, anchor = self.shared_from_this()] {
                    return _acquireDDLLockAsync<DatabaseName>(
                        self, executor, token, normalizedDbName, lockMode);
                });
            lastDb = normalizedDbName;
        }

        // Acquiring the collection DDL lock
        if (!isDbOnly) {
            futureChain =
                std::move(futureChain)
                    .then([=, this, &self, nss = lockNss, anchor = self.shared_from_this()] {
                        return _acquireDDLLockAsync<NamespaceString>(
                            self, executor, token, nss, MODE_X);
                    });
        }
    }
    return futureChain;
}

void ShardingDDLCoordinatorMixin::_releaseDDLLocks(OperationContext* opCtx) {
    while (!_scopedLocks.empty()) {
        _scopedLocks.pop();
    }
}

template <typename T>
ExecutorFuture<void> ShardingDDLCoordinatorMixin::_acquireDDLLockAsync(
    ShardingCoordinator& self,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const T& resource,
    LockMode lockMode) {
    return AsyncTry([this, &self, resource, lockMode] {
               auto opCtxHolder = self.makeOperationContext();
               auto* opCtx = opCtxHolder.get();

               const auto coorName = idl::serialize(self._coordId.getOperationType());

               _scopedLocks.emplace(DDLLockManager::ScopedBaseDDLLock{opCtx,
                                                                      _locker.get(),
                                                                      resource,
                                                                      coorName,
                                                                      lockMode,
                                                                      false /* waitForRecovery */});
           })
        .until([this, &self, resource, lockMode](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(6819300,
                              "DDL lock acquisition attempt failed",
                              logv2::DynamicAttributes{
                                  logv2::DynamicAttributes(self.getCoordinatorLogAttrs()),
                                  "resource"_attr = toStringForLogging(resource),
                                  "mode"_attr = modeName(lockMode),
                                  "error"_attr = redact(status)});
            }
            // Coordinators can't generally be rolled back so in case we recovered a coordinator
            // from disk we need to ensure eventual completion of the operation, so we must
            // retry until we manage to acquire the lock.
            return (!self._recoveredFromDisk) || status.isOK();
        })
        .withBackoffBetweenIterations(ShardingCoordinator::kExponentialBackoff)
        .on(**executor, token);
}

}  // namespace mongo
