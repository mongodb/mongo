// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state_for_test.h"

#include "mongo/db/s/primary_only_service_helpers/operation_session_tracker.h"

namespace mongo {
namespace {
// Unit tests have no real shard servers, so the causality barrier must not attempt the noop
// retryable write on participants.
class NoOpCausalityBarrier : public CausalityBarrier {
public:
    void perform(OperationContext*, const OperationSessionInfo&) override {}
};
}  // namespace

ShardingCoordinatorExternalStateForTest::ShardingCoordinatorExternalStateForTest() {
    allowMigrationsResponse = MockCommandResponse();
    migrationsAllowedResponse = MockCommandResponse();
}

void ShardingCoordinatorExternalStateForTest::checkShardedDDLAllowedToStart(
    OperationContext* opCtx, const NamespaceString& nss) const {}

void ShardingCoordinatorExternalStateForTest::waitForVectorClockDurable(
    OperationContext* opCtx) const {}

void ShardingCoordinatorExternalStateForTest::assertIsPrimaryShardForDb(
    OperationContext* opCtx, const DatabaseName& dbName) const {
    assertIsPrimaryShardForDbResponse.getNext();
}

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
    _migrationsAllowed.store(allowMigrations);
}

bool ShardingCoordinatorExternalStateForTest::checkAllowMigrationsOnConfigServer(
    OperationContext* opCtx, const NamespaceString& nss) {
    migrationsAllowedResponse.getNext();
    return _migrationsAllowed.load();
}

std::unique_ptr<CausalityBarrier> ShardingCoordinatorExternalStateForTest::makeCausalityBarrier(
    std::shared_ptr<executor::TaskExecutor> executor, CancellationToken token) {
    return std::make_unique<NoOpCausalityBarrier>();
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
