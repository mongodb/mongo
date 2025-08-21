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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_server_status.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/sharding_environment/client/num_hosts_targeted_metrics.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/vector_clock/vector_clock.h"
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

        return result.obj();
    }
};
auto& shardingStatisitcsServerStatus =
    *ServerStatusSectionBuilder<ShardingStatisticsServerStatus>("shardingStatistics").forRouter();

}  // namespace
}  // namespace mongo
