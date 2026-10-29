// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/sharding_environment/client/num_hosts_targeted_metrics.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_server_status.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/s/balancer_configuration.h"

#include <memory>

namespace mongo {
namespace {

class ShardingServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto const grid = Grid::get(opCtx);
        auto const shardRegistry = grid->shardRegistry();

        BSONObjBuilder result;

        result.append("configsvrConnectionString",
                      shardRegistry->getConfigServerConnectionString().toString());

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

        return result.obj();
    }

private:
    ClusterServerParameterServerStatus _clusterParameterStatus;
};
auto& shardingServerStatus =
    *ServerStatusSectionBuilder<ShardingServerStatus>("sharding").forRouter();

class ShardingStatisticsServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {

        const auto grid = Grid::get(opCtx);
        if (!grid->isInitialized()) {
            return {};
        }

        BSONObjBuilder result;
        auto configServerInShardCache = grid->shardRegistry()->cachedClusterHasConfigShard();
        result.appendBool("configServerInShardCache", configServerInShardCache.value_or(false));

        NumHostsTargetedMetrics::get(opCtx).report(&result);
        grid->catalogCache()->report(&result);
        grid->shardRegistry()->report(&result);

        auto const& shardSharedStateCache = ShardSharedStateCache::get(opCtx);
        {
            auto shards = BSONObjBuilder{result.subobjStart("shards")};
            shardSharedStateCache.report(&shards);
        }

        return result.obj();
    }
};
auto& shardingStatisitcsServerStatus =
    *ServerStatusSectionBuilder<ShardingStatisticsServerStatus>("shardingStatistics").forRouter();

}  // namespace
}  // namespace mongo
