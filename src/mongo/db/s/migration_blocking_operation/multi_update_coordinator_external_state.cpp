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

#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_external_state.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/s/migration_blocking_operation/migration_blocking_operation_coordinator.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/service_entry_point_mongos.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
auto runDDLOperationWithRetry(OperationContext* opCtx,
                              const NamespaceString& nss,
                              BSONObj command) {
    auto cmdName = command.firstElement().fieldNameStringData();
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    return shardVersionRetry(opCtx, catalogCache, nss, cmdName, [&] {
        const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, nss.dbName()));
        auto response = executeDDLCoordinatorCommandAgainstDatabasePrimary(
            opCtx,
            DatabaseName::kAdmin,
            dbInfo,
            CommandHelpers::appendMajorityWriteConcern(command, opCtx->getWriteConcern()),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);
        const auto remoteResponse = uassertStatusOK(response.swResponse);
        return uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
    });
}
}  // namespace

MultiUpdateCoordinatorExternalStateImpl::MultiUpdateCoordinatorExternalStateImpl(
    InternalSessionPool* sessionPool)
    : _sessionPool{sessionPool} {}

Future<DbResponse> MultiUpdateCoordinatorExternalStateImpl::sendClusterUpdateCommandToShards(
    OperationContext* opCtx, const Message& message) const {
    return ServiceEntryPointMongos::handleRequestImpl(opCtx, message);
}

void MultiUpdateCoordinatorExternalStateImpl::startBlockingMigrations(OperationContext* opCtx,
                                                                      const NamespaceString& nss,
                                                                      const UUID& operationId) {
    runDDLOperationWithRetry(
        opCtx,
        nss,
        BSON("_shardsvrBeginMigrationBlockingOperation"
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateCommandRequest())
             << "operationId" << operationId));
}

void MultiUpdateCoordinatorExternalStateImpl::stopBlockingMigrations(OperationContext* opCtx,
                                                                     const NamespaceString& nss,
                                                                     const UUID& operationId) {
    runDDLOperationWithRetry(
        opCtx,
        nss,
        BSON("_shardsvrEndMigrationBlockingOperation"
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateCommandRequest())
             << "operationId" << operationId));
}

bool MultiUpdateCoordinatorExternalStateImpl::isUpdatePending(
    OperationContext* opCtx, const NamespaceString& nss, AggregateCommandRequest& request) const {
    auto catalogCache = Grid::get(opCtx)->catalogCache();
    return shardVersionRetry(
        opCtx, catalogCache, nss, "AggregationForMultiUpdateCoordinator"_sd, [&] {
            BSONObjBuilder responseBuilder;
            auto status = ClusterAggregate::runAggregate(opCtx,
                                                         ClusterAggregate::Namespaces{nss, nss},
                                                         request,
                                                         LiteParsedPipeline{request},
                                                         PrivilegeVector(),
                                                         &responseBuilder);

            uassertStatusOKWithContext(status,
                                       "Aggregation request in MultiUpdateCoordinator failed.");

            auto resultArr = responseBuilder.obj()["cursor"]["firstBatch"].Array();
            return (!resultArr.empty());
        });
}


InternalSessionPool::Session MultiUpdateCoordinatorExternalStateImpl::acquireSession() {
    return _sessionPool->acquireSystemSession();
}

void MultiUpdateCoordinatorExternalStateImpl::releaseSession(InternalSessionPool::Session session) {
    _sessionPool->release(std::move(session));
}

MultiUpdateCoordinatorExternalStateFactoryImpl::MultiUpdateCoordinatorExternalStateFactoryImpl(
    ServiceContext* serviceContext)
    : _serviceContext{serviceContext} {}

std::unique_ptr<MultiUpdateCoordinatorExternalState>
MultiUpdateCoordinatorExternalStateFactoryImpl::createExternalState() const {
    return std::make_unique<MultiUpdateCoordinatorExternalStateImpl>(
        InternalSessionPool::get(_serviceContext));
}

}  // namespace mongo
