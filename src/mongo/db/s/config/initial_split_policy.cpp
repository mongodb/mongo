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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/initial_split_policy.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/logical_clock.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_util.h"

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
StringMap<std::vector<ShardId>> buildTagsToShardIdsMap(OperationContext* opCtx,
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

std::vector<BSONObj> InitialSplitPolicy::calculateHashedSplitPoints(
    const ShardKeyPattern& shardKeyPattern, BSONObj prefix, int numInitialChunks) {
    invariant(shardKeyPattern.isHashedPattern());
    invariant(numInitialChunks > 0);

    std::vector<BSONObj> splitPoints;
    if (numInitialChunks == 1) {
        return splitPoints;
    }

    // Hashes are signed, 64-bit integers. So we divide the range (-MIN long, +MAX long) into
    // intervals of size (2^64/numInitialChunks) and create split points at the boundaries.
    //
    // The logic below ensures that initial chunks are all symmetric around 0.
    const long long intervalSize = (std::numeric_limits<long long>::max() / numInitialChunks) * 2;
    long long current = 0;

    const auto proposedKey(shardKeyPattern.getKeyPattern().toBSON());

    auto buildSplitPoint = [&](long long value) {
        // Forward the iterator until hashed field is reached.
        auto shardKeyPatternItr = BSONObjIterator(shardKeyPattern.getKeyPattern().toBSON());
        while (shardKeyPattern.getHashedField().fieldNameStringData() !=
               (*shardKeyPatternItr++).fieldNameStringData()) {
        }

        // Append the prefix fields to the new splitpoint, if any such fields exist.
        BSONObjBuilder bob(prefix);

        // Append the value of the hashed field for the current splitpoint.
        bob.append(shardKeyPattern.getHashedField().fieldNameStringData(), value);

        // Set all subsequent shard key fields to MinKey.
        while (shardKeyPatternItr.more()) {
            bob.appendMinKey((*shardKeyPatternItr++).fieldNameStringData());
        }
        return bob.obj();
    };

    if (numInitialChunks % 2 == 0) {
        splitPoints.push_back(buildSplitPoint(current));
        current += intervalSize;
    } else {
        current += intervalSize / 2;
    }

    for (int i = 0; i < (numInitialChunks - 1) / 2; i++) {
        splitPoints.push_back(buildSplitPoint(current));
        splitPoints.push_back(buildSplitPoint(-current));
        current += intervalSize;
    }

    sort(splitPoints.begin(), splitPoints.end(), SimpleBSONObjComparator::kInstance.makeLessThan());
    return splitPoints;
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

std::unique_ptr<InitialSplitPolicy> InitialSplitPolicy::calculateOptimizationStrategy(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    const ShardsvrShardCollection& request,
    const std::vector<TagsType>& tags,
    size_t numShards,
    bool collectionIsEmpty) {
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "numInitialChunks is only supported when the collection is empty "
                             "and has a hashed field in the shard key pattern",
            !request.getNumInitialChunks() ||
                (shardKeyPattern.isHashedPattern() && collectionIsEmpty));
    uassert(ErrorCodes::InvalidOptions,
            str::stream()
                << "When the prefix of the hashed shard key is a range field, "
                   "'numInitialChunks' can only be used when the 'presplitHashedZones' is true",
            !request.getNumInitialChunks() || shardKeyPattern.hasHashedPrefix() ||
                request.getPresplitHashedZones());
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Cannot have both initial split points and tags set",
            !request.getInitialSplitPoints() || tags.empty());

    // If 'presplitHashedZones' flag is set, we always use 'PresplitHashedZonesSplitPolicy', to make
    // sure we throw the correct assertion if further validation fails.
    if (request.getPresplitHashedZones()) {
        return std::make_unique<PresplitHashedZonesSplitPolicy>(
            opCtx, shardKeyPattern, tags, request.getNumInitialChunks(), collectionIsEmpty);
    }

    // The next preference is to use split points based strategy. This is only possible if
    // 'initialSplitPoints' is set, or if the collection is empty with shard key having a hashed
    // prefix.
    if (request.getInitialSplitPoints()) {
        return std::make_unique<SplitPointsBasedSplitPolicy>(*request.getInitialSplitPoints());
    }
    if (tags.empty() && shardKeyPattern.hasHashedPrefix() && collectionIsEmpty) {
        return std::make_unique<SplitPointsBasedSplitPolicy>(
            shardKeyPattern, numShards, request.getNumInitialChunks());
    }

    if (!tags.empty()) {
        if (collectionIsEmpty) {
            return std::make_unique<SingleChunkPerTagSplitPolicy>(opCtx, tags);
        }
        return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
    }

    if (collectionIsEmpty) {
        return std::make_unique<SingleChunkOnPrimarySplitPolicy>();
    }
    return std::make_unique<UnoptimizedSplitPolicy>();
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

    // Set the distributionMode to "sharded" because this CollectionType represents the requested
    // target state for the collection after shardCollection. The requested CollectionType will be
    // compared with the existing CollectionType below, and if the existing CollectionType either
    // does not have a distributionMode (FCV 4.2) or has distributionMode "sharded" (FCV 4.4), the
    // collection will be considered to already be in its target state.
    requestedOptions.setDistributionMode(CollectionType::DistributionMode::kSharded);

    // If the collection is already sharded, fail if the deduced options in this request do not
    // match the options the collection was originally sharded with.
    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "sharding already enabled for collection " << nss.ns()
                          << " with options " << existingOptions.toString(),
            requestedOptions.hasSameOptions(existingOptions));

    return existingOptions;
}

InitialSplitPolicy::ShardCollectionConfig SingleChunkOnPrimarySplitPolicy::createFirstChunks(
    OperationContext* opCtx, const ShardKeyPattern& shardKeyPattern, SplitPolicyParams params) {
    ShardCollectionConfig initialChunks;
    ChunkVersion version(1, 0, OID::gen());
    const auto& keyPattern = shardKeyPattern.getKeyPattern();
    appendChunk(params.nss,
                keyPattern.globalMin(),
                keyPattern.globalMax(),
                &version,
                LogicalClock::get(opCtx)->getClusterTime().asTimestamp(),
                params.primaryShardId,
                &initialChunks.chunks);
    return initialChunks;
}

InitialSplitPolicy::ShardCollectionConfig UnoptimizedSplitPolicy::createFirstChunks(
    OperationContext* opCtx, const ShardKeyPattern& shardKeyPattern, SplitPolicyParams params) {
    // Under this policy, chunks are only placed on the primary shard.
    std::vector<ShardId> shardIds{params.primaryShardId};

    // Refresh the balancer settings to ensure the chunk size setting, which is sent as part of
    // the splitVector command and affects the number of chunks returned, has been loaded.
    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));
    const auto shardSelectedSplitPoints = uassertStatusOK(
        shardutil::selectChunkSplitPoints(opCtx,
                                          params.primaryShardId,
                                          params.nss,
                                          shardKeyPattern,
                                          ChunkRange(shardKeyPattern.getKeyPattern().globalMin(),
                                                     shardKeyPattern.getKeyPattern().globalMax()),
                                          balancerConfig->getMaxChunkSizeBytes(),
                                          0));
    return generateShardCollectionInitialChunks(
        params.nss,
        shardKeyPattern,
        params.primaryShardId,
        LogicalClock::get(opCtx)->getClusterTime().asTimestamp(),
        shardSelectedSplitPoints,
        shardIds,
        1  // numContiguousChunksPerShard
    );
}

InitialSplitPolicy::ShardCollectionConfig SplitPointsBasedSplitPolicy::createFirstChunks(
    OperationContext* opCtx, const ShardKeyPattern& shardKeyPattern, SplitPolicyParams params) {

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // On which shards are the generated chunks allowed to be placed.
    std::vector<ShardId> shardIds;
    shardRegistry->getAllShardIdsNoReload(&shardIds);

    return generateShardCollectionInitialChunks(
        params.nss,
        shardKeyPattern,
        params.primaryShardId,
        LogicalClock::get(opCtx)->getClusterTime().asTimestamp(),
        _splitPoints,
        shardIds,
        _numContiguousChunksPerShard);
}

AbstractTagsBasedSplitPolicy::AbstractTagsBasedSplitPolicy(OperationContext* opCtx,
                                                           std::vector<TagsType> tags)
    : _tags(tags) {
    _tagToShardIds = buildTagsToShardIdsMap(opCtx, tags);
}

AbstractTagsBasedSplitPolicy::SplitInfo SingleChunkPerTagSplitPolicy::buildSplitInfoForTag(
    TagsType tag, const ShardKeyPattern& shardKeyPattern) {
    const auto nextShardIndex = _nextShardIndexForZone[tag.getTag()]++;
    const auto& shardIdsForTag = getTagsToShardIds().find(tag.getTag())->second;
    auto shardId = shardIdsForTag[nextShardIndex % shardIdsForTag.size()];

    // Do not generate any split points when using this strategy. We create one chunk on a shard
    // choosen using round-robin.
    return {{}, {std::make_pair(shardId, 1)}};
}

InitialSplitPolicy::ShardCollectionConfig AbstractTagsBasedSplitPolicy::createFirstChunks(
    OperationContext* opCtx, const ShardKeyPattern& shardKeyPattern, SplitPolicyParams params) {
    invariant(!_tags.empty());

    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    std::vector<ShardId> shardIds;
    shardRegistry->getAllShardIdsNoReload(&shardIds);
    const auto validAfter = LogicalClock::get(opCtx)->getClusterTime().asTimestamp();
    const auto& keyPattern = shardKeyPattern.getKeyPattern();

    auto tagToShards = getTagsToShardIds();

    auto nextShardIdForHole = [&, indx = 0L]() mutable {
        return shardIds[indx++ % shardIds.size()];
    };

    ChunkVersion version(1, 0, OID::gen());
    auto lastChunkMax = keyPattern.globalMin();
    std::vector<ChunkType> chunks;
    for (const auto& tag : _tags) {
        // Create a chunk for the hole [lastChunkMax, tag.getMinKey)
        if (tag.getMinKey().woCompare(lastChunkMax) > 0) {
            appendChunk(params.nss,
                        lastChunkMax,
                        tag.getMinKey(),
                        &version,
                        validAfter,
                        nextShardIdForHole(),
                        &chunks);
        }
        // Create chunk for the actual tag - [tag.getMinKey, tag.getMaxKey)
        const auto it = tagToShards.find(tag.getTag());
        invariant(it != tagToShards.end());
        uassert(50973,
                str::stream()
                    << "Cannot shard collection " << params.nss.ns() << " due to zone "
                    << tag.getTag()
                    << " which is not assigned to a shard. Please assign this zone to a shard.",
                !it->second.empty());

        // The buildSplitInfoForTag() should provide split points which are in sorted order. So we
        // don't need to sort them again while generating chunks.
        auto splitInfo = buildSplitInfoForTag(tag, shardKeyPattern);

        // Ensure that the number of splitPoints is consistent with the computed chunk distribution.
        // The resulting number of chunks will be one more than the number of split points to
        // accommodate boundaries.
        invariant(splitInfo.splitPoints.size() + 1 ==
                  std::accumulate(splitInfo.chunkDistribution.begin(),
                                  splitInfo.chunkDistribution.end(),
                                  static_cast<size_t>(0),  // initial value for 'runningSum'.
                                  [](size_t runningSum, const auto& currentElem) {
                                      return runningSum + currentElem.second;
                                  }));

        // Generate chunks using 'splitPoints' and distribute them among shards based on
        // 'chunkDistributionPerShard'.
        size_t splitPointIdx = 0;
        for (auto&& chunksOnShard : splitInfo.chunkDistribution) {
            const auto [targetShard, numChunksForShard] = chunksOnShard;
            for (size_t i = 0; i < numChunksForShard; ++i, ++splitPointIdx) {
                const BSONObj min = (splitPointIdx == 0) ? tag.getMinKey()
                                                         : splitInfo.splitPoints[splitPointIdx - 1];
                const BSONObj max = (splitPointIdx == splitInfo.splitPoints.size())
                    ? tag.getMaxKey()
                    : splitInfo.splitPoints[splitPointIdx];
                appendChunk(params.nss, min, max, &version, validAfter, targetShard, &chunks);
            }
        }
        lastChunkMax = tag.getMaxKey();
    }

    // Create a chunk for the hole [lastChunkMax, MaxKey]
    if (lastChunkMax.woCompare(keyPattern.globalMax()) < 0) {
        appendChunk(params.nss,
                    lastChunkMax,
                    keyPattern.globalMax(),
                    &version,
                    validAfter,
                    nextShardIdForHole(),
                    &chunks);
    }

    return {std::move(chunks)};
}

AbstractTagsBasedSplitPolicy::SplitInfo PresplitHashedZonesSplitPolicy::buildSplitInfoForTag(
    TagsType tag, const ShardKeyPattern& shardKeyPattern) {
    // Returns the ceiling number for the decimal value of x/y.
    auto ceilOfXOverY = [](auto x, auto y) { return (x / y) + (x % y != 0); };

    // This strategy presplits each tag such that at least 1 chunk is placed on every shard to which
    // the tag is assigned. We distribute the chunks such that at least '_numInitialChunks' are
    // created across the cluster, and we make a best-effort attempt to ensure that an equal number
    // of chunks are created on each shard regardless of how the zones are laid out.

    //  We take the ceiling when the number is not divisible so that the final number of chunks
    //  we generate are at least '_numInitialChunks'.
    auto numChunksPerShard = ceilOfXOverY(_numInitialChunks, _numTagsPerShard.size());

    const auto& tagsToShardsMap = getTagsToShardIds();
    invariant(tagsToShardsMap.find(tag.getTag()) != tagsToShardsMap.end());
    const auto& shardsForCurrentTag = tagsToShardsMap.find(tag.getTag())->second;

    // For each shard in the current zone, find the quota of chunks that can be allocated to that
    // zone. We distribute chunks equally to all the zones present on a shard.
    std::vector<std::pair<ShardId, size_t>> chunkDistribution;
    chunkDistribution.reserve((shardsForCurrentTag.size()));
    auto numChunksForCurrentTag = 0;
    for (auto&& shard : shardsForCurrentTag) {
        auto numChunksForCurrentTagOnShard =
            ceilOfXOverY(numChunksPerShard, _numTagsPerShard[shard.toString()]);
        chunkDistribution.push_back({shard, numChunksForCurrentTagOnShard});
        numChunksForCurrentTag += (numChunksForCurrentTagOnShard);
    }

    // Extract the fields preceding the hashed field. We use this object as a base for building
    // split points.
    BSONObjBuilder bob;
    for (auto&& elem : tag.getMinKey()) {
        if (elem.fieldNameStringData() == shardKeyPattern.getHashedField().fieldNameStringData()) {
            break;
        }
        bob.append(elem);
    }
    auto prefixBSON = bob.obj();

    return {calculateHashedSplitPoints(shardKeyPattern, prefixBSON, numChunksForCurrentTag),
            std::move(chunkDistribution)};
}

PresplitHashedZonesSplitPolicy::PresplitHashedZonesSplitPolicy(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    std::vector<TagsType> tags,
    size_t numInitialChunks,
    bool isCollectionEmpty)
    : AbstractTagsBasedSplitPolicy(opCtx, tags) {
    // Verify that tags have been set up correctly for this split policy.
    _validate(shardKeyPattern, isCollectionEmpty);

    // Calculate the count of zones on each shard and save it in a map for later.
    const auto& tagsToShards = getTagsToShardIds();
    for (auto&& tag : tags) {
        auto& shardsForCurrentTag = tagsToShards.find(tag.getTag())->second;
        for (auto&& shard : shardsForCurrentTag) {
            _numTagsPerShard[shard.toString()]++;
        }
    }
    // If we are here, we have confirmed that at least one tag is already set up. A tag can only be
    // created if they are associated with a zone and the zone has to be assigned to a shard.
    invariant(!_numTagsPerShard.empty());

    // If 'numInitialChunks' was not specified, use default value.
    _numInitialChunks = numInitialChunks ? numInitialChunks : _numTagsPerShard.size() * 2;
}

/**
 * If 'presplitHashedZones' flag is set with shard key prefix being a non-hashed field then all
 * zones must be set up according to the following rules:
 *  1. All lower-bound prefix fields of the shard key must have a value other than MinKey or
 * MaxKey.
 *  2. All lower-bound fields from the hash field onwards must be MinKey.
 *  3. At least one upper-bound prefix field must be different than the lower bound counterpart.
 *
 * Examples for shard key {country : 1, hashedField: "hashed", suffix : 1}:
 * Zone with range : [{country : "US", hashedField: MinKey, suffix: MinKey}, {country :MaxKey,
 * hashedField: MaxKey, suffix: MaxKey}) is valid.
 * Zone with range : [{country : MinKey, hashedField: MinKey, suffix: MinKey}, {country : "US",
 * hashedField: MinKey, suffix: MinKey}) is invalid since it violates #1 rule.
 * Zone with range: [{country : "US", hashedField: MinKey, suffix: "someVal"}, {country :MaxKey,
 * hashedField: MaxKey, suffix: MaxKey}) is invalid since it violates #2 rule.
 * Zone with range: [{country : "US", hashedField: MinKey, suffix: MinKey}, {country : "US",
 * hashedField: MaxKey, suffix: MaxKey}) is invalid since it violates #3 rule.
 *
 * If the shard key has a hashed prefix, then pre-splitting is only supported if there is a single
 * zone defined from global MinKey to global MaxKey. i.e, if the shard key is {x: "hashed", y: 1}
 * then there should be exactly one zone ranging from {x:MinKey, y:MinKey} to {x:MaxKey, y:MaxKey}.
 */
void PresplitHashedZonesSplitPolicy::_validate(const ShardKeyPattern& shardKeyPattern,
                                               bool isCollectionEmpty) {
    const auto& tags = getTags();
    uassert(
        31387,
        "'presplitHashedZones' is only supported when the collection is empty, zones are set up "
        "and shard key pattern has a hashed field",
        isCollectionEmpty && !tags.empty() && shardKeyPattern.isHashedPattern());

    if (shardKeyPattern.hasHashedPrefix()) {
        uassert(31412,
                "For hashed prefix shard keys, 'presplitHashedZones' is only supported when there "
                "is a single zone defined which covers entire shard key range",
                (tags.size() == 1) &&
                    !shardKeyPattern.getKeyPattern().globalMin().woCompare(tags[0].getMinKey()) &&
                    !shardKeyPattern.getKeyPattern().globalMax().woCompare(tags[0].getMaxKey()));
        return;
    }
    for (auto&& tag : tags) {
        auto startItr = BSONObjIterator(tag.getMinKey());
        auto endItr = BSONObjIterator(tag.getMaxKey());

        // We cannot pre-split if the lower bound fields preceding the hashed field are same as
        // the upper bound. We validate that at least one of the preceding field is different.
        // Additionally we all make sure that none of the lower-bound prefix fields have Minkey
        // or MaxKey.
        bool isPrefixDifferent = false;
        do {
            uassert(31388,
                    str::stream()
                        << "One or more zones are not defined in a manner that supports hashed "
                           "pre-splitting. Cannot have MinKey or MaxKey in the lower bound for "
                           "fields preceding the hashed field but found one, for zone "
                        << tag.getTag(),
                    (*startItr).type() != BSONType::MinKey &&
                        (*startItr).type() != BSONType::MaxKey);
            isPrefixDifferent = isPrefixDifferent || (*startItr).woCompare(*endItr);
            ++endItr;
            // Forward the iterator until hashed field is reached.
        } while ((*++startItr).fieldNameStringData() !=
                 shardKeyPattern.getHashedField().fieldNameStringData());
        uassert(31390,
                str::stream() << "One or more zones are not defined in a manner that supports "
                                 "hashed pre-splitting. The value preceding hashed field of the "
                                 "upper bound should be greater than that of lower bound, for zone "
                              << tag.getTag(),
                isPrefixDifferent);

        uassert(
            31389,
            str::stream() << "One or more zones are not defined in a manner that supports "
                             "hashed pre-splitting. The hashed field value for lower bound must "
                             "be MinKey, for zone "
                          << tag.getTag(),
            (*startItr).type() == BSONType::MinKey);

        // Each field in the lower bound after the hashed field must be set to MinKey.
        while (startItr.more()) {
            uassert(31391,
                    str::stream() << "One or more zones are not defined in a manner that supports "
                                     "hashed pre-splitting. The fields after the hashed field must "
                                     "have MinKey value, for zone "
                                  << tag.getTag(),
                    (*startItr++).type() == BSONType::MinKey);
        }
    }
}

}  // namespace mongo
