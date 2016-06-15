/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/s/shard_util.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace shardutil {
namespace {

const char kMinKey[] = "min";
const char kMaxKey[] = "max";
const char kShouldMigrate[] = "shouldMigrate";

}  // namespace

StatusWith<long long> retrieveTotalShardSize(OperationContext* txn, const ShardId& shardId) {
    auto shard = Grid::get(txn)->shardRegistry()->getShard(txn, shardId);
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "shard " << shardId << " not found");
    }
    auto listDatabasesStatus =
        shard->runCommand(txn,
                          ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                          "admin",
                          BSON("listDatabases" << 1),
                          Shard::RetryPolicy::kIdempotent);
    if (!listDatabasesStatus.isOK()) {
        return std::move(listDatabasesStatus.getStatus());
    }
    if (!listDatabasesStatus.getValue().commandStatus.isOK()) {
        return std::move(listDatabasesStatus.getValue().commandStatus);
    }

    BSONElement totalSizeElem = listDatabasesStatus.getValue().response["totalSize"];
    if (!totalSizeElem.isNumber()) {
        return {ErrorCodes::NoSuchKey, "totalSize field not found in listDatabases"};
    }

    return totalSizeElem.numberLong();
}

StatusWith<BSONObj> selectMedianKey(OperationContext* txn,
                                    const ShardId& shardId,
                                    const NamespaceString& nss,
                                    const ShardKeyPattern& shardKeyPattern,
                                    const BSONObj& minKey,
                                    const BSONObj& maxKey) {
    BSONObjBuilder cmd;
    cmd.append("splitVector", nss.ns());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    cmd.append(kMinKey, minKey);
    cmd.append(kMaxKey, maxKey);
    cmd.appendBool("force", true);

    auto shard = Grid::get(txn)->shardRegistry()->getShard(txn, shardId);
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "shard " << shardId << " not found");
    }
    auto cmdStatus = shard->runCommand(txn,
                                       ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                                       "admin",
                                       cmd.obj(),
                                       Shard::RetryPolicy::kIdempotent);
    if (!cmdStatus.isOK()) {
        return std::move(cmdStatus.getStatus());
    }
    if (!cmdStatus.getValue().commandStatus.isOK()) {
        return std::move(cmdStatus.getValue().commandStatus);
    }

    const auto response = std::move(cmdStatus.getValue().response);

    BSONObjIterator it(response.getObjectField("splitKeys"));
    if (it.more()) {
        return it.next().Obj().getOwned();
    }

    return BSONObj();
}

StatusWith<std::vector<BSONObj>> selectChunkSplitPoints(OperationContext* txn,
                                                        const ShardId& shardId,
                                                        const NamespaceString& nss,
                                                        const ShardKeyPattern& shardKeyPattern,
                                                        const BSONObj& minKey,
                                                        const BSONObj& maxKey,
                                                        long long chunkSizeBytes,
                                                        int maxPoints,
                                                        int maxObjs) {
    BSONObjBuilder cmd;
    cmd.append("splitVector", nss.ns());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    cmd.append(kMinKey, minKey);
    cmd.append(kMaxKey, maxKey);
    cmd.append("maxChunkSizeBytes", chunkSizeBytes);
    cmd.append("maxSplitPoints", maxPoints);
    cmd.append("maxChunkObjects", maxObjs);

    auto shard = Grid::get(txn)->shardRegistry()->getShard(txn, shardId);
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "shard " << shardId << " not found");
    }
    auto cmdStatus = shard->runCommand(txn,
                                       ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                                       "admin",
                                       cmd.obj(),
                                       Shard::RetryPolicy::kIdempotent);
    if (!cmdStatus.isOK()) {
        return std::move(cmdStatus.getStatus());
    }
    if (!cmdStatus.getValue().commandStatus.isOK()) {
        return std::move(cmdStatus.getValue().commandStatus);
    }

    const auto response = std::move(cmdStatus.getValue().response);

    std::vector<BSONObj> splitPoints;

    BSONObjIterator it(response.getObjectField("splitKeys"));
    while (it.more()) {
        splitPoints.push_back(it.next().Obj().getOwned());
    }

    return std::move(splitPoints);
}

StatusWith<boost::optional<ChunkRange>> splitChunkAtMultiplePoints(
    OperationContext* txn,
    const ShardId& shardId,
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    ChunkVersion collectionVersion,
    const BSONObj& minKey,
    const BSONObj& maxKey,
    const std::vector<BSONObj>& splitPoints) {
    invariant(!splitPoints.empty());
    invariant(minKey.woCompare(maxKey) < 0);

    const size_t kMaxSplitPoints = 8192;

    if (splitPoints.size() > kMaxSplitPoints) {
        return {ErrorCodes::BadValue,
                str::stream() << "Cannot split chunk in more than " << kMaxSplitPoints
                              << " parts at a time."};
    }

    BSONObjBuilder cmd;
    cmd.append("splitChunk", nss.ns());
    cmd.append("configdb",
               Grid::get(txn)->shardRegistry()->getConfigServerConnectionString().toString());
    cmd.append("from", shardId.toString());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    collectionVersion.appendForCommands(&cmd);
    cmd.append(kMinKey, minKey);
    cmd.append(kMaxKey, maxKey);
    cmd.append("splitKeys", splitPoints);

    BSONObj cmdObj = cmd.obj();

    Status status{ErrorCodes::InternalError, "Uninitialized value"};
    BSONObj cmdResponse;

    auto shard = Grid::get(txn)->shardRegistry()->getShard(txn, shardId);
    if (!shard) {
        status =
            Status(ErrorCodes::ShardNotFound, str::stream() << "shard " << shardId << " not found");
    } else {
        auto cmdStatus = shard->runCommand(txn,
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           "admin",
                                           cmdObj,
                                           Shard::RetryPolicy::kNotIdempotent);
        if (!cmdStatus.isOK()) {
            status = std::move(cmdStatus.getStatus());
        } else {
            status = std::move(cmdStatus.getValue().commandStatus);
            cmdResponse = std::move(cmdStatus.getValue().response);
        }
    }

    if (!status.isOK()) {
        log() << "Split chunk " << cmdObj << " failed" << causedBy(status);
        return {status.code(), str::stream() << "split failed due to " << status.toString()};
    }

    BSONElement shouldMigrateElement;
    status = bsonExtractTypedField(cmdResponse, kShouldMigrate, Object, &shouldMigrateElement);
    if (status.isOK()) {
        auto chunkRangeStatus = ChunkRange::fromBSON(shouldMigrateElement.embeddedObject());
        if (!chunkRangeStatus.isOK()) {
            return chunkRangeStatus.getStatus();
        }

        return boost::optional<ChunkRange>(std::move(chunkRangeStatus.getValue()));
    } else if (status != ErrorCodes::NoSuchKey) {
        warning()
            << "Chunk migration will be skipped because splitChunk returned invalid response: "
            << cmdResponse << ". Extracting " << kShouldMigrate << " field failed"
            << causedBy(status);
    }

    return boost::optional<ChunkRange>();
}

}  // namespace shardutil
}  // namespace mongo
