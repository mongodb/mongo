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
namespace {

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

    // Get all docs in config.shards through a query instead of going through the shard registry
    // because we need the zones as well
    const auto configServer = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto shardDocs = uassertStatusOK(
        configServer->exhaustiveFindOnConfig(opCtx,
                                             ReadPreferenceSetting(ReadPreference::Nearest),
                                             repl::ReadConcernLevel::kMajorityReadConcern,
                                             ShardType::ConfigNS,
                                             BSONObj(),
                                             BSONObj(),
                                             0));
    uassert(50986, str::stream() << "Could not find any shard documents", !shardDocs.docs.empty());

    for (const auto& tag : tags) {
        tagToShardIds[tag.getTag()] = {};
    }

    for (const auto& shardDoc : shardDocs.docs) {
        auto parsedShard = uassertStatusOK(ShardType::fromBSON(shardDoc));
        for (const auto& tag : parsedShard.getTags()) {
            tagToShardIds[tag].push_back(parsedShard.getName());
        }
    }

    return tagToShardIds;
}

}  // namespace

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

    std::vector<BSONObj> finalSplitPoints;

    // Make sure points are unique and ordered
    auto orderedPts = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    for (const auto& splitPoint : splitPoints) {
        orderedPts.insert(splitPoint);
    }

    for (const auto& splitPoint : orderedPts) {
        finalSplitPoints.push_back(splitPoint);
    }

    ChunkVersion version(1, 0, OID::gen());
    const auto& keyPattern(shardKeyPattern.getKeyPattern());

    std::vector<ChunkType> chunks;

    for (size_t i = 0; i <= finalSplitPoints.size(); i++) {
        const BSONObj min = (i == 0) ? keyPattern.globalMin() : finalSplitPoints[i - 1];
        const BSONObj max =
            (i < finalSplitPoints.size()) ? finalSplitPoints[i] : keyPattern.globalMax();

        // It's possible there are no split points or fewer split points than total number of
        // shards, and we need to be sure that at least one chunk is placed on the primary shard
        const ShardId shardId = (i == 0 && finalSplitPoints.size() + 1 < allShardIds.size())
            ? databasePrimaryShardId
            : allShardIds[(i / numContiguousChunksPerShard) % allShardIds.size()];

        appendChunk(nss, min, max, &version, validAfter, shardId, &chunks);
    }

    return {std::move(chunks)};
}

InitialSplitPolicy::ShardCollectionConfig
InitialSplitPolicy::generateShardCollectionInitialZonedChunks(
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    const Timestamp& validAfter,
    const std::vector<TagsType>& tags,
    const StringMap<std::vector<ShardId>>& tagToShards,
    const std::vector<ShardId>& shardIdsForGaps) {
    invariant(!shardIdsForGaps.empty());
    invariant(!tags.empty());

    const auto& keyPattern = shardKeyPattern.getKeyPattern();

    auto nextShardIdForHole = [&, indx = 0]() mutable {
        return shardIdsForGaps[indx++ % shardIdsForGaps.size()];
    };

    auto nextShardIdForTag = [&, tagToIndx = StringMap<size_t>()](const auto& tag) mutable {
        const auto it = tagToShards.find(tag.getTag());
        invariant(it != tagToShards.end());
        const auto& shardIdsForTag = it->second;
        uassert(50973,
                str::stream()
                    << "Cannot shard collection " << nss.ns() << " due to zone " << tag.getTag()
                    << " which is not assigned to a shard. Please assign this zone to a shard.",
                !shardIdsForTag.empty());
        const auto nextShardIndx = tagToIndx[tag.getTag()]++;
        return shardIdsForTag[nextShardIndx % shardIdsForTag.size()];
    };

    std::vector<ChunkType> chunks;

    ChunkVersion version(1, 0, OID::gen());
    auto lastChunkMax = keyPattern.globalMin();

    for (const auto& tag : tags) {
        // Create a chunk for the hole [lastChunkMax, tag.getMinKey)
        if (tag.getMinKey().woCompare(lastChunkMax) > 0) {
            appendChunk(nss,
                        lastChunkMax,
                        tag.getMinKey(),
                        &version,
                        validAfter,
                        nextShardIdForHole(),
                        &chunks);
        }

        // Create chunk for the actual tag - [tag.getMinKey, tag.getMaxKey)
        appendChunk(nss,
                    tag.getMinKey(),
                    tag.getMaxKey(),
                    &version,
                    validAfter,
                    nextShardIdForTag(tag),
                    &chunks);
        lastChunkMax = tag.getMaxKey();
    }

    // Create a chunk for the hole [lastChunkMax, MaxKey]
    if (lastChunkMax.woCompare(keyPattern.globalMax()) < 0) {
        appendChunk(nss,
                    lastChunkMax,
                    keyPattern.globalMax(),
                    &version,
                    validAfter,
                    nextShardIdForHole(),
                    &chunks);
    }

    return {std::move(chunks)};
}

InitialSplitPolicy::ShardingOptimizationType InitialSplitPolicy::calculateOptimizationType(
    const std::vector<BSONObj>& splitPoints,
    const std::vector<TagsType>& tags,
    bool collectionIsEmpty) {
    if (!splitPoints.empty()) {
        return SplitPointsProvided;
    }

    if (!tags.empty()) {
        if (collectionIsEmpty) {
            return TagsProvidedWithEmptyCollection;
        }
        return TagsProvidedWithNonEmptyCollection;
    }

    if (collectionIsEmpty) {
        return EmptyCollection;
    }

    return None;
}

InitialSplitPolicy::ShardCollectionConfig InitialSplitPolicy::createFirstChunksOptimized(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    const ShardId& primaryShardId,
    const std::vector<BSONObj>& splitPoints,
    const std::vector<TagsType>& tags,
    ShardingOptimizationType optimizationType,
    bool isEmpty,
    int numContiguousChunksPerShard) {
    uassert(ErrorCodes::InvalidOptions,
            "Cannot generate initial chunks based on both split points and zones",
            tags.empty() || splitPoints.empty());

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    const auto& keyPattern = shardKeyPattern.getKeyPattern();

    const auto validAfter = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();

    // On which shards are the generated chunks allowed to be placed
    std::vector<ShardId> shardIds;
    if (isEmpty) {
        shardRegistry->getAllShardIdsNoReload(&shardIds);
    } else {
        shardIds.push_back(primaryShardId);
    }

    ShardCollectionConfig initialChunks;

    if (optimizationType == ShardingOptimizationType::SplitPointsProvided) {
        initialChunks = generateShardCollectionInitialChunks(nss,
                                                             shardKeyPattern,
                                                             primaryShardId,
                                                             validAfter,
                                                             splitPoints,
                                                             shardIds,
                                                             numContiguousChunksPerShard);
    } else if (optimizationType == ShardingOptimizationType::TagsProvidedWithEmptyCollection) {
        initialChunks = generateShardCollectionInitialZonedChunks(
            nss, shardKeyPattern, validAfter, tags, getTagToShardIds(opCtx, tags), shardIds);
    } else if (optimizationType == ShardingOptimizationType::TagsProvidedWithNonEmptyCollection ||
               optimizationType == ShardingOptimizationType::EmptyCollection) {
        ChunkVersion version(1, 0, OID::gen());
        appendChunk(nss,
                    keyPattern.globalMin(),
                    keyPattern.globalMax(),
                    &version,
                    validAfter,
                    primaryShardId,
                    &initialChunks.chunks);
    } else {
        MONGO_UNREACHABLE;
    }

    return initialChunks;
}

InitialSplitPolicy::ShardCollectionConfig InitialSplitPolicy::createFirstChunksUnoptimized(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    const ShardId& primaryShardId) {
    const auto& keyPattern = shardKeyPattern.getKeyPattern();
    const auto validAfter = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();

    // On which shards are the generated chunks allowed to be placed
    std::vector<ShardId> shardIds{primaryShardId};

    auto primaryShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, primaryShardId));

    // Refresh the balancer settings to ensure the chunk size setting, which is sent as part of
    // the autoSplitVector command and affects the number of chunks returned, has been loaded.
    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));

    const auto shardSelectedSplitPoints = uassertStatusOK(shardutil::selectChunkSplitPoints(
        opCtx,
        primaryShardId,
        nss,
        shardKeyPattern,
        ChunkRange(keyPattern.globalMin(), keyPattern.globalMax()),
        balancerConfig->getMaxChunkSizeBytes()));

    return generateShardCollectionInitialChunks(nss,
                                                shardKeyPattern,
                                                primaryShardId,
                                                validAfter,
                                                shardSelectedSplitPoints,
                                                shardIds,
                                                1  // numContiguousChunksPerShard
    );
}

boost::optional<CollectionType> InitialSplitPolicy::checkIfCollectionAlreadyShardedWithSameOptions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardsvrShardCollection& request,
    repl::ReadConcernLevel readConcernLevel) {
    auto const catalogClient = Grid::get(opCtx)->catalogClient();

    auto collStatus = catalogClient->getCollection(opCtx, nss, readConcernLevel);
    if (collStatus == ErrorCodes::NamespaceNotFound) {
        // Not currently sharded.
        return boost::none;
    }

    uassertStatusOK(collStatus);
    auto existingOptions = collStatus.getValue().value;

    CollectionType requestedOptions;
    requestedOptions.setNs(nss);
    requestedOptions.setKeyPattern(KeyPattern(request.getKey()));
    requestedOptions.setDefaultCollation(*request.getCollation());
    requestedOptions.setUnique(request.getUnique());

    // If the collection is already sharded, fail if the deduced options in this request do not
    // match the options the collection was originally sharded with.
    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "sharding already enabled for collection " << nss.ns()
                          << " with options " << existingOptions.toString(),
            requestedOptions.hasSameOptions(existingOptions));

    return existingOptions;
}

}  // namespace mongo
