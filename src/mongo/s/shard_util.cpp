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
#include "mongo/s/request_types/auto_split_vector_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace shardutil {
namespace {

const char kMinKey[] = "min";
const char kMaxKey[] = "max";
const char kShouldMigrate[] = "shouldMigrate";

}  // namespace

StatusWith<long long> retrieveTotalShardSize(OperationContext* opCtx, const ShardId& shardId) {
    auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }

    // Since 'listDatabases' is potentially slow in the presence of large number of collections, use
    // a higher maxTimeMS to prevent it from prematurely timing out
    const Minutes maxTimeMSOverride{10};

    auto listDatabasesStatus = shardStatus.getValue()->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
        "admin",
        BSON("listDatabases" << 1),
        maxTimeMSOverride,
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

StatusWith<std::vector<BSONObj>> selectChunkSplitPoints(OperationContext* opCtx,
                                                        const ShardId& shardId,
                                                        const NamespaceString& nss,
                                                        const ShardKeyPattern& shardKeyPattern,
                                                        const ChunkRange& chunkRange,
                                                        long long chunkSizeBytes) {
    auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }

    auto invokeSplitCommand = [&](const BSONObj& command, const StringData db) {
        return shardStatus.getValue()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
            db.toString(),
            command,
            Shard::RetryPolicy::kIdempotent);
    };

    const AutoSplitVectorRequest req(
        nss, shardKeyPattern.toBSON(), chunkRange.getMin(), chunkRange.getMax(), chunkSizeBytes);

    auto cmdStatus = invokeSplitCommand(req.toBSON({}), nss.db());

    // Fallback to splitVector command in case of mixed binaries not supporting autoSplitVector
    bool fallback = [&]() {
        auto status = Shard::CommandResponse::getEffectiveStatus(cmdStatus);
        return !status.isOK() && status.code() == ErrorCodes::CommandNotFound;
    }();

    if (fallback) {
        BSONObjBuilder cmd;
        cmd.append("splitVector", nss.ns());
        cmd.append("keyPattern", shardKeyPattern.toBSON());
        chunkRange.append(&cmd);
        cmd.append("maxChunkSizeBytes", chunkSizeBytes);
        cmdStatus = invokeSplitCommand(cmd.obj(), NamespaceString::kAdminDb);
    }


    auto status = Shard::CommandResponse::getEffectiveStatus(cmdStatus);
    if (!status.isOK()) {
        return status;
    }

    if (fallback) {
        const auto response = std::move(cmdStatus.getValue().response);
        std::vector<BSONObj> splitPoints;

        BSONObjIterator it(response.getObjectField("splitKeys"));
        while (it.more()) {
            splitPoints.push_back(it.next().Obj().getOwned());
        }
        return std::move(splitPoints);
    }

    const auto response = AutoSplitVectorResponse::parse(
        IDLParserErrorContext("AutoSplitVectorResponse"), std::move(cmdStatus.getValue().response));
    return response.getSplitKeys();

}  // namespace shardutil

StatusWith<boost::optional<ChunkRange>> splitChunkAtMultiplePoints(
    OperationContext* opCtx,
    const ShardId& shardId,
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    ChunkVersion collectionVersion,
    const ChunkRange& chunkRange,
    std::vector<BSONObj>* splitPoints) {
    invariant(!splitPoints->empty());

    if (splitPoints->size() > kMaxSplitPoints) {
        warning() << "Unable to apply all the" << splitPoints->size()
                  << "split points received. Only the first " << shardutil::kMaxSplitPoints
                  << "will be processed";
        splitPoints->resize(kMaxSplitPoints);
    }

    // Sanity check that we are not attempting to split at the boundaries of the chunk. This check
    // is already performed at chunk split commit time, but we are performing it here for parity
    // with old auto-split code, which might rely on it.
    const auto& firstSplitPoint = splitPoints->front();

    if (SimpleBSONObjComparator::kInstance.evaluate(chunkRange.getMin() == firstSplitPoint)) {
        const std::string msg(str::stream()
                              << "not splitting chunk " << chunkRange.toString() << ", split point "
                              << firstSplitPoint << " is exactly on chunk bounds");
        return {ErrorCodes::CannotSplit, msg};
    }

    const auto& lastSplitPoint = splitPoints->back();
    if (SimpleBSONObjComparator::kInstance.evaluate(chunkRange.getMax() == lastSplitPoint)) {
        const std::string msg(str::stream()
                              << "not splitting chunk " << chunkRange.toString() << ", split point "
                              << lastSplitPoint << " is exactly on chunk bounds");
        return {ErrorCodes::CannotSplit, msg};
    }

    BSONObjBuilder cmd;
    cmd.append("splitChunk", nss.ns());
    cmd.append("from", shardId.toString());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    cmd.append("epoch", collectionVersion.epoch());
    collectionVersion.appendWithField(
        &cmd, ChunkVersion::kShardVersionField);  // backwards compatibility with v3.4
    chunkRange.append(&cmd);
    cmd.append("splitKeys", *splitPoints);

    BSONObj cmdObj = cmd.obj();

    Status status{ErrorCodes::InternalError, "Uninitialized value"};
    BSONObj cmdResponse;

    auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!shardStatus.isOK()) {
        status = shardStatus.getStatus();
    } else {
        auto cmdStatus = shardStatus.getValue()->runCommandWithFixedRetryAttempts(
            opCtx,
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
        log() << "Split chunk " << redact(cmdObj) << " failed" << causedBy(redact(status));
        return status.withContext("split failed");
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
            << redact(cmdResponse) << ". Extracting " << kShouldMigrate << " field failed"
            << causedBy(redact(status));
    }

    return boost::optional<ChunkRange>();
}

}  // namespace shardutil
}  // namespace mongo
