// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sessions_collection_config_server.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/chunk_constraints.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
using namespace std::literals::string_view_literals;

MONGO_FAIL_POINT_DEFINE(preventSessionsCollectionSharding);

void SessionsCollectionConfigServer::_shardCollectionIfNeeded(OperationContext* opCtx) {
    // First, check if the collection is already sharded.
    try {
        checkSessionsCollectionExists(opCtx);
        return;
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotSharded>&) {
        // If the sessions collection isn't sharded, shard it.
    }

    // If we don't have any shards, we can't set up this collection yet.
    uassert(ErrorCodes::ShardNotFound,
            str::stream() << "Failed to create "
                          << NamespaceString::kLogicalSessionsNamespace.toStringForErrorMsg()
                          << ": cannot create the collection until there are shards",
            Grid::get(opCtx)->shardRegistry()->getNumShards(opCtx) != 0);

    cluster::createCollection(opCtx, cluster::shardLogicalSessionsCollectionRequest());

    Lock::GlobalLock lock(opCtx, MODE_IX);
    if (const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        replCoord->canAcceptWritesFor(opCtx, NamespaceString::kConfigsvrCollectionsNamespace)) {
        auto filterQuery =
            BSON("_id" << NamespaceStringUtil::serialize(NamespaceString::kLogicalSessionsNamespace,
                                                         SerializationContext::stateDefault())
                       << CollectionType::kMaxChunkSizeBytesFieldName << BSON("$exists" << false));
        auto updateQuery = BSON("$set" << BSON(CollectionType::kMaxChunkSizeBytesFieldName
                                               << logical_sessions::kMaxChunkSizeBytes));

        const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

        // TODO SERVER-83917: Make a more general API to switch the service used by the client.
        auto originalService = opCtx->getService();
        auto shardService = opCtx->getServiceContext()->getService();
        {
            ClientLock lk(opCtx->getClient());
            opCtx->getClient()->setService(shardService);
        }

        ScopeGuard onScopeExitGuard([&] {
            ClientLock lk(opCtx->getClient());
            opCtx->getClient()->setService(originalService);
        });

        uassertStatusOK(catalogClient->updateConfigDocument(
            opCtx,
            NamespaceString::kConfigsvrCollectionsNamespace,
            filterQuery,
            updateQuery,
            false,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter()));
    }
}

void SessionsCollectionConfigServer::_generateIndexesIfNeeded(OperationContext* opCtx) {
    const auto nss = NamespaceString::kLogicalSessionsNamespace;
    sharding::router::CollectionRouter router(opCtx, nss);
    router.routeWithRoutingContext(
        "SessionsCollectionConfigServer::_generateIndexesIfNeeded"sv,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
            // (SERVER-61214) This assertion ensures that the catalog cache recognizes
            // the sessions collection as sharded, guaranteeing the retrieval of a
            // valid routing table.
            uassert(StaleConfigInfo(nss,
                                    cri.getCollectionVersion() /* receivedVersion */,
                                    ShardVersion::UNTRACKED() /* wantedVersion */,
                                    ShardingState::get(opCtx)->getShardHandle().toShardRef(opCtx)),
                    str::stream() << "Collection " << nss.toStringForErrorMsg()
                                  << " is not sharded",
                    cri.isSharded());

            auto shardResults = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                routingCtx,
                nss,
                SessionsCollection::generateCreateIndexesCmd(),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kStrictlyNotIdempotent,
                BSONObj() /*query*/,
                BSONObj() /*collation*/,
                boost::none /*letParameters*/,
                boost::none /*runtimeConstants*/);

            for (auto& shardResult : shardResults) {
                const auto shardResponse = uassertStatusOK(std::move(shardResult.swResponse));
                const auto& res = shardResponse.data;
                uassertStatusOK(getStatusFromCommandResult(res));
            }
        });
}

void SessionsCollectionConfigServer::setupSessionsCollection(OperationContext* opCtx) {
    // If the sharding state is not yet initialized, fail.
    uassert(ErrorCodes::ShardingStateNotInitialized,
            "sharding state is not yet initialized",
            Grid::get(opCtx)->isShardingInitialized());

    std::lock_guard<std::mutex> lk(_mutex);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Cannot setup the sessions collection when failpoint "
            "`preventSessionsCollectionSharding` is set",
            !MONGO_unlikely(preventSessionsCollectionSharding.shouldFail()));

    _shardCollectionIfNeeded(opCtx);
    _generateIndexesIfNeeded(opCtx);
}

}  // namespace mongo
