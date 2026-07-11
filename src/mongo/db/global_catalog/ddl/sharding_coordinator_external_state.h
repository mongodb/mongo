// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {
class ShardingCoordinatorExternalState {
public:
    virtual ~ShardingCoordinatorExternalState() = default;
    virtual void checkShardedDDLAllowedToStart(OperationContext* opCtx,
                                               const NamespaceString& nss) const = 0;
    virtual void waitForVectorClockDurable(OperationContext* opCtx) const = 0;
    virtual void assertIsPrimaryShardForDb(OperationContext* opCtx,
                                           const DatabaseName& dbName) const = 0;
    virtual bool isTrackedTimeseries(OperationContext* opCtx,
                                     const NamespaceString& bucketNss) const = 0;
    virtual void allowMigrations(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 bool allowMigrations,
                                 std::function<OperationSessionInfo()> osiGetter,
                                 AuthoritativeMetadataAccessLevelEnum authoritativeState) = 0;
    virtual bool checkAllowMigrationsOnConfigServer(OperationContext* opCtx,
                                                    const NamespaceString& nss) = 0;

private:
};

class ShardingCoordinatorExternalStateImpl : public ShardingCoordinatorExternalState {
public:
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
                         std::function<OperationSessionInfo()> osiGetter,
                         AuthoritativeMetadataAccessLevelEnum authoritativeState) override;
    bool checkAllowMigrationsOnConfigServer(OperationContext* opCtx,
                                            const NamespaceString& nss) override;
};

class ShardingCoordinatorExternalStateFactory {
public:
    virtual ~ShardingCoordinatorExternalStateFactory() = default;
    virtual std::shared_ptr<ShardingCoordinatorExternalState> create() const = 0;
};

class ShardingCoordinatorExternalStateFactoryImpl : public ShardingCoordinatorExternalStateFactory {
public:
    std::shared_ptr<ShardingCoordinatorExternalState> create() const override;
};


}  // namespace mongo
