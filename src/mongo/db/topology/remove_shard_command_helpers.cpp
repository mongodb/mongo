// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/topology/remove_shard_command_helpers.h"

#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
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
        // which cannot be nested. Therefore, the VersionContext shouldn't have an OFCV yet.
        invariant(!VersionContext::getDecoration(opCtx).hasOperationFCV());
        coordinatorDoc.setShardingCoordinatorMetadata({{NamespaceString::kConfigsvrShardsNamespace,
                                                        CoordinatorTypeEnum::kRemoveShardCommit}});
        auto service = ShardingCoordinatorService::getService(opCtx);
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
            // which cannot be nested. Therefore, the VersionContext shouldn't have an OFCV yet.
            invariant(!VersionContext::getDecoration(opCtx).hasOperationFCV());
            if (feature_flags::gUseTopologyChangeCoordinators.isEnabled(
                    VersionContext::getDecoration(opCtx), (*fixedFCV)->acquireFCVSnapshot())) {
                return runCoordinatorRemoveShard(opCtx, ddlLock, fixedFCV, shardId);
            } else {
                // We need to check that there are not any coordinators which have been created
                // but have not yet acquired the DDL lock in case we are just after an FCV
                // downgrade. We need to release the DDL lock before waiting for that
                // coordinator to complete, so we throw ConflictingOperationInProgress and retry
                // after waiting.
                uassert(
                    ErrorCodes::ConflictingOperationInProgress,
                    "Post FCV downgrade remove shard must wait for ongoing remove shard "
                    "coordinators to complete before executing",
                    ShardingCoordinatorService::getService(opCtx)->areAllCoordinatorsOfTypeFinished(
                        opCtx, CoordinatorTypeEnum::kRemoveShardCommit));
                fixedFCV.reset();
                return shardingCatalogManager->removeShard(opCtx, shardId);
            }
        } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>& ex) {
            LOGV2(10154101,
                  "Remove shard received retriable error and will be retried",
                  "shardId"_attr = shardId,
                  "error"_attr = redact(ex));

            ShardingCoordinatorService::getService(opCtx)->waitForCoordinatorsOfGivenTypeToComplete(
                opCtx, CoordinatorTypeEnum::kRemoveShardCommit);
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
