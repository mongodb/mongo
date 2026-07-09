/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/global_catalog/chunks_test_util.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::chunks_test_util {
namespace {

PseudoRandom _random{SecureRandom().nextInt64()};

std::vector<ChunkHistory> genChunkHistory(const ShardId& currentShard,
                                          const Timestamp& onCurrentShardSince,
                                          size_t numShards,
                                          size_t maxLenght) {
    std::vector<ChunkHistory> history;
    const auto historyLength = _random.nextInt64(maxLenght);
    auto lastTime = onCurrentShardSince;
    for (int64_t i = 0; i < historyLength; i++) {
        auto shard = i == 0 ? currentShard : getShardId(_random.nextInt64(numShards));
        history.emplace_back(onCurrentShardSince, shard);
        lastTime = lastTime - 1 - _random.nextInt64(10000);
    }
    return history;
}

}  // namespace

void assertEqualChunkInfo(const ChunkInfo& x, const ChunkInfo& y) {
    ASSERT_BSONOBJ_EQ(x.getMin(), y.getMin());
    ASSERT_BSONOBJ_EQ(x.getMax(), y.getMax());
    ASSERT_EQ(x.getMaxKeyString(), y.getMaxKeyString());
    ASSERT_EQ(x.getShardId(), y.getShardId());
    ASSERT_EQ(x.getLastmod(), y.getLastmod());
    ASSERT_EQ(x.isJumbo(), y.isJumbo());
    ASSERT_EQ(x.getHistory(), y.getHistory());
}

ShardId getShardId(int shardIdx) {
    return {std::string(str::stream() << "shard_" << shardIdx)};
}

std::vector<BSONObj> genRandomSplitPoints(size_t numChunks) {
    std::vector<BSONObj> splitPoints;
    splitPoints.reserve(numChunks + 1);
    splitPoints.emplace_back(kShardKeyPattern.globalMin());
    int64_t nextSplit{-1000};
    for (size_t i = 0; i < numChunks - 1; ++i) {
        nextSplit += 10 * (_random.nextInt32(10) + 1);
        splitPoints.emplace_back(BSON(kSKey << nextSplit));
    }
    splitPoints.emplace_back(kShardKeyPattern.globalMax());
    return splitPoints;
}

std::vector<ChunkVersion> genRandomVersions(size_t num, const ChunkVersion& initialVersion) {
    std::vector<ChunkVersion> versions;
    versions.reserve(num);
    auto major = initialVersion.majorVersion();
    auto minor = initialVersion.minorVersion();

    for (size_t i = 0; i < num; ++i) {
        if (_random.nextInt32(2)) {
            ++major;
            minor = 0;
        } else {
            ++minor;
        }
        versions.emplace_back(
            CollectionGeneration(initialVersion.epoch(), initialVersion.getTimestamp()),
            CollectionPlacement(major, minor));
    }
    std::shuffle(versions.begin(), versions.end(), _random.urbg());
    return versions;
}

std::vector<ChunkType> genChunkVector(const UUID& uuid,
                                      const std::vector<BSONObj>& splitPoints,
                                      const ChunkVersion& initialVersion,
                                      size_t numShards) {

    return genChunkVector(
        uuid, splitPoints, genRandomVersions(splitPoints.size() - 1, initialVersion), numShards);
}

std::vector<ChunkType> genChunkVector(const UUID& uuid,
                                      const std::vector<BSONObj>& splitPoints,
                                      const std::vector<ChunkVersion>& versions,
                                      size_t numShards) {

    invariant(SimpleBSONObjComparator::kInstance.evaluate(splitPoints.front() ==
                                                          kShardKeyPattern.globalMin()));
    invariant(SimpleBSONObjComparator::kInstance.evaluate(splitPoints.back() ==
                                                          kShardKeyPattern.globalMax()));
    const auto numChunks = splitPoints.size() - 1;
    invariant(numChunks == versions.size());

    Timestamp oldestValidAfter{Date_t::now() - Seconds{1000}};
    std::vector<ChunkType> chunks;
    chunks.reserve(numChunks);
    auto minKey = splitPoints.front();
    for (size_t i = 0; i < numChunks; ++i) {
        auto maxKey = splitPoints.at(i + 1);
        const auto shard = getShardId(_random.nextInt64(numShards));
        const auto version = versions.at(i);
        ChunkType chunk{uuid, ChunkRange{minKey, maxKey}, version, shard};
        chunk.setHistory(
            genChunkHistory(shard,
                            oldestValidAfter + version.majorVersion() * 10 + version.minorVersion(),
                            numShards,
                            10 /* maxLenght */));
        chunks.emplace_back(std::move(chunk));
        minKey = std::move(maxKey);
    }
    return chunks;
}

std::map<ShardId, Timestamp> calculateShardsMaxValidAfter(
    const std::vector<ChunkType>& chunkVector) {

    std::map<ShardId, Timestamp> vaMap;
    for (const auto& chunk : chunkVector) {
        if (chunk.getHistory().empty())
            continue;

        const auto& chunkMaxValidAfter = chunk.getHistory().front().getValidAfter();
        auto mapIt = vaMap.find(chunk.getShard());
        if (mapIt == vaMap.end()) {
            vaMap.emplace(chunk.getShard(), chunkMaxValidAfter);
            continue;
        }
        if (chunkMaxValidAfter > mapIt->second) {
            mapIt->second = chunkMaxValidAfter;
        }
    }
    return vaMap;
}

ChunkVersion calculateCollVersion(const std::map<ShardId, ChunkVersion>& shardVersions) {
    return std::max_element(shardVersions.begin(),
                            shardVersions.end(),
                            [](const std::pair<ShardId, ChunkVersion>& p1,
                               const std::pair<ShardId, ChunkVersion>& p2) {
                                return (p1.second <=> p2.second) == std::partial_ordering::less;
                            })
        ->second;
}

std::map<ShardId, ChunkVersion> calculateShardVersions(const std::vector<ChunkType>& chunkVector) {
    std::map<ShardId, ChunkVersion> svMap;
    for (const auto& chunk : chunkVector) {
        auto mapIt = svMap.find(chunk.getShard());
        if (mapIt == svMap.end()) {
            svMap.emplace(chunk.getShard(), chunk.getVersion());
            continue;
        }
        if ((mapIt->second <=> chunk.getVersion()) == std::partial_ordering::less) {
            mapIt->second = chunk.getVersion();
        }
    }
    return svMap;
}

std::vector<ChunkType> genRandomChunkVector(const UUID& uuid,
                                            const OID& epoch,
                                            const Timestamp& timestamp,
                                            size_t maxNumChunks,
                                            size_t minNumChunks) {
    invariant(minNumChunks <= maxNumChunks);
    const auto numChunks = minNumChunks + _random.nextInt32((maxNumChunks - minNumChunks) + 1);
    const auto numShards = _random.nextInt32(numChunks) + 1;
    const ChunkVersion initialVersion{{epoch, timestamp}, {1, 0}};

    LOGV2(7162700,
          "Generating random chunk vector",
          "numChunks"_attr = numChunks,
          "numShards"_attr = numShards);

    return genChunkVector(uuid, genRandomSplitPoints(numChunks), initialVersion, numShards);
}

BSONObj calculateIntermediateShardKey(const BSONObj& leftKey,
                                      const BSONObj& rightKey,
                                      double minKeyProb,
                                      double maxKeyProb) {
    invariant(0 <= minKeyProb && minKeyProb <= 1, "minKeyProb out of range [0, 1]");
    invariant(0 <= maxKeyProb && maxKeyProb <= 1, "maxKeyProb out of range [0, 1]");

    if (_random.nextInt32(100) < minKeyProb * 100) {
        return leftKey;
    }

    if (_random.nextInt32(100) < maxKeyProb * 100) {
        return rightKey;
    }

    const auto isMinKey = leftKey.woCompare(kShardKeyPattern.globalMin()) == 0;
    const auto isMaxKey = rightKey.woCompare(kShardKeyPattern.globalMax()) == 0;

    int64_t splitPoint;
    if (isMinKey && isMaxKey) {
        // [min, max] -> split at 0
        splitPoint = 0;
    } else if (!isMinKey && !isMaxKey) {
        // [x, y] -> split in the middle
        auto min = leftKey.firstElement().numberLong();
        auto max = rightKey.firstElement().numberLong();
        invariant(min + 1 < max,
                  str::stream() << "Can't split range [" << min << ", " << max << "]");
        splitPoint = min + ((max - min) / 2);
    } else if (isMaxKey) {
        // [x, maxKey] -> split at x*2;
        auto prevBound = leftKey.firstElement().numberLong();
        auto increment = prevBound ? prevBound : _random.nextInt32(100) + 1;
        splitPoint = prevBound + std::abs(increment);
    } else if (isMinKey) {
        // [minKey, x] -> split at x*2;
        auto prevBound = rightKey.firstElement().numberLong();
        auto increment = prevBound ? prevBound : _random.nextInt32(100) + 1;
        splitPoint = prevBound - std::abs(increment);
    } else {
        MONGO_UNREACHABLE;
    }

    return BSON(kSKey << splitPoint);
}

void performRandomChunkOperations(std::vector<ChunkType>* chunksPtr, size_t numOperations) {
    auto& chunks = *chunksPtr;
    auto collVersion = calculateCollVersion(calculateShardVersions(chunks));

    auto moveChunk = [&] {
        auto& chunkToMigrate = chunks[_random.nextInt32(chunks.size())];
        collVersion.incMajor();

        auto controlChunkIt = std::find_if(chunks.begin(), chunks.end(), [&](const auto& chunk) {
            return chunk.getShard() == chunkToMigrate.getShard() &&
                !chunk.getRange().overlaps(chunkToMigrate.getRange());
        });
        if (controlChunkIt != chunks.end()) {
            controlChunkIt->setVersion(collVersion);
            collVersion.incMinor();
        }
        auto newShard = getShardId(_random.nextInt64(chunks.size()));
        chunkToMigrate.setShard(newShard);
        chunkToMigrate.setVersion(collVersion);
        chunkToMigrate.setHistory([&] {
            auto history = chunkToMigrate.getHistory();
            history.emplace(history.begin(), Timestamp{Date_t::now()}, newShard);
            return history;
        }());
    };

    auto splitChunk = [&] {
        auto chunkToSplitIt = chunks.begin() + _random.nextInt32(chunks.size());
        while (chunkToSplitIt != chunks.begin() && chunkToSplitIt != std::prev(chunks.end()) &&
               (chunkToSplitIt->getMax().firstElement().numberLong() -
                chunkToSplitIt->getMin().firstElement().numberLong()) < 2) {
            // If the chunk is unsplittable select another one
            chunkToSplitIt = chunks.begin() + _random.nextInt32(chunks.size());
        }

        const auto& chunkToSplit = *chunkToSplitIt;

        auto splitKey = calculateIntermediateShardKey(chunkToSplit.getMin(), chunkToSplit.getMax());

        collVersion.incMinor();
        const ChunkRange leftRange{chunkToSplit.getMin(), splitKey};
        ChunkType leftChunk{
            chunkToSplit.getCollectionUUID(), leftRange, collVersion, chunkToSplit.getShard()};
        leftChunk.setHistory(chunkToSplit.getHistory());

        collVersion.incMinor();
        const ChunkRange rightRange{splitKey, chunkToSplit.getMax()};
        ChunkType rightChunk{
            chunkToSplit.getCollectionUUID(), rightRange, collVersion, chunkToSplit.getShard()};
        rightChunk.setHistory(chunkToSplit.getHistory());

        auto it = chunks.erase(chunkToSplitIt);
        it = chunks.insert(it, std::move(rightChunk));
        it = chunks.insert(it, std::move(leftChunk));
    };

    auto mergeChunks = [&] {
        const auto firstChunkIt = chunks.begin() + _random.nextInt32(chunks.size());
        const auto& shardId = firstChunkIt->getShard();
        auto lastChunkIt = std::find_if(firstChunkIt, chunks.end(), [&](const auto& chunk) {
            return chunk.getShard() != shardId;
        });
        const auto numContiguosChunks = std::distance(firstChunkIt, lastChunkIt);
        if (numContiguosChunks < 2) {
            // nothing to merge
            return;
        }
        const auto numChunkToMerge = _random.nextInt32(numContiguosChunks - 1) + 2;
        lastChunkIt = firstChunkIt + numChunkToMerge;
        const auto& firstChunk = *firstChunkIt;
        collVersion.incMinor();
        const ChunkRange mergedRange{firstChunk.getMin(), std::prev(lastChunkIt)->getMax()};
        ChunkType mergedChunk{
            firstChunk.getCollectionUUID(), mergedRange, collVersion, firstChunk.getShard()};
        mergedChunk.setHistory({ChunkHistory{Timestamp{Date_t::now()}, firstChunk.getShard()}});

        auto it = chunks.erase(firstChunkIt, lastChunkIt);
        it = chunks.insert(it, mergedChunk);
    };

    for (size_t i = 0; i < numOperations; i++) {
        switch (_random.nextInt32(3)) {
            case 0:
                moveChunk();
                break;
            case 1:
                splitChunk();
                break;
            case 2:
                mergeChunks();
                break;
            default:
                MONGO_UNREACHABLE;
                break;
        }
    }
}

std::vector<std::shared_ptr<ChunkInfo>> toChunkInfoPtrVector(
    const std::vector<ChunkType>& chunkTypes) {
    std::vector<std::shared_ptr<ChunkInfo>> chunkPtrs;
    chunkPtrs.reserve(chunkTypes.size());
    for (const auto& chunkType : chunkTypes) {
        chunkPtrs.push_back(std::make_shared<ChunkInfo>(chunkType));
    }
    return chunkPtrs;
}

}  // namespace mongo::chunks_test_util
