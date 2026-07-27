// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state.h"

#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/s/primary_only_service_helpers/all_shards_and_config_causality_barrier.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/topology/user_write_block/global_user_write_block_state.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"

namespace mongo {

void ShardingCoordinatorExternalStateImpl::checkShardedDDLAllowedToStart(
    OperationContext* opCtx, const NamespaceString& nss) const {
    GlobalUserWriteBlockState::get(opCtx)->checkShardedDDLAllowedToStart(opCtx, nss);
}

void ShardingCoordinatorExternalStateImpl::waitForVectorClockDurable(
    OperationContext* opCtx) const {
    VectorClockMutable::get(opCtx)->waitForDurable().get(opCtx);
}

void ShardingCoordinatorExternalStateImpl::assertIsPrimaryShardForDb(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    const auto scopedDss = DatabaseShardingState::acquire(opCtx, dbName);
    scopedDss->assertIsPrimaryShardForDb(opCtx);
}

bool ShardingCoordinatorExternalStateImpl::isTrackedTimeseries(
    OperationContext* opCtx, const NamespaceString& bucketNss) const {
    return sharding_util::isTrackedTimeseries(opCtx, bucketNss);
}

void ShardingCoordinatorExternalStateImpl::allowMigrations(
    OperationContext* opCtx,
    const NamespaceString& nss,
    bool allowMigrations,
    std::function<OperationSessionInfo()> osiGetter,
    AuthoritativeMetadataAccessLevelEnum authoritativeState) {
    if (allowMigrations) {
        sharding_ddl_util::resumeMigrations(opCtx, nss, boost::none, osiGetter, authoritativeState);
    } else {
        sharding_ddl_util::stopMigrations(opCtx, nss, boost::none, osiGetter, authoritativeState);
    }
}

bool ShardingCoordinatorExternalStateImpl::checkAllowMigrationsOnConfigServer(
    OperationContext* opCtx, const NamespaceString& nss) {
    return sharding_ddl_util::checkAllowMigrationsOnConfigServer(opCtx, nss);
}

std::unique_ptr<CausalityBarrier> ShardingCoordinatorExternalStateImpl::makeCausalityBarrier(
    std::shared_ptr<executor::TaskExecutor> executor, CancellationToken token) {
    return std::make_unique<AllShardsAndConfigCausalityBarrier>(std::move(executor),
                                                                std::move(token));
}

std::shared_ptr<ShardingCoordinatorExternalState>
ShardingCoordinatorExternalStateFactoryImpl::create() const {
    return std::make_shared<ShardingCoordinatorExternalStateImpl>();
}

}  // namespace mongo
