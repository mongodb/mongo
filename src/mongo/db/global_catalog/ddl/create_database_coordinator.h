// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/create_database_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/util/modules.h"

namespace mongo {
class CreateDatabaseCoordinator final
    : public RecoverableShardingDDLCoordinator<CreateDatabaseCoordinatorDocument> {
public:
    CreateDatabaseCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "CreateDatabaseCoordinator", initialState),
          _critSecReason(BSON("createDatabase" << DatabaseNameUtil::serialize(
                                  nss().dbName(), SerializationContext::stateCommandRequest()))) {}

    ~CreateDatabaseCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override {}

    ConfigsvrCreateDatabaseResponse getResult(OperationContext* opCtx);

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kEnterCriticalSectionOnPrimary;
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    void checkDBVersion(OperationContext* opCtx, bool afterAcquiringLocks) override {
        // CreateDatabaseCoordinator runs on the config server, so don't check the DB version.
    }

    // Check the command arguments passed and if a database can be returned right away.
    void _checkPreconditions();

    void _setupPrimaryShard(OperationContext* opCtx);

    void _enterCriticalSection(OperationContext* opCtx,
                               std::shared_ptr<executor::ScopedTaskExecutor> executor,
                               const CancellationToken& token);

    void _storeDBVersion(OperationContext* opCtx, const DatabaseType& db);

    void _exitCriticalSection(OperationContext* opCtx,
                              std::shared_ptr<executor::ScopedTaskExecutor> executor,
                              const CancellationToken& token,
                              bool throwIfReasonDiffers);

    DatabaseType _commitClusterCatalog(OperationContext* opCtx);

    const BSONObj _critSecReason;

    // Set on successful completion of the coordinator.
    boost::optional<ConfigsvrCreateDatabaseResponse> _result;
};

}  // namespace mongo
