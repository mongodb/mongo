/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/db/s/config/initial_split_policy.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/logical_clock.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/log.h"

namespace mongo {

/*
 * Creates a chunk based on the given arguments, appends it to 'chunks', and
 * increments the given chunk version
 */
void appendChunk(const NamespaceString& nss,
                 const BSONObj& min,
                 const BSONObj& max,
                 ChunkVersion* version,
                 const Timestamp& validAfter,
                 const ShardId& shardId,
                 std::vector<ChunkType>* chunks) {
    chunks->emplace_back(nss, ChunkRange(min, max), *version, shardId);
    auto& chunk = chunks->back();
    chunk.setHistory({ChunkHistory(validAfter, shardId)});
    version->incMinor();
}

/*
 * Returns a map mapping each tag name to a vector of shard ids with that tag name
 */
StringMap<std::vector<ShardId>> getTagToShardIds(OperationContext* opCtx,
                                                 const std::vector<TagsType>& tags) {
    StringMap<std::vector<ShardId>> tagToShardIds;
    if (tags.empty()) {
        return tagToShardIds;
    }

    // get all docs in config.shards
    auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findShardsStatus =
        configServer->exhaustiveFindOnConfig(opCtx,
                                             ReadPreferenceSetting(ReadPreference::Nearest),
                                             repl::ReadConcernLevel::kMajorityReadConcern,
                                             ShardType::ConfigNS,
                                             BSONObj(),
                                             BSONObj(),
                                             0);
    uassertStatusOK(findShardsStatus);
    uassert(ErrorCodes::InternalError,
            str::stream() << "cannot find any shard documents",
            !findShardsStatus.getValue().docs.empty());

    for (const auto& tag : tags) {
        tagToShardIds[tag.getTag()] = {};
    }

    const auto& shardDocList = findShardsStatus.getValue().docs;

    for (const auto& shardDoc : shardDocList) {
        auto shardParseStatus = ShardType::fromBSON(shardDoc);
        uassertStatusOK(shardParseStatus);
        auto parsedShard = shardParseStatus.getValue();
        for (const auto& tag : parsedShard.getTags()) {
            auto it = tagToShardIds.find(tag);
            if (it != tagToShardIds.end()) {
                it->second.push_back(parsedShard.getName());
            }
        }
    }

    return tagToShardIds;
}

void InitialSplitPolicy::calculateHashedSplitPointsForEmptyCollection(
    const ShardKeyPattern& shardKeyPattern,
    bool isEmpty,
    int numShards,
    int numInitialChunks,
    std::vector<BSONObj>* initialSplitPoints,
    std::vector<BSONObj>* finalSplitPoints) {
    if (!shardKeyPattern.isHashedPattern() || !isEmpty) {
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "numInitialChunks is not supported when the collection is not "
                              << (!shardKeyPattern.isHashedPattern() ? "hashed" : "empty"),
                !numInitialChunks);
        return;
    }

    // no split points are needed
    if (numInitialChunks == 1) {
        return;
    }

    // If initial split points are not specified, only pre-split when using a hashed shard key and
    // the collection is empty
    if (numInitialChunks <= 0) {
        // Default the number of initial chunks it they are not specified
        numInitialChunks = 2 * numShards;
    }

    // Hashes are signed, 64-bit integers. So we divide the range (-MIN long, +MAX long) into
    // intervals of size (2^64/numInitialChunks) and create split points at the boundaries.
    //
    // The logic below ensures that initial chunks are all symmetric around 0.
    const long long intervalSize = (std::numeric_limits<long long>::max() / numInitialChunks) * 2;
    long long current = 0;

    const auto proposedKey(shardKeyPattern.getKeyPattern().toBSON());

    if (numInitialChunks % 2 == 0) {
        finalSplitPoints->push_back(BSON(proposedKey.firstElementFieldName() << current));
        current += intervalSize;
    } else {
        current += intervalSize / 2;
    }

    for (int i = 0; i < (numInitialChunks - 1) / 2; i++) {
        finalSplitPoints->push_back(BSON(proposedKey.firstElementFieldName() << current));
        finalSplitPoints->push_back(BSON(proposedKey.firstElementFieldName() << -current));
        current += intervalSize;
    }

    sort(finalSplitPoints->begin(),
         finalSplitPoints->end(),
         SimpleBSONObjComparator::kInstance.makeLessThan());

    // The initial splits define the "big chunks" that we will subdivide later.
    int lastIndex = -1;
    for (int i = 1; i < numShards; i++) {
        if (lastIndex < (i * numInitialChunks) / numShards - 1) {
            lastIndex = (i * numInitialChunks) / numShards - 1;
            initialSplitPoints->push_back(finalSplitPoints->at(lastIndex));
        }
    }
}

InitialSplitPolicy::ShardCollectionConfig InitialSplitPolicy::generateShardCollectionInitialChunks(
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    const ShardId& databasePrimaryShardId,
    const Timestamp& validAfter,
    const std::vector<BSONObj>& splitPoints,
    const std::vector<ShardId>& allShardIds,
    const int numContiguousChunksPerShard) {
    invariant(!allShardIds.empty());

    ChunkVersion version(1, 0, OID::gen());
    const auto& keyPattern(shardKeyPattern.getKeyPattern());

    std::vector<ChunkType> chunks;

    for (size_t i = 0; i <= splitPoints.size(); i++) {
        const BSONObj min = (i == 0) ? keyPattern.globalMin() : splitPoints[i - 1];
        const BSONObj max = (i < splitPoints.size()) ? splitPoints[i] : keyPattern.globalMax();

        // It's possible there are no split points or fewer split points than total number of
        // shards, and we need to be sure that at least one chunk is placed on the primary shard
        const ShardId shardId = (i == 0 && splitPoints.size() + 1 < allShardIds.size())
            ? databasePrimaryShardId
            : allShardIds[(i / numContiguousChunksPerShard) % allShardIds.size()];

        appendChunk(nss, min, max, &version, validAfter, shardId, &chunks);
    }

    log() << "Created " << chunks.size() << " chunk(s) for: " << nss << " using new epoch "
          << version.epoch();

    return {std::move(chunks)};
}

InitialSplitPolicy::ShardCollectionConfig
InitialSplitPolicy::generateShardCollectionInitialZonedChunks(
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    const Timestamp& validAfter,
    const std::vector<TagsType>& tags,
    const StringMap<std::vector<ShardId>>& tagToShards,
    const std::vector<ShardId>& allShardIds) {
    invariant(!allShardIds.empty());

    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "cannot find zone split points because no zone docs were found",
            !tags.empty());

    ChunkVersion version(1, 0, OID::gen());
    const auto& keyPattern = shardKeyPattern.getKeyPattern();
    auto lastChunkMax = keyPattern.globalMin();
    int indx = 0;

    std::vector<ChunkType> chunks;

    for (const auto& tag : tags) {
        if (tag.getMinKey().woCompare(lastChunkMax) > 0) {
            // create a chunk for the hole between zones
            const ShardId shardId = allShardIds[indx++ % allShardIds.size()];
            appendChunk(nss, lastChunkMax, tag.getMinKey(), &version, validAfter, shardId, &chunks);
        }
        // create a chunk for the zone
        appendChunk(nss,
                    tag.getMinKey(),
                    tag.getMaxKey(),
                    &version,
                    validAfter,
                    tagToShards.find(tag.getTag())->second[0],
                    &chunks);
        lastChunkMax = tag.getMaxKey();
    }
    if (lastChunkMax.woCompare(keyPattern.globalMax()) < 0) {
        // existing zones do not span to $maxKey so create a chunk for that
        const ShardId shardId = allShardIds[indx++ % allShardIds.size()];
        appendChunk(
            nss, lastChunkMax, keyPattern.globalMax(), &version, validAfter, shardId, &chunks);
    }

    log() << "Created " << chunks.size() << " chunk(s) for: " << nss << " using new epoch "
          << version.epoch();

    return {std::move(chunks)};
}

InitialSplitPolicy::ShardCollectionConfig InitialSplitPolicy::writeFirstChunksToConfig(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    const ShardId& primaryShardId,
    const std::vector<BSONObj>& splitPoints,
    const std::vector<TagsType>& tags,
    const bool distributeInitialChunks,
    const int numContiguousChunksPerShard) {
    const auto& keyPattern = shardKeyPattern.getKeyPattern();

    std::vector<BSONObj> finalSplitPoints;
    std::vector<ShardId> shardIds;

    if (splitPoints.empty() && tags.empty()) {
        // If neither split points nor tags were specified use the shard's data distribution to
        // determine them
        auto primaryShard =
            uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, primaryShardId));

        auto result = uassertStatusOK(primaryShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryPreferred},
            nss.db().toString(),
            BSON("count" << nss.coll()),
            Shard::RetryPolicy::kIdempotent));

        long long numObjects = 0;
        uassertStatusOK(result.commandStatus);
        uassertStatusOK(bsonExtractIntegerField(result.response, "n", &numObjects));

        // Refresh the balancer settings to ensure the chunk size setting, which is sent as part of
        // the splitVector command and affects the number of chunks returned, has been loaded.
        uassertStatusOK(Grid::get(opCtx)->getBalancerConfiguration()->refreshAndCheck(opCtx));

        if (numObjects > 0) {
            finalSplitPoints = uassertStatusOK(shardutil::selectChunkSplitPoints(
                opCtx,
                primaryShardId,
                nss,
                shardKeyPattern,
                ChunkRange(keyPattern.globalMin(), keyPattern.globalMax()),
                Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes(),
                0));
        }

        // If docs already exist for the collection, must use primary shard,
        // otherwise defer to passed-in distribution option.
        if (numObjects == 0 && distributeInitialChunks) {
            Grid::get(opCtx)->shardRegistry()->getAllShardIdsNoReload(&shardIds);
        } else {
            shardIds.push_back(primaryShardId);
        }
    } else {
        // Make sure points are unique and ordered
        auto orderedPts = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

        for (const auto& splitPoint : splitPoints) {
            orderedPts.insert(splitPoint);
        }

        for (const auto& splitPoint : orderedPts) {
            finalSplitPoints.push_back(splitPoint);
        }

        if (distributeInitialChunks) {
            Grid::get(opCtx)->shardRegistry()->getAllShardIdsNoReload(&shardIds);
        } else {
            shardIds.push_back(primaryShardId);
        }
    }

    const auto tagToShards = getTagToShardIds(opCtx, tags);
    const Timestamp& validAfter = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();

    uassert(ErrorCodes::InternalError,
            str::stream() << "cannot generate initial chunks based on both split points and tags",
            tags.empty() || finalSplitPoints.empty());

    auto initialChunks = tags.empty()
        ? InitialSplitPolicy::generateShardCollectionInitialChunks(nss,
                                                                   shardKeyPattern,
                                                                   primaryShardId,
                                                                   validAfter,
                                                                   finalSplitPoints,
                                                                   shardIds,
                                                                   numContiguousChunksPerShard)
        : InitialSplitPolicy::generateShardCollectionInitialZonedChunks(
              nss, shardKeyPattern, validAfter, tags, tagToShards, shardIds);
    for (const auto& chunk : initialChunks.chunks) {
        uassertStatusOK(Grid::get(opCtx)->catalogClient()->insertConfigDocument(
            opCtx,
            ChunkType::ConfigNS,
            chunk.toConfigBSON(),
            ShardingCatalogClient::kMajorityWriteConcern));
    }

    return initialChunks;
}

}  // namespace mongo
