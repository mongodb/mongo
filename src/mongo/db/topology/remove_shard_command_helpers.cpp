/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/topology/remove_shard_command_helpers.h"

#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/s/replica_set_endpoint_feature_flag.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/topology/remove_shard_commit_coordinator.h"
#include "mongo/db/topology/remove_shard_commit_coordinator_document_gen.h"
#include "mongo/logv2/log.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace topology_change_helpers {

RemoveShardProgress runCoordinatorRemoveShard(
    OperationContext* opCtx,
    boost::optional<DDLLockManager::ScopedCollectionDDLLock>& ddlLock,
    boost::optional<FixedFCVRegion>& fcvRegion,
    const ShardId& shardId) {
    invariant(ddlLock);
    invariant(fcvRegion);

    const auto removeShardCommitCoordinator = [&] {
        auto coordinatorDoc = RemoveShardCommitCoordinatorDocument();
        coordinatorDoc.setShardId(shardId);
        coordinatorDoc.setIsTransitionToDedicated(shardId == ShardId::kConfigServerId);
        // The Operation FCV is currently propagated only for DDL operations,
        // which cannot be nested. Therefore, the VersionContext shouldn't have
        // been initialized yet.
        invariant(!VersionContext::getDecoration(opCtx).isInitialized());
        coordinatorDoc.setShouldUpdateClusterCardinality(
            replica_set_endpoint::isFeatureFlagEnabled(VersionContext::getDecoration(opCtx)));
        coordinatorDoc.setShardingDDLCoordinatorMetadata(
            {{NamespaceString::kConfigsvrShardsNamespace,
              DDLCoordinatorTypeEnum::kRemoveShardCommit}});
        auto service = ShardingDDLCoordinatorService::getService(opCtx);
        auto coordinator = checked_pointer_cast<RemoveShardCommitCoordinator>(
            service->getOrCreateInstance(opCtx, coordinatorDoc.toBSON(), *fcvRegion));
        return coordinator;
    }();
    fcvRegion.reset();
    ddlLock.reset();

    const auto& drainingStatus = [&]() -> RemoveShardProgress {
        try {
            auto drainingStatus = removeShardCommitCoordinator->getResult(opCtx);
            return drainingStatus;
        } catch (const ExceptionFor<ErrorCodes::RemoveShardDrainingInProgress>& ex) {
            const auto removeShardProgress = ex.extraInfo<RemoveShardDrainingInfo>();
            tassert(
                1003142, "RemoveShardDrainingInProgress must have extra info", removeShardProgress);
            return removeShardProgress->getProgress();
        }
    }();
    return drainingStatus;
}

RemoveShardProgress removeShard(OperationContext* opCtx, const ShardId& shardId) {
    const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);
    while (true) {
        try {
            {
                DDLLockManager::ScopedCollectionDDLLock ddlLock(
                    opCtx,
                    NamespaceString::kConfigsvrShardsNamespace,
                    "startShardDraining",
                    LockMode::MODE_X);

                if (auto drainingStatus =
                        topology_change_helpers::startShardDraining(opCtx, shardId, ddlLock)) {
                    return *drainingStatus;
                }
            }
            auto drainingStatus = shardingCatalogManager->checkDrainingProgress(opCtx, shardId);
            if (drainingStatus.getState() != ShardDrainingStateEnum::kDrainingComplete) {
                return drainingStatus;
            }

            boost::optional<DDLLockManager::ScopedCollectionDDLLock> ddlLock{
                boost::in_place_init,
                opCtx,
                NamespaceString::kConfigsvrShardsNamespace,
                "removeShard",
                LockMode::MODE_X};
            boost::optional<FixedFCVRegion> fixedFCV{boost::in_place_init, opCtx};
            // The Operation FCV is currently propagated only for DDL operations,
            // which cannot be nested. Therefore, the VersionContext shouldn't have
            // been initialized yet.
            invariant(!VersionContext::getDecoration(opCtx).isInitialized());
            if (feature_flags::gUseTopologyChangeCoordinators.isEnabled(
                    VersionContext::getDecoration(opCtx), (*fixedFCV)->acquireFCVSnapshot())) {
                return runCoordinatorRemoveShard(opCtx, ddlLock, fixedFCV, shardId);
            } else {
                // We need to check that there are not any coordinators which have been created
                // but have not yet acquired the DDL lock in case we are just after an FCV
                // downgrade. We need to release the DDL lock before waiting for that
                // coordinator to complete, so we throw ConflictingOperationInProgress and retry
                // after waiting.
                uassert(ErrorCodes::ConflictingOperationInProgress,
                        "Post FCV downgrade remove shard must wait for ongoing remove shard "
                        "coordinators to complete before executing",
                        ShardingDDLCoordinatorService::getService(opCtx)
                            ->areAllCoordinatorsOfTypeFinished(
                                opCtx, DDLCoordinatorTypeEnum::kRemoveShardCommit));
                fixedFCV.reset();
                return shardingCatalogManager->removeShard(opCtx, shardId);
            }
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
            LOGV2(10154101,
                  "Remove shard received retriable error and will be retried",
                  "shardId"_attr = shardId,
                  "error"_attr = redact(ex));

            ShardingDDLCoordinatorService::getService(opCtx)
                ->waitForCoordinatorsOfGivenTypeToComplete(
                    opCtx, DDLCoordinatorTypeEnum::kRemoveShardCommit);
        }
    }
}

boost::optional<RemoveShardProgress> startShardDraining(
    OperationContext* opCtx,
    const ShardId& shardId,
    DDLLockManager::ScopedCollectionDDLLock& ddlLock) {
    const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);

    return shardingCatalogManager->checkPreconditionsAndStartDrain(opCtx, shardId);
}

void stopShardDraining(OperationContext* opCtx, const ShardId& shardId) {
    const auto shardingCatalogManager = ShardingCatalogManager::get(opCtx);

    DDLLockManager::ScopedCollectionDDLLock ddlLock(
        opCtx, NamespaceString::kConfigsvrShardsNamespace, "stopShardDraining", LockMode::MODE_X);

    shardingCatalogManager->stopDrain(opCtx, shardId);
}

}  // namespace topology_change_helpers
}  // namespace mongo
