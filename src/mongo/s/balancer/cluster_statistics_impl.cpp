/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/balancer/cluster_statistics_impl.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::vector;

namespace {

const char kVersionField[] = "version";

/**
 * Executes the serverStatus command against the specified shard and obtains the version of the
 * running MongoD service.
 *
 * Returns the MongoD version in strig format or an error. Known error codes are:
 *  ShardNotFound if shard by that id is not available on the registry
 *  NoSuchKey if the version could not be retrieved
 */
StatusWith<string> retrieveShardMongoDVersion(OperationContext* txn, ShardId shardId) {
    auto shardRegistry = Grid::get(txn)->shardRegistry();
    auto shard = shardRegistry->getShard(txn, shardId);
    if (!shard) {
        return {ErrorCodes::ShardNotFound, str::stream() << "shard " << shardId << " not found"};
    }

    auto commandResponse = shard->runCommand(txn,
                                             ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                             "admin",
                                             BSON("serverStatus" << 1),
                                             Shard::RetryPolicy::kIdempotent);
    if (!commandResponse.isOK()) {
        return commandResponse.getStatus();
    }
    if (!commandResponse.getValue().commandStatus.isOK()) {
        return commandResponse.getValue().commandStatus;
    }

    BSONObj serverStatus = std::move(commandResponse.getValue().response);

    string version;
    Status status = bsonExtractStringField(serverStatus, kVersionField, &version);
    if (!status.isOK()) {
        return status;
    }

    return version;
}

}  // namespace

using ShardStatistics = ClusterStatistics::ShardStatistics;

ClusterStatisticsImpl::ClusterStatisticsImpl() = default;

ClusterStatisticsImpl::~ClusterStatisticsImpl() = default;

StatusWith<vector<ShardStatistics>> ClusterStatisticsImpl::getStats(OperationContext* txn) {
    try {
        _refreshShardStats(txn);
    } catch (const DBException& e) {
        return e.toStatus();
    }

    vector<ShardStatistics> stats;

    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    for (const auto& stat : _shardStatsMap) {
        stats.push_back(stat.second);
    }

    return stats;
}

void ClusterStatisticsImpl::_refreshShardStats(OperationContext* txn) {
    // Get a list of all the shards that are participating in this balance round along with any
    // maximum allowed quotas and current utilization. We get the latter by issuing
    // db.serverStatus() (mem.mapped) to all shards.
    //
    // TODO: skip unresponsive shards and mark information as stale.
    auto shardsStatus = Grid::get(txn)->catalogClient(txn)->getAllShards(txn);
    uassertStatusOK(shardsStatus.getStatus());

    const vector<ShardType> shards(std::move(shardsStatus.getValue().value));

    for (const auto& shard : shards) {
        auto shardSizeStatus = shardutil::retrieveTotalShardSize(txn, shard.getName());
        if (!shardSizeStatus.isOK()) {
            continue;
        }

        auto mongoDVersionStatus = retrieveShardMongoDVersion(txn, shard.getName());
        if (!mongoDVersionStatus.isOK()) {
            continue;
        }
        const string mongoDVersion(std::move(mongoDVersionStatus.getValue()));

        std::set<string> shardTags;
        for (const auto& shardTag : shard.getTags()) {
            shardTags.insert(shardTag);
        }

        ShardStatistics newShardStat(shard.getName(),
                                     shard.getMaxSizeMB(),
                                     shardSizeStatus.getValue() / 1024 / 1024,
                                     shard.getDraining(),
                                     shardTags,
                                     mongoDVersion);

        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        _shardStatsMap[shard.getName()] = std::move(newShardStat);
    }
}

}  // namespace mongo
