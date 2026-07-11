// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state_for_test.h"

namespace mongo {

ShardingCoordinatorExternalStateForTest::ShardingCoordinatorExternalStateForTest() {
    allowMigrationsResponse = MockCommandResponse();
    migrationsAllowedResponse = MockCommandResponse();
}

void ShardingCoordinatorExternalStateForTest::checkShardedDDLAllowedToStart(
    OperationContext* opCtx, const NamespaceString& nss) const {}

void ShardingCoordinatorExternalStateForTest::waitForVectorClockDurable(
    OperationContext* opCtx) const {}

void ShardingCoordinatorExternalStateForTest::assertIsPrimaryShardForDb(
    OperationContext* opCtx, const DatabaseName& dbName) const {}

bool ShardingCoordinatorExternalStateForTest::isTrackedTimeseries(
    OperationContext* opCtx, const NamespaceString& bucketNss) const {
    return false;
}

void ShardingCoordinatorExternalStateForTest::allowMigrations(
    OperationContext* opCtx,
    const NamespaceString& nss,
    bool allowMigrations,
    std::function<OperationSessionInfo()> osiGenerator,
    AuthoritativeMetadataAccessLevelEnum authoritativeState) {
    allowMigrationsResponse.getNext();
    migrationsAllowed = allowMigrations;
}

bool ShardingCoordinatorExternalStateForTest::checkAllowMigrationsOnConfigServer(
    OperationContext* opCtx, const NamespaceString& nss) {
    migrationsAllowedResponse.getNext();
    return migrationsAllowed;
}

ShardingCoordinatorExternalStateFactoryForTest::ShardingCoordinatorExternalStateFactoryForTest(
    std::shared_ptr<ShardingCoordinatorExternalStateForTest> externalState) {
    _externalState = std::move(externalState);
}

std::shared_ptr<ShardingCoordinatorExternalState>
ShardingCoordinatorExternalStateFactoryForTest::create() const {
    if (_externalState != nullptr) {
        return _externalState;
    }
    return std::make_shared<ShardingCoordinatorExternalStateForTest>();
}

}  // namespace mongo
