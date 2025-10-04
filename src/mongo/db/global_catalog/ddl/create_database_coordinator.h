/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/global_catalog/ddl/create_database_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/type_database_gen.h"

namespace mongo {
class CreateDatabaseCoordinator final
    : public RecoverableShardingDDLCoordinator<CreateDatabaseCoordinatorDocument,
                                               CreateDatabaseCoordinatorPhaseEnum> {
public:
    using StateDoc = CreateDatabaseCoordinatorDocument;
    using Phase = CreateDatabaseCoordinatorPhaseEnum;

    CreateDatabaseCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "CreateDatabaseCoordinator", initialState),
          _critSecReason(BSON("createDatabase" << DatabaseNameUtil::serialize(
                                  nss().dbName(), SerializationContext::stateCommandRequest()))) {}

    ~CreateDatabaseCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override {}

    ConfigsvrCreateDatabaseResponse getResult(OperationContext* opCtx);

private:
    StringData serializePhase(const Phase& phase) const override {
        return CreateDatabaseCoordinatorPhase_serializer(phase);
    }

    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kEnterCriticalSectionOnPrimary;
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

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
