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

#include <memory>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/vector_clock.h"
#include "mongo/executor/hedging_metrics.h"
#include "mongo/idl/cluster_server_parameter_server_status.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"

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

        result.append("routerServiceEnabled",
                      serverGlobalParams.clusterRole.has(ClusterRole::RouterServer));

        result.append("configsvrConnectionString",
                      shardRegistry->getConfigServerConnectionString().toString());

        const auto vcTime = VectorClock::get(opCtx)->getTime();

        const auto configOpTime = [&]() {
            const auto vcConfigTimeTs = vcTime.configTime().asTimestamp();
            return mongo::repl::OpTime(vcConfigTimeTs, mongo::repl::OpTime::kUninitializedTerm);
        }();
        configOpTime.append(&result, "lastSeenConfigServerOpTime");

        const auto topologyOpTime = [&]() {
            const auto vcTopologyTimeTs = vcTime.topologyTime().asTimestamp();
            return mongo::repl::OpTime(vcTopologyTimeTs, mongo::repl::OpTime::kUninitializedTerm);
        }();
        topologyOpTime.append(&result, "lastSeenTopologyOpTime");

        const long long maxChunkSizeInBytes =
            grid->getBalancerConfiguration()->getMaxChunkSizeBytes();
        result.append("maxChunkSizeInBytes", maxChunkSizeInBytes);

        _clusterParameterStatus.report(opCtx, &result);

        return result.obj();
    }

private:
    ClusterServerParameterServerStatus _clusterParameterStatus;
};
auto& shardingServerStatus = *ServerStatusSectionBuilder<ShardingServerStatus>("sharding");

class ShardingStatisticsServerStatus final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto const grid = Grid::get(opCtx);
        auto const catalogCache = grid->catalogCache();
        auto& numHostsTargetedMetrics = NumHostsTargetedMetrics::get(opCtx);

        BSONObjBuilder result;

        numHostsTargetedMetrics.appendSection(&result);
        catalogCache->report(&result);
        return result.obj();
    }
};
auto& shardingStatisitcsServerStatus =
    *ServerStatusSectionBuilder<ShardingStatisticsServerStatus>("shardingStatistics");

class HedgingMetricsServerStatus : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~HedgingMetricsServerStatus() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        return HedgingMetrics::get(opCtx)->toBSON();
    }
};
auto& hedgingMetricsServerStatus =
    *ServerStatusSectionBuilder<HedgingMetricsServerStatus>("hedgingMetrics");

}  // namespace
}  // namespace mongo
