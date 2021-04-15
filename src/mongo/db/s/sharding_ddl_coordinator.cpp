/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_coordinator.h"

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

namespace mongo {

ShardingDDLCoordinatorMetadata extractShardingDDLCoordinatorMetadata(const BSONObj& coorDoc) {
    return ShardingDDLCoordinatorMetadata::parse(
        IDLParserErrorContext("ShardingDDLCoordinatorMetadata"), coorDoc);
}

ShardingDDLCoordinator::ShardingDDLCoordinator(const BSONObj& coorDoc)
    : _coorMetadata(extractShardingDDLCoordinatorMetadata(coorDoc)),
      _recoveredFromDisk(_coorMetadata.getRecoveredFromDisk()) {}

ShardingDDLCoordinator::~ShardingDDLCoordinator() {
    invariant(_constructionCompletionPromise.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

void ShardingDDLCoordinator::_removeDocument(OperationContext* opCtx) {
    PersistentTaskStore<ShardingDDLCoordinatorMetadata> store(
        NamespaceString::kShardingDDLCoordinatorsNamespace);
    LOGV2_DEBUG(5565601,
                2,
                "Removing sharding DDL coordinator document",
                "coordinatorId"_attr = _coorMetadata.getId());
    store.remove(
        opCtx,
        BSON(ShardingDDLCoordinatorMetadata::kIdFieldName << _coorMetadata.getId().toBSON()),
        WriteConcerns::kMajorityWriteConcern);
}

void ShardingDDLCoordinator::interrupt(Status status) {
    LOGV2_DEBUG(5390535,
                1,
                "Sharding DDL Coordinator received an interrupt",
                "coordinatorId"_attr = _coorMetadata.getId(),
                "reason"_attr = redact(status));

    _interrupt(status);

    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_constructionCompletionPromise.getFuture().isReady()) {
        _constructionCompletionPromise.setError(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

SemiFuture<void> ShardingDDLCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                             const CancellationToken& token) noexcept {

    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            const auto coorName =
                DDLCoordinatorType_serializer(_coorMetadata.getId().getOperationType());

            auto distLockManager = DistLockManager::get(opCtx);
            auto dbDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, nss().db(), coorName, DistLockManager::kDefaultLockTimeout));
            _scopedLocks.emplace(dbDistLock.moveToAnotherThread());

            if (!nss().isConfigDB() && !_coorMetadata.getRecoveredFromDisk()) {
                invariant(_coorMetadata.getDatabaseVersion());

                OperationShardingState::get(opCtx).initializeClientRoutingVersions(
                    nss(), boost::none /* ChunkVersion */, _coorMetadata.getDatabaseVersion());
                // Check under the dbLock if this is still the primary shard for the database
                DatabaseShardingState::checkIsPrimaryShardForDb(opCtx, nss().db());
            };

            if (!nss().ns().empty()) {
                auto collDistLock = uassertStatusOK(distLockManager->lock(
                    opCtx, nss().ns(), coorName, DistLockManager::kDefaultLockTimeout));
                _scopedLocks.emplace(collDistLock.moveToAnotherThread());
            }

            for (auto& lock : _acquireAdditionalLocks(opCtx)) {
                _scopedLocks.emplace(lock.moveToAnotherThread());
            }

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_constructionCompletionPromise.getFuture().isReady()) {
                _constructionCompletionPromise.emplaceValue();
            }
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            static constexpr auto& errorMsg =
                "Failed to complete construction of sharding DDL coordinator";
            LOGV2_ERROR(5390530,
                        errorMsg,
                        "coordinatorId"_attr = _coorMetadata.getId(),
                        "reason"_attr = redact(status));
            interrupt(status.withContext(errorMsg));
            return status;
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            return _runImpl(executor, token);
        })
        .onCompletion([this, anchor = shared_from_this()](const Status& status) {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            const auto completionStatus = [&] {
                if (!status.isOK() &&
                    (status.isA<ErrorCategory::NotPrimaryError>() ||
                     status.isA<ErrorCategory::ShutdownError>())) {
                    // Do not remove the coordinator document
                    // if we had a stepdown related error.
                    return status;
                }

                try {
                    _removeDocument(opCtx);
                    return status;
                } catch (DBException& ex) {
                    static constexpr auto& errMsg =
                        "Failed to remove sharding DDL coordinator document";
                    LOGV2_WARNING(5565605,
                                  errMsg,
                                  "coordinatorId"_attr = _coorMetadata.getId(),
                                  "error"_attr = redact(ex));
                    return ex.toStatus(errMsg);
                }
            }();

            while (!_scopedLocks.empty()) {
                _scopedLocks.top().assignNewOpCtx(opCtx);
                _scopedLocks.pop();
            }

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.setFrom(completionStatus);
            }

            return completionStatus;
        })
        .semi();
}

ShardingDDLCoordinator_NORESILIENT::ShardingDDLCoordinator_NORESILIENT(OperationContext* opCtx,
                                                                       const NamespaceString& ns)
    : _nss(ns), _forwardableOpMetadata(opCtx) {}

SemiFuture<void> ShardingDDLCoordinator_NORESILIENT::run(OperationContext* opCtx) {
    if (!_nss.isConfigDB()) {
        // Check that the operation context has a database version for this namespace
        const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(_nss.db());
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Request sent without attaching database version",
                clientDbVersion);

        // Checks that this is the primary shard for the namespace's db
        DatabaseShardingState::checkIsPrimaryShardForDb(opCtx, _nss.db());
    }
    return runImpl(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
}

}  // namespace mongo
