// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/router_role/routing_cache/routing_information_cache.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/resharding/local_resharding_operations_registry.h"
#include "mongo/db/s/resharding/resharding_cumulative_metrics.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_server_status.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
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
            auto const& shardSharedStateCache = ShardSharedStateCache::get(opCtx);

            ShardingStatistics::get(opCtx).report(&result);
            catalogCache->report(&result);

            {
                auto shards = BSONObjBuilder{result.subobjStart("shards")};
                shardSharedStateCache.report(&shards);
            }

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
        LocalReshardingOperationsRegistry::get().reportForServerStatus(&result);

        return result.obj();
    }

    void reportDataTransformMetrics(OperationContext* opCtx, BSONObjBuilder* bob) const {
        auto sCtx = opCtx->getServiceContext();
        using Metrics = ReshardingCumulativeMetrics;
        Metrics::getForResharding(sCtx)->reportForServerStatus(bob);
        Metrics::getForMoveCollection(sCtx)->reportForServerStatus(bob);
        Metrics::getForRewriteCollection(sCtx)->reportForServerStatus(bob);
        Metrics::getForBalancerMoveCollection(sCtx)->reportForServerStatus(bob);
        Metrics::getForUnshardCollection(sCtx)->reportForServerStatus(bob);
    }
};
auto& shardingStatisticsServerStatus =
    *ServerStatusSectionBuilder<ShardingStatisticsServerStatus>("shardingStatistics").forShard();

}  // namespace
}  // namespace mongo
