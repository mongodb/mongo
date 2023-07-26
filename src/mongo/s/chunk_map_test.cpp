/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <absl/container/node_hash_map.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/shard_id.h"
#include "mongo/platform/random.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/chunks_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using chunks_test_util::assertEqualChunkInfo;
using chunks_test_util::calculateCollVersion;
using chunks_test_util::calculateIntermediateShardKey;
using chunks_test_util::genChunkVector;
using chunks_test_util::genRandomSplitPoints;
using chunks_test_util::performRandomChunkOperations;

namespace {

PseudoRandom _random{SecureRandom().nextInt64()};

const ShardId kThisShard("testShard");

ShardPlacementVersionMap getShardVersionMap(const ChunkMap& chunkMap) {
    return chunkMap.getShardPlacementVersionMap();
}

std::map<ShardId, ChunkVersion> calculateShardVersions(
    const std::vector<std::shared_ptr<ChunkInfo>>& chunkVector) {
    std::map<ShardId, ChunkVersion> svMap;
    for (const auto& chunk : chunkVector) {
        auto mapIt = svMap.find(chunk->getShardId());
        if (mapIt == svMap.end()) {
            svMap.emplace(chunk->getShardId(), chunk->getLastmod());
            continue;
        }
        if (mapIt->second.isOlderThan(chunk->getLastmod())) {
            mapIt->second = chunk->getLastmod();
        }
    }
    return svMap;
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

class ChunkMapTest : public unittest::Test {
public:
    const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const UUID& uuid() const {
        return _uuid;
    }

    const OID& collEpoch() const {
        return _epoch;
    }

    const Timestamp& collTimestamp() const {
        return _collTimestamp;
    }

    ChunkMap makeChunkMap(const std::vector<std::shared_ptr<ChunkInfo>>& chunks) const {
        const auto chunkBucketSize =
            static_cast<size_t>(_random.nextInt64(chunks.size() * 1.2) + 1);
        LOGV2(7162701, "Creating new chunk map", "chunkBucketSize"_attr = chunkBucketSize);
        return ChunkMap{collEpoch(), collTimestamp(), chunkBucketSize}.createMerged(chunks);
    }

    std::vector<ChunkType> genRandomChunkVector(size_t maxNumChunks = 30,
                                                size_t minNumChunks = 1) const {
        return chunks_test_util::genRandomChunkVector(
            _uuid, _epoch, _collTimestamp, maxNumChunks, minNumChunks);
    }

private:
    KeyPattern _shardKeyPattern{chunks_test_util::kShardKeyPattern};
    const UUID _uuid = UUID::gen();
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
};

TEST_F(ChunkMapTest, TestAddChunk) {
    ChunkVersion version({collEpoch(), collTimestamp()}, {1, 0});

    auto chunk = std::make_shared<ChunkInfo>(
        ChunkType{uuid(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  version,
                  kThisShard});

    auto newChunkMap = makeChunkMap({chunk});

    ASSERT_EQ(newChunkMap.size(), 1);
}

TEST_F(ChunkMapTest, ConstructChunkMapRandom) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector());

    const auto expectedShardVersions = calculateShardVersions(chunkVector);
    const auto expectedCollVersion = calculateCollVersion(expectedShardVersions);

    const auto chunkMap = makeChunkMap(chunkVector);

    // Check that it contains all the chunks
    ASSERT_EQ(chunkVector.size(), chunkMap.size());
    // Check collection version
    ASSERT_EQ(expectedCollVersion, chunkMap.getVersion());

    size_t i = 0;
    chunkMap.forEach([&](const auto& chunkPtr) {
        const auto& expectedChunkPtr = chunkVector[i++];
        assertEqualChunkInfo(*expectedChunkPtr, *chunkPtr);
        return true;
    });

    // Validate all shard versions
    const auto shardVersions = getShardVersionMap(chunkMap);
    ASSERT_EQ(expectedShardVersions.size(), shardVersions.size());
    for (const auto& mapIt : shardVersions) {
        ASSERT_EQ(expectedShardVersions.at(mapIt.first), mapIt.second.placementVersion);
    }

    // Check that vectors are balanced in size
    auto maxVectorSize = static_cast<size_t>(std::lround(chunkMap.getMaxChunkVectorSize() * 1.5));
    auto minVectorSize = std::min(
        chunkMap.size(), static_cast<size_t>(std::lround(chunkMap.getMaxChunkVectorSize() / 2)));

    for (const auto& [maxKeyString, chunkVectorPtr] : chunkMap.getChunkVectorMap()) {
        ASSERT_GTE(chunkVectorPtr->size(), minVectorSize);
        ASSERT_LTE(chunkVectorPtr->size(), maxVectorSize);
    }
}

TEST_F(ChunkMapTest, ConstructChunkMapRandomAllChunksSameVersion) {
    auto chunkVector = genRandomChunkVector();
    auto commonVersion = chunkVector.front().getVersion();

    // Set same version on all chunks
    for (auto& chunk : chunkVector) {
        chunk.setVersion(commonVersion);
    }

    auto chunkInfoVector = toChunkInfoPtrVector(chunkVector);
    const auto expectedShardVersions = calculateShardVersions(chunkInfoVector);
    const auto expectedCollVersion = calculateCollVersion(expectedShardVersions);

    ASSERT_EQ(commonVersion, expectedCollVersion);

    const auto chunkMap = makeChunkMap(chunkInfoVector);

    // Check that it contains all the chunks
    ASSERT_EQ(chunkInfoVector.size(), chunkMap.size());
    // Check collection version
    ASSERT_EQ(expectedCollVersion, chunkMap.getVersion());

    size_t i = 0;
    chunkMap.forEach([&](const auto& chunkPtr) {
        const auto& expectedChunkPtr = chunkInfoVector[i++];
        assertEqualChunkInfo(*expectedChunkPtr, *chunkPtr);
        return true;
    });

    // Validate all shard versions
    const auto shardVersions = getShardVersionMap(chunkMap);
    ASSERT_EQ(expectedShardVersions.size(), shardVersions.size());
    for (const auto& mapIt : shardVersions) {
        ASSERT_EQ(expectedShardVersions.at(mapIt.first), mapIt.second.placementVersion);
    }
}

/*
 * Check that constucting a ChunkMap with chunks that have mismatching timestamp fails.
 */
TEST_F(ChunkMapTest, ConstructChunkMapMismatchingTimestamp) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector());

    // Set a different epoch in one of the chunks
    const Timestamp wrongTimestamp{Date_t::now()};
    ASSERT_NE(wrongTimestamp, collTimestamp());
    const auto wrongChunkIdx = _random.nextInt32(chunkVector.size());
    const auto oldChunk = chunkVector.at(wrongChunkIdx);
    const auto oldVersion = oldChunk->getLastmod();
    const ChunkVersion wrongVersion{{collEpoch(), wrongTimestamp},
                                    {oldVersion.majorVersion(), oldVersion.minorVersion()}};
    chunkVector[wrongChunkIdx] = std::make_shared<ChunkInfo>(
        ChunkType{uuid(), oldChunk->getRange(), wrongVersion, oldChunk->getShardId()});

    ASSERT_THROWS_CODE(
        makeChunkMap(chunkVector), AssertionException, ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(ChunkMapTest, UpdateMapNotLeaveSmallVectors) {
    const ChunkVersion initialVersion{{collEpoch(), collTimestamp()}, {1, 0}};
    auto chunkVector = toChunkInfoPtrVector(
        genChunkVector(uuid(), genRandomSplitPoints(8), initialVersion, 1 /*numShards*/));

    const auto chunkBucketSize = 4;
    LOGV2(7162703, "Constructing new chunk map", "chunkBucketSize"_attr = chunkBucketSize);
    const auto initialChunkMap =
        ChunkMap(collEpoch(), collTimestamp(), chunkBucketSize).createMerged(chunkVector);

    // Check that it contains all the chunks
    ASSERT_EQ(chunkVector.size(), initialChunkMap.size());

    auto mergedVersion = initialChunkMap.getVersion();
    mergedVersion.incMinor();

    auto mergedChunk = std::make_shared<ChunkInfo>(ChunkType{
        uuid(),
        ChunkRange{chunkVector[4]->getRange().getMin(), chunkVector.back()->getRange().getMax()},
        mergedVersion,
        kThisShard});
    const auto chunkMap = initialChunkMap.createMerged({mergedChunk});

    // Check that vectors are balanced in size
    auto maxVectorSize = std::lround(chunkMap.getMaxChunkVectorSize() * 1.5);
    auto minVectorSize = std::min(
        chunkMap.size(), static_cast<size_t>(std::lround(chunkMap.getMaxChunkVectorSize() / 2)));

    for (const auto& [maxKeyString, chunkVectorPtr] : chunkMap.getChunkVectorMap()) {
        ASSERT_GTE(chunkVectorPtr->size(), minVectorSize);
        ASSERT_LTE(chunkVectorPtr->size(), maxVectorSize);
    }
}

/*
 * Check that updating a ChunkMap with chunks that have mismatching timestamp fails.
 */
TEST_F(ChunkMapTest, UpdateChunkMapMismatchingTimestamp) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector());

    auto chunkMap = makeChunkMap(chunkVector);
    auto collVersion = chunkMap.getVersion();

    // Set a different epoch in one of the chunks
    const Timestamp wrongTimestamp{Date_t::now()};
    const auto wrongChunkIdx = _random.nextInt32(chunkVector.size());
    const auto oldChunk = chunkVector.at(wrongChunkIdx);
    const ChunkVersion wrongVersion{{collEpoch(), wrongTimestamp},
                                    {collVersion.majorVersion(), collVersion.minorVersion()}};
    auto updateChunk = std::make_shared<ChunkInfo>(
        ChunkType{uuid(), oldChunk->getRange(), wrongVersion, oldChunk->getShardId()});

    ASSERT_THROWS_CODE(chunkMap.createMerged({updateChunk}),
                       AssertionException,
                       ErrorCodes::ConflictingOperationInProgress);
}

/*
 * Check that updating a ChunkMap with chunks that have lower version fails.
 */
TEST_F(ChunkMapTest, UpdateChunkMapLowerVersion) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector());

    auto chunkMap = makeChunkMap(chunkVector);

    const auto wrongChunkIdx = _random.nextInt32(chunkVector.size());
    const auto oldChunk = chunkVector.at(wrongChunkIdx);
    const ChunkVersion wrongVersion{{collEpoch(), collTimestamp()}, {0, 1}};
    auto updateChunk = std::make_shared<ChunkInfo>(
        ChunkType{uuid(), oldChunk->getRange(), wrongVersion, oldChunk->getShardId()});

    ASSERT_THROWS_CODE(chunkMap.createMerged({updateChunk}), AssertionException, 626840);
}
/*
 * Test update of ChunkMap with random chunk manipulation (splits/merges/moves);
 */
TEST_F(ChunkMapTest, UpdateChunkMapRandom) {
    auto initialChunks = genRandomChunkVector();
    auto initialChunksInfo = toChunkInfoPtrVector(initialChunks);

    const auto initialChunkMap = makeChunkMap(initialChunksInfo);

    const auto initialShardVersions = calculateShardVersions(initialChunksInfo);
    const auto initialCollVersion = calculateCollVersion(initialShardVersions);

    auto chunks = initialChunks;

    const auto maxNumChunkOps = 2 * initialChunks.size();
    const auto numChunkOps = _random.nextInt32(maxNumChunkOps);
    performRandomChunkOperations(&chunks, numChunkOps);

    auto chunksInfo = toChunkInfoPtrVector(initialChunks);

    std::vector<std::shared_ptr<ChunkInfo>> updatedChunksInfo;
    for (auto& chunkPtr : chunksInfo) {
        if (!chunkPtr->getLastmod().isOlderOrEqualThan(initialCollVersion)) {
            updatedChunksInfo.push_back(std::make_shared<ChunkInfo>(ChunkType{
                uuid(), chunkPtr->getRange(), chunkPtr->getLastmod(), chunkPtr->getShardId()}));
        }
    }

    const auto expectedShardVersions = calculateShardVersions(chunksInfo);
    const auto expectedCollVersion = calculateCollVersion(expectedShardVersions);
    auto chunkMap = initialChunkMap.createMerged(updatedChunksInfo);

    // Check that it contains all the chunks
    ASSERT_EQ(chunksInfo.size(), chunkMap.size());
    // Check collection version
    ASSERT_EQ(expectedCollVersion, chunkMap.getVersion());

    size_t i = 0;
    chunkMap.forEach([&](const auto& chunkPtr) {
        const auto& expectedChunkPtr = chunksInfo[i++];
        assertEqualChunkInfo(*expectedChunkPtr, *chunkPtr);
        return true;
    });

    // Validate all shard versions
    const auto shardVersions = getShardVersionMap(chunkMap);
    ASSERT_EQ(expectedShardVersions.size(), shardVersions.size());
    for (const auto& mapIt : shardVersions) {
        ASSERT_EQ(expectedShardVersions.at(mapIt.first), mapIt.second.placementVersion);
    }

    // Check that vectors are balanced in size
    auto maxVectorSize = static_cast<size_t>(std::lround(chunkMap.getMaxChunkVectorSize() * 1.5));
    auto minVectorSize = std::min(
        chunkMap.size(), static_cast<size_t>(std::lround(chunkMap.getMaxChunkVectorSize() / 2)));

    for (const auto& [maxKeyString, chunkVectorPtr] : chunkMap.getChunkVectorMap()) {
        ASSERT_GTE(chunkVectorPtr->size(), minVectorSize);
        ASSERT_LTE(chunkVectorPtr->size(), maxVectorSize);
    }
}

TEST_F(ChunkMapTest, TestEnumerateAllChunks) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};

    auto newChunkMap = makeChunkMap(
        {std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       kThisShard})});

    int count = 0;
    auto lastMax = getShardKeyPattern().globalMin();

    newChunkMap.forEach([&](const auto& chunkInfo) {
        ASSERT(SimpleBSONObjComparator::kInstance.evaluate(chunkInfo->getMax() > lastMax));
        lastMax = chunkInfo->getMax();
        count++;

        return true;
    });

    ASSERT_EQ(count, newChunkMap.size());
}


TEST_F(ChunkMapTest, TestIntersectingChunk) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};

    auto newChunkMap = makeChunkMap(
        {std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       kThisShard})});

    auto intersectingChunk = newChunkMap.findIntersectingChunk(BSON("a" << 50));

    ASSERT(intersectingChunk);
    ASSERT(
        SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMin() == BSON("a" << 0)));
    ASSERT(SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMax() ==
                                                       BSON("a" << 100)));

    // findIntersectingChunks returns last chunk if invoked with MaxKey
    intersectingChunk =
        newChunkMap.findIntersectingChunk(BSON("a" << getShardKeyPattern().globalMax()));
    ASSERT(SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMin() ==
                                                       BSON("a" << 100)));
    ASSERT(SimpleBSONObjComparator::kInstance.evaluate(intersectingChunk->getMax() ==
                                                       getShardKeyPattern().globalMax()));
}

TEST_F(ChunkMapTest, TestIntersectingChunkRandom) {
    auto chunks = toChunkInfoPtrVector(genRandomChunkVector());

    const auto chunkMap = makeChunkMap(chunks);

    auto targetChunkIt = chunks.begin() + _random.nextInt64(chunks.size());
    auto intermediateKey = calculateIntermediateShardKey(
        (*targetChunkIt)->getMin(), (*targetChunkIt)->getMax(), 0.2 /* minKeyProb */);

    auto intersectingChunkPtr = chunkMap.findIntersectingChunk(intermediateKey);
    assertEqualChunkInfo(**(targetChunkIt), *intersectingChunkPtr);
}

TEST_F(ChunkMapTest, TestEnumerateOverlappingChunks) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};

    auto newChunkMap = makeChunkMap(
        {std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, kThisShard}),

         std::make_shared<ChunkInfo>(
             ChunkType{uuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       kThisShard})});

    auto min = BSON("a" << -50);
    auto max = BSON("a" << 150);
    int count = 0;
    newChunkMap.forEachOverlappingChunk(min, max, true, [&](const auto& chunk) {
        count++;
        return true;
    });
    ASSERT_EQ(count, 3);

    min = BSON("a" << -50);
    max = BSON("a" << getShardKeyPattern().globalMax());
    count = 0;
    newChunkMap.forEachOverlappingChunk(min, max, false, [&](const auto& chunk) {
        count++;
        return true;
    });
    ASSERT_EQ(count, 3);

    min = BSON("a" << 50);
    max = BSON("a" << 100);
    count = 0;
    newChunkMap.forEachOverlappingChunk(min, max, true, [&](const auto& chunk) {
        count++;
        return true;
    });
    ASSERT_EQ(count, 2);

    min = BSON("a" << 50);
    max = BSON("a" << 100);
    count = 0;
    newChunkMap.forEachOverlappingChunk(min, max, false, [&](const auto& chunk) {
        count++;
        return true;
    });
    ASSERT_EQ(count, 1);
}

TEST_F(ChunkMapTest, ForEachNoShardKey) {
    auto chunks = toChunkInfoPtrVector(genRandomChunkVector());

    const auto chunkMap = makeChunkMap(chunks);

    auto lastChunkIdx = std::max(_random.nextInt64(chunks.size()), static_cast<int64_t>(1));

    int i = 0;
    chunkMap.forEach([&](const auto& chunkInfo) {
        assertEqualChunkInfo(*chunks[i], *chunkInfo);
        return ++i < lastChunkIdx;
    });

    ASSERT_EQ(i, lastChunkIdx);
}

TEST_F(ChunkMapTest, ForEachWithShardKey) {
    auto chunks = toChunkInfoPtrVector(genRandomChunkVector());

    const auto chunkMap = makeChunkMap(chunks);

    auto firstChunkIdx = static_cast<size_t>(_random.nextInt64(chunks.size()));
    const auto& firstChunk = chunks[firstChunkIdx];
    auto skey = calculateIntermediateShardKey(
        firstChunk->getMin(), firstChunk->getMax(), 0.2 /* minKeyProb */);

    size_t i = firstChunkIdx;
    auto lastChunkIdx = firstChunkIdx +
        std::max(_random.nextInt64(chunks.size() - firstChunkIdx), static_cast<int64_t>(1));
    chunkMap.forEach(
        [&](const auto& chunkInfo) {
            assertEqualChunkInfo(*chunks[i], *chunkInfo);
            return ++i < lastChunkIdx;
        },
        skey);

    ASSERT_EQ(i, lastChunkIdx);
}

TEST_F(ChunkMapTest, TestEnumerateOverlappingChunksRandom) {
    auto chunks = toChunkInfoPtrVector(genRandomChunkVector());

    const auto chunkMap = makeChunkMap(chunks);

    auto firstChunkIt = chunks.begin() + _random.nextInt64(chunks.size());
    auto lastChunkIt = firstChunkIt + _random.nextInt64(std::distance(firstChunkIt, chunks.end()));

    auto minBound = calculateIntermediateShardKey(
        (*firstChunkIt)->getMin(), (*firstChunkIt)->getMax(), 0.2 /* minKeyProb */);
    auto maxBound = calculateIntermediateShardKey(
        (*lastChunkIt)->getMin(), (*lastChunkIt)->getMax(), 0.2 /* minKeyProb */);

    auto it = firstChunkIt;
    chunkMap.forEachOverlappingChunk(minBound, maxBound, true, [&](const auto& chunkInfoPtr) {
        assertEqualChunkInfo(**(it++), *chunkInfoPtr);
        return true;
    });
    ASSERT_EQ(0, std::distance(it, std::next(lastChunkIt)));
}

}  // namespace

}  // namespace mongo
