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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_server_status.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {
namespace {

class ShardingServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer);
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
            !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer))
            return {};

        auto const shardingState = ShardingState::get(opCtx);
        if (!shardingState->enabled())
            return {};

        auto const grid = Grid::get(opCtx);
        auto const shardRegistry = grid->shardRegistry();

        BSONObjBuilder result;

        result.append("configsvrConnectionString",
                      shardRegistry->getConfigServerConnectionString().toString());

        shardRegistry->report(&result);

        const auto vcTime = VectorClock::get(opCtx)->getTime();

        const auto configOpTime = [&]() {
            const auto vcConfigTimeTs = vcTime.configTime().asTimestamp();
            return mongo::repl::OpTime(vcConfigTimeTs, mongo::repl::OpTime::kUninitializedTerm);
        }();
        configOpTime.append("lastSeenConfigServerOpTime", &result);

        const auto topologyOpTime = [&]() {
            const auto vcTopologyTimeTs = vcTime.topologyTime().asTimestamp();
            return mongo::repl::OpTime(vcTopologyTimeTs, mongo::repl::OpTime::kUninitializedTerm);
        }();
        topologyOpTime.append("lastSeenTopologyOpTime", &result);

        const long long maxChunkSizeInBytes =
            grid->getBalancerConfiguration()->getMaxChunkSizeBytes();
        result.append("maxChunkSizeInBytes", maxChunkSizeInBytes);

        _clusterParameterStatus.report(opCtx, &result);

        // Get a migration status report if a migration is active. The call to
        // getActiveMigrationStatusReport will take an IS lock on the namespace of the active
        // migration if there is one that is active.
        BSONObj migrationStatus =
            ActiveMigrationsRegistry::get(opCtx).getActiveMigrationStatusReport(opCtx);
        if (!migrationStatus.isEmpty()) {
            result.append("migrations", migrationStatus);
        }

        return result.obj();
    }

private:
    ClusterServerParameterServerStatus _clusterParameterStatus;
};
auto& shardingServerStatus =
    *ServerStatusSectionBuilder<ShardingServerStatus>("sharding").forShard();

class ShardingStatisticsServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
            serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer);
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) &&
            !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer))
            return {};

        BSONObjBuilder result;
        if (auto const shardingState = ShardingState::get(opCtx); shardingState->enabled()) {
            auto const grid = Grid::get(opCtx);
            auto const catalogCache = grid->catalogCache();
            auto const routingInfoCache = RoutingInformationCache::get(opCtx);

            ShardingStatistics::get(opCtx).report(&result);
            catalogCache->report(&result);
            if (routingInfoCache && !feature_flags::gDualCatalogCache.isEnabled()) {
                routingInfoCache->report(&result);
            }

            FilteringMetadataCache::get(opCtx)->report(&result);

            auto nRangeDeletions = [&]() {
                try {
                    return RangeDeleterService::get(opCtx)->totalNumOfRegisteredTasks();
                } catch (const ExceptionFor<ErrorCodes::NotYetInitialized>&) {
                    return 0LL;
                }
            }();
            result.appendNumber("rangeDeleterTasks", nRangeDeletions);

            auto configServerInShardCache = grid->shardRegistry()->cachedClusterHasConfigShard();
            result.appendBool("configServerInShardCache", configServerInShardCache.value_or(false));
        }

        reportDataTransformMetrics(opCtx, &result);

        return result.obj();
    }

    void reportDataTransformMetrics(OperationContext* opCtx, BSONObjBuilder* bob) const {
        auto sCtx = opCtx->getServiceContext();
        using Metrics = ReshardingCumulativeMetrics;
        Metrics::getForResharding(sCtx)->reportForServerStatus(bob);
        Metrics::getForMoveCollection(sCtx)->reportForServerStatus(bob);
        Metrics::getForBalancerMoveCollection(sCtx)->reportForServerStatus(bob);
        Metrics::getForUnshardCollection(sCtx)->reportForServerStatus(bob);
    }
};
auto& shardingStatisticsServerStatus =
    *ServerStatusSectionBuilder<ShardingStatisticsServerStatus>("shardingStatistics").forShard();

}  // namespace
}  // namespace mongo
