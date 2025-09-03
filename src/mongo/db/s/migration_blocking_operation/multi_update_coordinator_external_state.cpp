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

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/s/service_entry_point_router_role.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
auto getDatabaseVersion(OperationContext* opCtx, const MultiUpdateCoordinatorMetadata& metadata) {
    // TODO SERVER-110173: Only keep this branch to support older behavior in multiversion testing
    // where the MultiUpdateCoordinator did not store the database version. Once these changes are
    // in last LTS, the database version should always be set and this branch can be removed.
    if (!metadata.getDatabaseVersion().has_value()) {
        auto catalogCache = Grid::get(opCtx)->catalogCache();
        auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, metadata.getNss().dbName()));
        return dbInfo->getVersion();
    }
    return *metadata.getDatabaseVersion();
}

template <typename Command>
auto runDDLOperationOnCurrentShard(OperationContext* opCtx,
                                   const DatabaseVersion& databaseVersion,
                                   Command command) {
    auto selfId = ShardingState::get(opCtx)->shardId();
    auto self = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, selfId));
    generic_argument_util::setMajorityWriteConcern(command, &opCtx->getWriteConcern());
    generic_argument_util::setDbVersionIfPresent(command, databaseVersion);
    return uassertStatusOK(self->runCommand(opCtx,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            DatabaseName::kAdmin,
                                            command.toBSON(),
                                            Shard::RetryPolicy::kIdempotent));
}
}  // namespace

MultiUpdateCoordinatorExternalStateImpl::MultiUpdateCoordinatorExternalStateImpl(
    InternalSessionPool* sessionPool)
    : _sessionPool{sessionPool} {}

Future<DbResponse> MultiUpdateCoordinatorExternalStateImpl::sendClusterUpdateCommandToShards(
    OperationContext* opCtx, const Message& message) const {
    opCtx->setCommandForwardedFromRouter();
    return ServiceEntryPointRouterRole::handleRequestImpl(
        opCtx, message, opCtx->fastClockSource().now());
}

void MultiUpdateCoordinatorExternalStateImpl::startBlockingMigrations(
    OperationContext* opCtx, const MultiUpdateCoordinatorMetadata& metadata) {
    runDDLOperationOnCurrentShard(
        opCtx,
        getDatabaseVersion(opCtx, metadata),
        ShardsvrBeginMigrationBlockingOperation{metadata.getNss(), metadata.getId()});
}

void MultiUpdateCoordinatorExternalStateImpl::stopBlockingMigrations(
    OperationContext* opCtx, const MultiUpdateCoordinatorMetadata& metadata) {
    runDDLOperationOnCurrentShard(
        opCtx,
        getDatabaseVersion(opCtx, metadata),
        ShardsvrEndMigrationBlockingOperation{metadata.getNss(), metadata.getId()});
}

bool MultiUpdateCoordinatorExternalStateImpl::isUpdatePending(
    OperationContext* opCtx, const NamespaceString& nss, AggregateCommandRequest& request) const {
    BSONObjBuilder responseBuilder;
    auto status = ClusterAggregate::runAggregate(opCtx,
                                                 ClusterAggregate::Namespaces{nss, nss},
                                                 request,
                                                 LiteParsedPipeline{request},
                                                 PrivilegeVector(),
                                                 boost::none, /* verbosity */
                                                 &responseBuilder);

    uassertStatusOKWithContext(status, "Aggregation request in MultiUpdateCoordinator failed.");

    auto resultArr = responseBuilder.obj()["cursor"]["firstBatch"].Array();
    return (!resultArr.empty());
}

bool MultiUpdateCoordinatorExternalStateImpl::collectionExists(OperationContext* opCtx,
                                                               const NamespaceString& nss) const {

    try {
        auto acquisition = acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead));
        return acquisition.exists();
    } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
        // CommandNotSupportedOnView will be thrown if the nss is for a timeseries collection.
        return true;
    }
}


void MultiUpdateCoordinatorExternalStateImpl::createCollection(OperationContext* opCtx,
                                                               const NamespaceString& nss) const {
    cluster::createCollectionWithRouterLoop(opCtx, nss);
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
