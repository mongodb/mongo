/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/sessions_collection_config_server.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_constraints.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/cluster_ddl.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

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
        replCoord->canAcceptWritesFor(opCtx, CollectionType::ConfigNS)) {
        auto filterQuery =
            BSON("_id" << NamespaceStringUtil::serialize(NamespaceString::kLogicalSessionsNamespace,
                                                         SerializationContext::stateDefault())
                       << CollectionType::kMaxChunkSizeBytesFieldName << BSON("$exists" << false));
        auto updateQuery = BSON("$set" << BSON(CollectionType::kMaxChunkSizeBytesFieldName
                                               << logical_sessions::kMaxChunkSizeBytes));

        const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

        // TODO SERVER-83917: Make a more general API to switch the service used by the client.
        auto originalService = opCtx->getService();
        auto shardService = opCtx->getServiceContext()->getService(ClusterRole::ShardServer);
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
            CollectionType::ConfigNS,
            filterQuery,
            updateQuery,
            false,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter()));
    }
}

void SessionsCollectionConfigServer::_generateIndexesIfNeeded(OperationContext* opCtx) {
    const auto nss = NamespaceString::kLogicalSessionsNamespace;
    sharding::router::CollectionRouter router(opCtx->getServiceContext(), nss);
    router.routeWithRoutingContext(
        opCtx,
        "SessionsCollectionConfigServer::_generateIndexesIfNeeded"_sd,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            const auto& cri = routingCtx.getCollectionRoutingInfo(nss);
            // (SERVER-61214) This assertion ensures that the catalog cache recognizes
            // the sessions collection as sharded, guaranteeing the retrieval of a
            // valid routing table.
            uassert(StaleConfigInfo(nss,
                                    cri.getCollectionVersion() /* receivedVersion */,
                                    ShardVersion::UNSHARDED() /* wantedVersion */,
                                    ShardingState::get(opCtx)->shardId()),
                    str::stream() << "Collection " << nss.toStringForErrorMsg()
                                  << " is not sharded",
                    cri.isSharded());

            auto shardResults = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                routingCtx,
                nss,
                SessionsCollection::generateCreateIndexesCmd(),
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                Shard::RetryPolicy::kNoRetry,
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

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Cannot setup the sessions collection when failpoint "
            "`preventSessionsCollectionSharding` is set",
            !MONGO_unlikely(preventSessionsCollectionSharding.shouldFail()));

    _shardCollectionIfNeeded(opCtx);
    _generateIndexesIfNeeded(opCtx);
}

}  // namespace mongo
