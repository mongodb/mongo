// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/balancer/cluster_statistics_impl.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/ddl/shard_util.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

using ShardStatistics = ClusterStatistics::ShardStatistics;

ClusterStatisticsImpl::~ClusterStatisticsImpl() = default;

StatusWith<std::vector<ShardStatistics>> ClusterStatisticsImpl::getStats(OperationContext* opCtx) {
    // Get a list of all the shards that are participating in this balance round along with any
    // maximum allowed quotas and current utilization. We get the latter by issuing
    // db.serverStatus() (mem.mapped) to all shards.
    //
    // TODO: skip unresponsive shards and mark information as stale.
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    std::vector<ShardType> shards;
    try {
        shards = catalogClient->getAllShards(opCtx, repl::ReadConcernArgs::kMajority).value;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    auto client = opCtx->getClient();
    std::shuffle(shards.begin(), shards.end(), client->getPrng().urbg());

    std::vector<ShardStatistics> stats;

    for (const auto& shard : shards) {
        std::set<std::string> shardZones;
        for (const auto& shardZone : shard.getTags()) {
            shardZones.insert(shardZone);
        }

        stats.emplace_back(shard.getName(), shard.getDraining(), std::move(shardZones));
    }

    return stats;
}
}  // namespace mongo
