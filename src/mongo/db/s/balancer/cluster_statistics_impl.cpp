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


#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/random.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

using ShardStatistics = ClusterStatistics::ShardStatistics;

ClusterStatisticsImpl::~ClusterStatisticsImpl() = default;

StatusWith<std::vector<ShardStatistics>> ClusterStatisticsImpl::getStats(OperationContext* opCtx) {
    return _getStats(opCtx, boost::none);
}

StatusWith<std::vector<ShardStatistics>> ClusterStatisticsImpl::getCollStats(
    OperationContext* opCtx, NamespaceString const& ns) {
    return _getStats(opCtx, ns);
}

StatusWith<std::vector<ShardStatistics>> ClusterStatisticsImpl::_getStats(
    OperationContext* opCtx, boost::optional<NamespaceString> ns) {
    // Get a list of all the shards that are participating in this balance round along with any
    // maximum allowed quotas and current utilization. We get the latter by issuing
    // db.serverStatus() (mem.mapped) to all shards.
    //
    // TODO: skip unresponsive shards and mark information as stale.
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    auto shardsStatus =
        catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
    if (!shardsStatus.isOK()) {
        return shardsStatus.getStatus();
    }

    auto& shards = shardsStatus.getValue().value;

    auto client = opCtx->getClient();
    std::shuffle(shards.begin(), shards.end(), client->getPrng().urbg());

    std::vector<ShardStatistics> stats;

    for (const auto& shard : shards) {
        const auto shardSizeStatus = [&]() -> StatusWith<long long> {
            if (ns) {
                return shardutil::retrieveCollectionShardSize(opCtx, shard.getName(), *ns);
            }

            return shardutil::retrieveTotalShardSize(opCtx, shard.getName());
        }();

        if (!shardSizeStatus.isOK()) {
            const auto& status = shardSizeStatus.getStatus();

            return status.withContext(str::stream()
                                      << "Unable to obtain shard utilization information for "
                                      << shard.getName());
        }

        std::set<std::string> shardZones;
        for (const auto& shardZone : shard.getTags()) {
            shardZones.insert(shardZone);
        }

        stats.emplace_back(shard.getName(),
                           shardSizeStatus.getValue(),
                           shard.getDraining(),
                           std::move(shardZones),
                           ShardStatistics::use_bytes_t{});
    }

    return stats;
}

}  // namespace mongo
