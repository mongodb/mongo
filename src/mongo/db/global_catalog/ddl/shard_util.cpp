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


#include "mongo/db/global_catalog/ddl/shard_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/request_types/auto_split_vector_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace shardutil {

StatusWith<long long> retrieveCollectionShardSize(OperationContext* opCtx,
                                                  const ShardId& shardId,
                                                  NamespaceString const& ns,
                                                  bool estimate) {
    auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }

    const Minutes maxTimeMSOverride{10};
    const auto cmdObj =
        BSON("dataSize" << NamespaceStringUtil::serialize(ns, SerializationContext::stateDefault())
                        << "estimate" << estimate);
    auto statStatus =
        shardStatus.getValue()->runCommand(opCtx,
                                           ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                                           ns.dbName(),
                                           cmdObj,
                                           maxTimeMSOverride,
                                           Shard::RetryPolicy::kIdempotent);

    auto stat = Shard::CommandResponse::getEffectiveStatus(statStatus);
    if (!stat.isOK()) {
        if (stat == ErrorCodes::NamespaceNotFound) {
            return 0;
        }
        return stat;
    }


    BSONElement sizeElem = statStatus.getValue().response["size"];
    if (!sizeElem.isNumber()) {
        return {ErrorCodes::NoSuchKey, "size field not found in dataSize"};
    }

    return sizeElem.safeNumberLong();
}


StatusWith<std::vector<BSONObj>> selectChunkSplitPoints(OperationContext* opCtx,
                                                        const ShardId& shardId,
                                                        const NamespaceString& nss,
                                                        const ShardKeyPattern& shardKeyPattern,
                                                        const ChunkRange& chunkRange,
                                                        long long chunkSizeBytes,
                                                        boost::optional<int> limit) {
    auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!shardStatus.isOK()) {
        return shardStatus.getStatus();
    }

    AutoSplitVectorRequest req(
        nss, shardKeyPattern.toBSON(), chunkRange.getMin(), chunkRange.getMax(), chunkSizeBytes);
    req.setLimit(limit);

    auto cmdStatus =
        shardStatus.getValue()->runCommand(opCtx,
                                           ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
                                           nss.dbName(),
                                           req.toBSON(),
                                           Shard::RetryPolicy::kIdempotent);

    auto status = Shard::CommandResponse::getEffectiveStatus(cmdStatus);
    if (!status.isOK()) {
        return status;
    }

    const auto response = AutoSplitVectorResponse::parse(
        std::move(cmdStatus.getValue().response), IDLParserContext("AutoSplitVectorResponse"));
    return response.getSplitKeys();
}

Status splitChunkAtMultiplePoints(OperationContext* opCtx,
                                  const ShardId& shardId,
                                  const NamespaceString& nss,
                                  const ShardKeyPattern& shardKeyPattern,
                                  const OID& epoch,
                                  const Timestamp& timestamp,
                                  const ChunkRange& chunkRange,
                                  const std::vector<BSONObj>& splitPoints) {
    invariant(!splitPoints.empty());

    auto splitPointsBeginIt = splitPoints.begin();
    auto splitPointsEndIt = splitPoints.end();

    if (splitPoints.size() > kMaxSplitPoints) {
        LOGV2_WARNING(6320300,
                      "Unable to apply all the split points received. Only the first "
                      "kMaxSplitPoints will be processed",
                      "numSplitPointsReceived"_attr = splitPoints.size(),
                      "kMaxSplitPoints"_attr = kMaxSplitPoints);
        splitPointsEndIt = std::next(splitPointsBeginIt, kMaxSplitPoints);
    }

    // Sanity check that we are not attempting to split at the boundaries of the chunk. This check
    // is already performed at chunk split commit time, but we are performing it here for parity
    // with old auto-split code, which might rely on it.
    if (SimpleBSONObjComparator::kInstance.evaluate(chunkRange.getMin() == *splitPointsBeginIt)) {
        const std::string msg(str::stream()
                              << "not splitting chunk " << chunkRange.toString() << ", split point "
                              << *splitPointsBeginIt << " is exactly on chunk bounds");
        return {ErrorCodes::CannotSplit, msg};
    }

    const auto& lastSplitPoint = *std::prev(splitPointsEndIt);
    if (SimpleBSONObjComparator::kInstance.evaluate(chunkRange.getMax() == lastSplitPoint)) {
        const std::string msg(str::stream()
                              << "not splitting chunk " << chunkRange.toString() << ", split point "
                              << lastSplitPoint << " is exactly on chunk bounds");
        return {ErrorCodes::CannotSplit, msg};
    }

    BSONObjBuilder cmd;
    cmd.append("splitChunk",
               NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    cmd.append("from", shardId.toString());
    cmd.append("keyPattern", shardKeyPattern.toBSON());
    cmd.append("epoch", epoch);
    cmd.append("timestamp", timestamp);

    chunkRange.serialize(&cmd);
    cmd.append("splitKeys", splitPointsBeginIt, splitPointsEndIt);

    BSONObj cmdObj = cmd.obj();

    Status status{ErrorCodes::InternalError, "Uninitialized value"};
    BSONObj cmdResponse;

    auto shardStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!shardStatus.isOK()) {
        status = shardStatus.getStatus();
    } else {
        auto cmdStatus =
            shardStatus.getValue()->runCommand(opCtx,
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               DatabaseName::kAdmin,
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
        LOGV2(22878,
              "Split chunk request against shard failed",
              "request"_attr = redact(cmdObj),
              "shardId"_attr = shardId,
              "error"_attr = redact(status));
        return status.withContext("split failed");
    }

    return Status::OK();
}

}  // namespace shardutil
}  // namespace mongo
