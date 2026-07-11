// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_external_state.h"

#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator.h"
#include "mongo/db/global_catalog/ddl/migration_blocking_operation_gen.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
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

    const auto response = self->runCommand(opCtx,
                                           ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                           DatabaseName::kAdmin,
                                           command.toBSON(),
                                           Shard::RetryPolicy::kIdempotent);
    return uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(response));
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
