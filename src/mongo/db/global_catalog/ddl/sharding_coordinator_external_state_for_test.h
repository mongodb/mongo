// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state.h"
#include "mongo/db/global_catalog/ddl/sharding_test_helpers.h"
#include "mongo/util/modules.h"

namespace mongo {

using Fault = sharding_test_helpers::Fault;
using MockCommandResponse = sharding_test_helpers::FaultGenerator;

class ShardingCoordinatorExternalStateForTest : public ShardingCoordinatorExternalState {
public:
    ShardingCoordinatorExternalStateForTest();
    void checkShardedDDLAllowedToStart(OperationContext* opCtx,
                                       const NamespaceString& nss) const override;
    void waitForVectorClockDurable(OperationContext* opCtx) const override;
    void assertIsPrimaryShardForDb(OperationContext* opCtx,
                                   const DatabaseName& dbName) const override;
    bool isTrackedTimeseries(OperationContext* opCtx,
                             const NamespaceString& bucketNss) const override;
    void allowMigrations(OperationContext* opCtx,
                         const NamespaceString& nss,
                         bool allowMigrations,
                         std::function<OperationSessionInfo()> osiGenerator,
                         AuthoritativeMetadataAccessLevelEnum authoritativeState) override;
    bool checkAllowMigrationsOnConfigServer(OperationContext* opCtx,
                                            const NamespaceString& nss) override;

    MockCommandResponse allowMigrationsResponse;
    MockCommandResponse migrationsAllowedResponse;
    bool migrationsAllowed = true;
};

class ShardingCoordinatorExternalStateFactoryForTest
    : public ShardingCoordinatorExternalStateFactory {
public:
    ShardingCoordinatorExternalStateFactoryForTest() {}
    ShardingCoordinatorExternalStateFactoryForTest(
        std::shared_ptr<ShardingCoordinatorExternalStateForTest> externalState);

    std::shared_ptr<ShardingCoordinatorExternalState> create() const override;

private:
    std::shared_ptr<ShardingCoordinatorExternalStateForTest> _externalState;
};

}  // namespace mongo
