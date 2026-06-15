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
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/chunks_test_util.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/sharding_environment/shard_handle.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using chunks_test_util::assertEqualChunkInfo;
using chunks_test_util::calculateCollVersion;
using chunks_test_util::calculateIntermediateShardKey;
using chunks_test_util::genChunkVector;
using chunks_test_util::genRandomSplitPoints;
using chunks_test_util::performRandomChunkOperations;
using chunks_test_util::toChunkInfoPtrVector;

namespace {

PseudoRandom _random{SecureRandom().nextInt64()};

const ShardHandle kThisShard{ShardId("testShard"), UUID::gen()};
const ShardHandle kAnotherShard{ShardId("anotherTestShard"), UUID::gen()};

ShardPlacementVersionMap getShardVersionMap(const ChunkMap& chunkMap) {
    return chunkMap.getShardPlacementVersionMap();
}

std::map<ShardRef, ChunkVersion> calculateShardVersions(
    const std::vector<std::shared_ptr<ChunkInfo>>& chunkVector) {
    std::map<ShardRef, ChunkVersion> svMap;
    for (const auto& chunk : chunkVector) {
        auto mapIt = svMap.find(chunk->getShardRef());
        if (mapIt == svMap.end()) {
            svMap.emplace(chunk->getShardRef(), chunk->getLastmod());
            continue;
        }
        if ((mapIt->second <=> chunk->getLastmod()) == std::partial_ordering::less) {
            mapIt->second = chunk->getLastmod();
        }
    }
    return svMap;
}

void validateChunkMap(const ChunkMap& chunkMap,
                      const std::vector<std::shared_ptr<ChunkInfo>>& chunkInfoVector,
                      bool ignorePlacementVersionChecks = false) {

    // The chunkMap should contain all the chunks
    ASSERT_EQ(chunkInfoVector.size(), chunkMap.size());

    // Check collection version
    const auto expectedShardVersions = calculateShardVersions(chunkInfoVector);
    const auto expectedCollVersion = calculateCollVersion(expectedShardVersions);
    ASSERT_EQ(expectedCollVersion, chunkMap.getVersion());

    size_t i = 0;
    chunkMap.forEach([&](const auto& chunkPtr) {
        const auto& expectedChunkPtr = chunkInfoVector[i++];
        // Check that the chunk pointer is valid
        ASSERT_NOT_EQUALS(chunkPtr.get(), nullptr);
        assertEqualChunkInfo(*expectedChunkPtr, *chunkPtr);
        return true;
    });

    // Validate all shard versions
    if (!ignorePlacementVersionChecks) {
        const auto shardVersions = getShardVersionMap(chunkMap);
        ASSERT_EQ(expectedShardVersions.size(), shardVersions.size());
        for (const auto& mapIt : shardVersions) {
            ASSERT_EQ(expectedShardVersions.at(mapIt.first), mapIt.second.placementVersion);
        }
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

class ChunkMapShardRefParamTest : public unittest::Test, public testing::WithParamInterface<bool> {
protected:
    bool useShardUuid() const {
        return GetParam();
    }

    ShardRef thisShard() const {
        return useShardUuid() ? ShardRef(*kThisShard.uuid()) : ShardRef(kThisShard.name());
    }

    ShardRef anotherShard() const {
        return useShardUuid() ? ShardRef(*kAnotherShard.uuid()) : ShardRef(kAnotherShard.name());
    }
};

class ChunkMapTest : public ChunkMapShardRefParamTest {
public:
    const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const UUID& collUuid() const {
        return _collUuid;
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
            _collUuid, _epoch, _collTimestamp, maxNumChunks, minNumChunks, useShardUuid());
    }

private:
    KeyPattern _shardKeyPattern{chunks_test_util::kShardKeyPattern};
    const UUID _collUuid = UUID::gen();
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
};

INSTANTIATE_TEST_SUITE_P(ShardRefVariants,
                         ChunkMapTest,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "UuidShardRef" : "StringShardRef";
                         });

TEST_P(ChunkMapTest, TestAddChunk) {
    ChunkVersion version({collEpoch(), collTimestamp()}, {1, 0});

    auto chunk = std::make_shared<ChunkInfo>(
        ChunkType{collUuid(),
                  ChunkRange{getShardKeyPattern().globalMin(), getShardKeyPattern().globalMax()},
                  version,
                  thisShard()});

    auto newChunkMap = makeChunkMap({chunk});

    ASSERT_EQ(newChunkMap.size(), 1);

    validateChunkMap(newChunkMap, {chunk});
}

TEST_P(ChunkMapTest, ConstructChunkMapRandom) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector());

    const auto chunkMap = makeChunkMap(chunkVector);

    validateChunkMap(chunkMap, chunkVector);
}

TEST_P(ChunkMapTest, ConstructChunkMapRandomAllChunksSameVersion) {
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
    validateChunkMap(chunkMap, chunkInfoVector);
}

/*
 * Check that constucting a ChunkMap with chunks that have mismatching timestamp fails.
 */
TEST_P(ChunkMapTest, ConstructChunkMapMismatchingTimestamp) {
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
        ChunkType{collUuid(), oldChunk->getRange(), wrongVersion, oldChunk->getShardRef()});

    ASSERT_THROWS_CODE(
        makeChunkMap(chunkVector), AssertionException, ErrorCodes::ChunkMetadataInconsistency);
}

TEST_P(ChunkMapTest, UpdateMapNotLeaveSmallVectors) {
    const ChunkVersion initialVersion{{collEpoch(), collTimestamp()}, {1, 0}};
    auto chunkVector = toChunkInfoPtrVector(genChunkVector(
        collUuid(), genRandomSplitPoints(8), initialVersion, 1 /*numShards*/, useShardUuid()));

    const auto chunkBucketSize = 4;
    LOGV2(7162703, "Constructing new chunk map", "chunkBucketSize"_attr = chunkBucketSize);
    const auto initialChunkMap =
        ChunkMap(collEpoch(), collTimestamp(), chunkBucketSize).createMerged(chunkVector);

    // Check that it contains all the chunks
    ASSERT_EQ(chunkVector.size(), initialChunkMap.size());

    auto mergedVersion = initialChunkMap.getVersion();
    mergedVersion.incMinor();

    auto mergedChunk = std::make_shared<ChunkInfo>(ChunkType{
        collUuid(),
        ChunkRange{chunkVector[4]->getRange().getMin(), chunkVector.back()->getRange().getMax()},
        mergedVersion,
        thisShard()});
    const auto chunkMap = initialChunkMap.createMerged({mergedChunk});

    // Check that vectors are balanced in size
    auto maxVectorSize = std::lround(chunkMap.getMaxChunkVectorSize() * 1.5);
    auto minVectorSize = std::min(
        chunkMap.size(), static_cast<size_t>(std::lround(chunkMap.getMaxChunkVectorSize() / 2)));

    for (const auto& [maxKeyString, chunkVectorPtr] : chunkMap.getChunkVectorMap()) {
        ASSERT_GTE(chunkVectorPtr->size(), minVectorSize);
        ASSERT_LTE(chunkVectorPtr->size(), maxVectorSize);
    }

    // Check original map is sitll valid
    validateChunkMap(initialChunkMap, chunkVector);
}


/*
 * Check that updating a ChunkMap with chunks that have mismatching timestamp fails.
 */
TEST_P(ChunkMapTest, UpdateChunkMapMismatchingTimestamp) {
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
        ChunkType{collUuid(), oldChunk->getRange(), wrongVersion, oldChunk->getShardRef()});

    ASSERT_THROWS_CODE(chunkMap.createMerged({updateChunk}),
                       AssertionException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Check that updating a ChunkMap with chunks that have lower version fails.
 */
TEST_P(ChunkMapTest, UpdateChunkMapLowerVersion) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector());

    auto chunkMap = makeChunkMap(chunkVector);

    const auto wrongChunkIdx = _random.nextInt32(chunkVector.size());
    const auto oldChunk = chunkVector.at(wrongChunkIdx);
    const ChunkVersion wrongVersion{{collEpoch(), collTimestamp()}, {0, 1}};
    auto updateChunk = std::make_shared<ChunkInfo>(
        ChunkType{collUuid(), oldChunk->getRange(), wrongVersion, oldChunk->getShardRef()});

    ASSERT_THROWS_CODE(chunkMap.createMerged({updateChunk}), AssertionException, 626840);
}

/*
 * Re-applying a chunk whose version equals the current collection version is accepted (it is not
 * rejected like a strictly-lower version) and leaves the map unchanged.
 */
TEST_P(ChunkMapTest, UpdateChunkMapSameVersion) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector());

    auto chunkMap = makeChunkMap(chunkVector);
    const auto collVersion = chunkMap.getVersion();

    // Pick the chunk that carries the collection version and re-apply it verbatim.
    const auto sameVersionChunkIt =
        std::find_if(chunkVector.begin(), chunkVector.end(), [&](const auto& chunkPtr) {
            return chunkPtr->getLastmod() == collVersion;
        });
    ASSERT(sameVersionChunkIt != chunkVector.end());

    auto updatedChunkMap = chunkMap.createMerged({*sameVersionChunkIt});

    ASSERT_EQ(updatedChunkMap.getVersion(), collVersion);
    validateChunkMap(updatedChunkMap, chunkVector);

    // The original map is still valid and usable.
    validateChunkMap(chunkMap, chunkVector);
}

/*
 * Test update of ChunkMap with random chunk manipulation (splits/merges/moves);
 */
TEST_P(ChunkMapTest, UpdateChunkMapRandom) {
    auto initialChunks = genRandomChunkVector();
    auto initialChunksInfo = toChunkInfoPtrVector(initialChunks);

    const auto initialChunkMap = makeChunkMap(initialChunksInfo);

    const auto initialShardVersions = calculateShardVersions(initialChunksInfo);
    const auto initialCollVersion = calculateCollVersion(initialShardVersions);

    auto chunks = initialChunks;

    const auto maxNumChunkOps = 2 * initialChunks.size();
    const auto numChunkOps = _random.nextInt32(maxNumChunkOps);
    performRandomChunkOperations(&chunks, numChunkOps, useShardUuid());

    auto chunksInfo = toChunkInfoPtrVector(chunks);

    std::vector<std::shared_ptr<ChunkInfo>> updatedChunksInfo;
    for (const auto& chunk : chunks) {
        if ((chunk.getVersion() <=> initialCollVersion) == std::partial_ordering::greater) {
            updatedChunksInfo.push_back(std::make_shared<ChunkInfo>(chunk));
        }
    }

    // Create updated chunk map and validate it
    auto chunkMap = initialChunkMap.createMerged(updatedChunksInfo);
    validateChunkMap(chunkMap, chunksInfo);

    // Check that the initialChunkMap is still valid and usable
    validateChunkMap(initialChunkMap, initialChunksInfo);
}

TEST_P(ChunkMapTest, TestEnumerateAllChunks) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};

    auto newChunkMap = makeChunkMap(
        {std::make_shared<ChunkInfo>(
             ChunkType{collUuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       thisShard()}),

         std::make_shared<ChunkInfo>(ChunkType{
             collUuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, thisShard()}),

         std::make_shared<ChunkInfo>(
             ChunkType{collUuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       thisShard()})});

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


TEST_P(ChunkMapTest, TestIntersectingChunk) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};

    auto newChunkMap = makeChunkMap(
        {std::make_shared<ChunkInfo>(
             ChunkType{collUuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       thisShard()}),

         std::make_shared<ChunkInfo>(ChunkType{
             collUuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, thisShard()}),

         std::make_shared<ChunkInfo>(
             ChunkType{collUuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       thisShard()})});

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

TEST_P(ChunkMapTest, TestIntersectingChunkRandom) {
    auto chunks = toChunkInfoPtrVector(genRandomChunkVector());

    const auto chunkMap = makeChunkMap(chunks);

    auto targetChunkIt = chunks.begin() + _random.nextInt64(chunks.size());
    auto intermediateKey = calculateIntermediateShardKey(
        (*targetChunkIt)->getMin(), (*targetChunkIt)->getMax(), 0.2 /* minKeyProb */);

    auto intersectingChunkPtr = chunkMap.findIntersectingChunk(intermediateKey);
    assertEqualChunkInfo(**(targetChunkIt), *intersectingChunkPtr);
}

TEST_P(ChunkMapTest, TestEnumerateOverlappingChunks) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {1, 0}};

    auto newChunkMap = makeChunkMap(
        {std::make_shared<ChunkInfo>(
             ChunkType{collUuid(),
                       ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << 0)},
                       version,
                       thisShard()}),

         std::make_shared<ChunkInfo>(ChunkType{
             collUuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, version, thisShard()}),

         std::make_shared<ChunkInfo>(
             ChunkType{collUuid(),
                       ChunkRange{BSON("a" << 100), getShardKeyPattern().globalMax()},
                       version,
                       thisShard()})});

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

TEST_P(ChunkMapTest, ForEachNoShardKey) {
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

TEST_P(ChunkMapTest, ForEachWithShardKey) {
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

TEST_P(ChunkMapTest, TestEnumerateOverlappingChunksRandom) {
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

TEST_P(ChunkMapTest, EmptyChunkMapIteration) {
    const ChunkMap chunkMap(collEpoch(), collTimestamp(), 0);
    auto chunkMapIt = chunkMap.begin();
    ASSERT_EQ(chunkMapIt, chunkMap.end());
    ASSERT_THROWS_CODE(chunkMapIt--, AssertionException, 9526304);
    ASSERT_THROWS_CODE(chunkMapIt++, AssertionException, 9526305);
}

TEST_P(ChunkMapTest, TestChunkMapForwardIteration) {
    auto chunks = toChunkInfoPtrVector(genRandomChunkVector());
    const auto chunkMap = makeChunkMap(chunks);

    auto expectedChunkIt = chunks.begin();
    auto chunkMapIt = chunkMap.begin();
    while (expectedChunkIt != chunks.end()) {
        ASSERT_NE(chunkMapIt, chunkMap.end());
        ASSERT_EQ((*chunkMapIt).get(), expectedChunkIt->get());
        chunkMapIt++;
        expectedChunkIt = std::next(expectedChunkIt);
    }
    ASSERT_EQ(chunkMapIt, chunkMap.end());
    // Can't call next() on end().
    ASSERT_THROWS_CODE(chunkMapIt++, AssertionException, 9526305);
}

TEST_P(ChunkMapTest, TestChunkMapReverseIteration) {
    auto chunks = toChunkInfoPtrVector(genRandomChunkVector());
    const auto chunkMap = makeChunkMap(chunks);

    auto expectedChunkIt = chunks.rbegin();
    auto chunkMapIt = --chunkMap.end();
    while (expectedChunkIt != chunks.rend()) {
        ASSERT_NE(chunkMapIt, chunkMap.end());
        ASSERT_EQ((*chunkMapIt).get(), expectedChunkIt->get());
        chunkMapIt--;
        expectedChunkIt = std::next(expectedChunkIt);
    }
    ASSERT_EQ(chunkMapIt, chunkMap.end());
    // Calling prev() on end() returns iterator pointing to second-last element.
    ASSERT_EQ((--chunkMapIt).get().get(), chunks.rbegin()->get());
}

/*
 * Constructing a ChunkMap with chunks that do not cover the entire shard key space fails. The gap
 * is produced by removing a random chunk, which also covers the case of a missing min or max key.
 */
TEST_P(ChunkMapTest, CreateWithMissingChunkFail) {
    auto chunks = genRandomChunkVector(30, 2 /* minNumChunks */);

    // Remove one random chunk to simulate a gap in the shard key space.
    chunks.erase(chunks.begin() + _random.nextInt64(chunks.size()));

    ASSERT_THROWS_CODE(makeChunkMap(toChunkInfoPtrVector(chunks)),
                       DBException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Constructing a ChunkMap with a gap, produced by shrinking the range of a random chunk, fails.
 */
TEST_P(ChunkMapTest, CreateWithChunkGapFail) {
    auto chunks = genRandomChunkVector(30, 2 /* minNumChunks */);

    auto& shrinkedChunk = chunks.at(_random.nextInt64(chunks.size()));
    auto intermediateKey =
        calculateIntermediateShardKey(shrinkedChunk.getMin(), shrinkedChunk.getMax());
    if (_random.nextInt64(2)) {
        // Shrink right bound
        shrinkedChunk.setRange({shrinkedChunk.getMin(), intermediateKey});
    } else {
        // Shrink left bound
        shrinkedChunk.setRange({intermediateKey, shrinkedChunk.getMax()});
    }

    ASSERT_THROWS_CODE(makeChunkMap(toChunkInfoPtrVector(chunks)),
                       DBException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Updating a ChunkMap with a gap, produced by shrinking the range of a random chunk, fails.
 */
TEST_P(ChunkMapTest, UpdateWithChunkGapFail) {
    auto chunks = genRandomChunkVector();

    auto chunkMap = makeChunkMap(toChunkInfoPtrVector(chunks));
    auto collVersion = chunkMap.getVersion();

    auto shrinkedChunk = chunks.at(_random.nextInt64(chunks.size()));
    auto intermediateKey =
        calculateIntermediateShardKey(shrinkedChunk.getMin(), shrinkedChunk.getMax());
    if (_random.nextInt64(2)) {
        // Shrink right bound
        shrinkedChunk.setRange({shrinkedChunk.getMin(), intermediateKey});
    } else {
        // Shrink left bound
        shrinkedChunk.setRange({intermediateKey, shrinkedChunk.getMax()});
    }

    // Bump chunk version
    collVersion.incMajor();
    shrinkedChunk.setVersion(collVersion);

    ASSERT_THROWS_CODE(chunkMap.createMerged({std::make_shared<ChunkInfo>(shrinkedChunk)}),
                       AssertionException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Constructing a ChunkMap with overlapping chunks fails.
 */
TEST_P(ChunkMapTest, CreateWithChunkOverlapFail) {
    auto chunks = genRandomChunkVector(30, 2 /* minNumChunks */);

    auto chunkToExtendIt = chunks.begin() + _random.nextInt64(chunks.size());

    const auto canExtendLeft = chunkToExtendIt > chunks.begin();
    const auto extendRight =
        !canExtendLeft || ((chunkToExtendIt < std::prev(chunks.end())) && _random.nextInt64(2));
    const auto extendLeft = !extendRight;
    if (extendRight) {
        auto newMax = calculateIntermediateShardKey(chunkToExtendIt->getMax(),
                                                    std::next(chunkToExtendIt)->getMax(),
                                                    0.0 /* minKeyProb */,
                                                    0.1 /* maxKeyProb */);
        // extend right bound
        chunkToExtendIt->setRange({chunkToExtendIt->getMin(), newMax});
        auto newVersion = chunkToExtendIt->getVersion();
        newVersion.incMajor();
        std::next(chunkToExtendIt)->setVersion(newVersion);
    }

    if (extendLeft) {
        invariant(canExtendLeft);
        auto newMin = calculateIntermediateShardKey(std::prev(chunkToExtendIt)->getMin(),
                                                    chunkToExtendIt->getMin(),
                                                    0.1 /* minKeyProb */,
                                                    0.0 /* maxKeyProb */);
        // extend left bound
        chunkToExtendIt->setRange({newMin, chunkToExtendIt->getMax()});
        auto newVersion = chunkToExtendIt->getVersion();
        newVersion.incMajor();
        std::prev(chunkToExtendIt)->setVersion(newVersion);
    }

    ASSERT_THROWS_CODE(makeChunkMap(toChunkInfoPtrVector(chunks)),
                       DBException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Updating a ChunkMap with overlapping chunks fails.
 */
TEST_P(ChunkMapTest, UpdateWithChunkOverlapFail) {
    auto chunks = genRandomChunkVector(30, 2 /* minNumChunks */);

    auto chunkMap = makeChunkMap(toChunkInfoPtrVector(chunks));
    auto collVersion = chunkMap.getVersion();

    auto chunkToExtendIt = chunks.begin() + _random.nextInt64(chunks.size());

    const auto canExtendLeft = chunkToExtendIt > chunks.begin();
    const auto extendRight =
        !canExtendLeft || (chunkToExtendIt < std::prev(chunks.end()) && _random.nextInt64(2));
    const auto extendLeft = !extendRight;
    if (extendRight) {
        auto newMax = calculateIntermediateShardKey(chunkToExtendIt->getMax(),
                                                    std::next(chunkToExtendIt)->getMax());
        // extend right bound
        chunkToExtendIt->setRange({chunkToExtendIt->getMin(), newMax});
    }

    if (extendLeft) {
        invariant(canExtendLeft);
        auto newMin = calculateIntermediateShardKey(std::prev(chunkToExtendIt)->getMin(),
                                                    chunkToExtendIt->getMin());
        // extend left bound
        chunkToExtendIt->setRange({newMin, chunkToExtendIt->getMax()});
    }

    // Bump chunk version
    collVersion.incMajor();
    chunkToExtendIt->setVersion(collVersion);

    ASSERT_THROWS_CODE(chunkMap.createMerged({std::make_shared<ChunkInfo>(*chunkToExtendIt)}),
                       AssertionException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Constructing a ChunkMap whose first chunk does not start at MinKey fails.
 */
TEST_P(ChunkMapTest, CreateWrongMinFail) {
    auto chunks = genRandomChunkVector();

    chunks.begin()->setRange(
        {BSON("a" << std::numeric_limits<int64_t>::min()), chunks.begin()->getMax()});

    ASSERT_THROWS_CODE(makeChunkMap(toChunkInfoPtrVector(chunks)),
                       DBException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

/*
 * Constructing a ChunkMap whose last chunk does not end at MaxKey fails.
 */
TEST_P(ChunkMapTest, CreateWrongMaxFail) {
    auto chunks = genRandomChunkVector();

    chunks.begin()->setRange(
        {chunks.begin()->getMin(), BSON("a" << std::numeric_limits<int64_t>::max())});

    ASSERT_THROWS_CODE(makeChunkMap(toChunkInfoPtrVector(chunks)),
                       DBException,
                       ErrorCodes::ChunkMetadataInconsistency);
}

class ChunkMapWithGapsTest : public ChunkMapShardRefParamTest {
public:
    const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const UUID& collUuid() const {
        return _collUuid;
    }

    const OID& collEpoch() const {
        return _epoch;
    }

    const Timestamp& collTimestamp() const {
        return _collTimestamp;
    }

    const ChunkVersion nextCollVersion() {
        return ChunkVersion{{_epoch, _collTimestamp}, {majorVersion++, minorVersion}};
    }

    std::vector<std::shared_ptr<ChunkInfo>> chunksForShard(const ShardRef& shard,
                                                           const ChunkMap& chunkMap) {
        std::vector<std::shared_ptr<ChunkInfo>> chunks;
        for (auto it = chunkMap.begin(); it != chunkMap.end(); it++) {
            auto chunkInfo = it.get();
            if (chunkInfo->getShardRef() == shard) {
                chunks.push_back(it.get());
            }
        }
        return chunks;
    }

    ChunkMap makeChunkMap(const std::vector<std::shared_ptr<ChunkInfo>>& chunks) const {
        const auto chunkBucketSize =
            static_cast<size_t>(_random.nextInt64(chunks.size() * 1.2) + 1);
        return ChunkMap{collEpoch(), collTimestamp(), chunkBucketSize}.createMerged(chunks);
    }

    ChunkMap makeChunkMapWithGaps(const std::vector<std::shared_ptr<ChunkInfo>>& chunks) const {
        const auto chunkBucketSize =
            static_cast<size_t>(_random.nextInt64(chunks.size() * 1.2) + 1);
        return ChunkMap{collEpoch(), collTimestamp(), chunkBucketSize, true}.createMerged(chunks);
    }

    std::vector<ChunkType> genRandomChunkVector(size_t maxNumChunks = 30,
                                                size_t minNumChunks = 1) const {
        return chunks_test_util::genRandomChunkVector(
            _collUuid, _epoch, _collTimestamp, maxNumChunks, minNumChunks, useShardUuid());
    }

    ChunkVersion version(uint32_t major, uint32_t minor) const {
        return ChunkVersion{{_epoch, _collTimestamp}, {major, minor}};
    }

    std::shared_ptr<ChunkInfo> makeChunkInfo(
        const BSONObj& min,
        const BSONObj& max,
        const ChunkVersion& chunkVersion,
        const boost::optional<ShardRef>& shard = boost::none) const {
        return std::make_shared<ChunkInfo>(
            ChunkType{_collUuid, ChunkRange{min, max}, chunkVersion, shard ? *shard : thisShard()});
    }

    BSONObj key(int value) const {
        return BSON("a" << value);
    }

private:
    KeyPattern _shardKeyPattern{chunks_test_util::kShardKeyPattern};
    const UUID _collUuid = UUID::gen();
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
    uint32_t majorVersion = 1;
    uint32_t minorVersion = 0;
};

INSTANTIATE_TEST_SUITE_P(ShardRefVariants,
                         ChunkMapWithGapsTest,
                         testing::Bool(),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "UuidShardRef" : "StringShardRef";
                         });

TEST_P(ChunkMapWithGapsTest, TestChunkFillingTheGapInsertion) {
    std::vector<std::shared_ptr<ChunkInfo>> chunks = {
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -100), BSON("a" << 0)},
                                              nextCollVersion(),
                                              thisShard()}),
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << 100), BSON("a" << 200)},
                                              nextCollVersion(),
                                              thisShard()})};
    auto chunkMap = makeChunkMapWithGaps(chunks);

    auto betweenGapChunk = std::make_shared<ChunkInfo>(ChunkType{
        collUuid(), ChunkRange{BSON("a" << 25), BSON("a" << 50)}, nextCollVersion(), thisShard()});
    auto leftGapChunk =
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -250), BSON("a" << -200)},
                                              nextCollVersion(),
                                              thisShard()});
    auto rightGapChunk =
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << 300), BSON("a" << 350)},
                                              nextCollVersion(),
                                              thisShard()});

    chunkMap = chunkMap.createMerged({leftGapChunk, betweenGapChunk, rightGapChunk});

    validateChunkMap(chunkMap,
                     {leftGapChunk, chunks[0], betweenGapChunk, chunks[1], rightGapChunk});
}

TEST_P(ChunkMapWithGapsTest, TestChunkPerfectlyFillingTheGapInsertion) {
    std::vector<std::shared_ptr<ChunkInfo>> chunks = {
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -100), BSON("a" << 0)},
                                              nextCollVersion(),
                                              thisShard()}),
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << 100), BSON("a" << 200)},
                                              nextCollVersion(),
                                              thisShard()})};
    auto chunkMap = makeChunkMapWithGaps(chunks);

    auto betweenGapChunk = std::make_shared<ChunkInfo>(ChunkType{
        collUuid(), ChunkRange{BSON("a" << 0), BSON("a" << 100)}, nextCollVersion(), thisShard()});
    auto leftGapChunk = std::make_shared<ChunkInfo>(
        ChunkType{collUuid(),
                  ChunkRange{getShardKeyPattern().globalMin(), BSON("a" << -100)},
                  nextCollVersion(),
                  thisShard()});
    auto rightGapChunk = std::make_shared<ChunkInfo>(
        ChunkType{collUuid(),
                  ChunkRange{BSON("a" << 200), getShardKeyPattern().globalMax()},
                  nextCollVersion(),
                  thisShard()});

    chunkMap = chunkMap.createMerged({leftGapChunk, betweenGapChunk, rightGapChunk});

    validateChunkMap(chunkMap,
                     {leftGapChunk, chunks[0], betweenGapChunk, chunks[1], rightGapChunk});
}

TEST_P(ChunkMapWithGapsTest, TestSupersetChunkInsertion) {
    std::vector<std::shared_ptr<ChunkInfo>> chunks = {
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -100), BSON("a" << 0)},
                                              nextCollVersion(),
                                              thisShard()}),
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << 100), BSON("a" << 200)},
                                              nextCollVersion(),
                                              thisShard()})};
    auto chunkMap = makeChunkMapWithGaps(chunks);

    auto supersetChunk =
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -200), BSON("a" << 50)},
                                              nextCollVersion(),
                                              thisShard()});

    chunkMap = chunkMap.createMerged({supersetChunk});

    validateChunkMap(chunkMap, {supersetChunk, chunks[1]});
}

TEST_P(ChunkMapWithGapsTest, TestSubsetChunkInsertion) {
    std::vector<std::shared_ptr<ChunkInfo>> chunks = {
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -100), BSON("a" << 0)},
                                              nextCollVersion(),
                                              thisShard()}),
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << 100), BSON("a" << 200)},
                                              nextCollVersion(),
                                              thisShard()})};
    auto chunkMap = makeChunkMapWithGaps(chunks);

    auto subsetChunk =
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -75), BSON("a" << -25)},
                                              nextCollVersion(),
                                              thisShard()});

    chunkMap = chunkMap.createMerged({subsetChunk});

    validateChunkMap(chunkMap, {subsetChunk, chunks[1]});
}

TEST_P(ChunkMapWithGapsTest, TestIntersectingChunkInsertion) {
    std::vector<std::shared_ptr<ChunkInfo>> chunks = {
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -100), BSON("a" << 0)},
                                              nextCollVersion(),
                                              thisShard()}),
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << 100), BSON("a" << 200)},
                                              nextCollVersion(),
                                              thisShard()})};
    auto chunkMap = makeChunkMapWithGaps(chunks);

    auto intersectingChunk =
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << -150), BSON("a" << -50)},
                                              nextCollVersion(),
                                              thisShard()});

    chunkMap = chunkMap.createMerged({intersectingChunk});

    // always prefer chunk with the new version in case of intersection
    validateChunkMap(chunkMap, {intersectingChunk, chunks[1]});
}

TEST_P(ChunkMapWithGapsTest, TestFindIntersectingShardKeysWorks) {
    auto chunkVector = toChunkInfoPtrVector(genRandomChunkVector(10, 10));
    auto chunks = chunkVector;
    auto chunkMapWithoutGaps = makeChunkMap(chunkVector);

    // derive a chunk map from previous one with gaps in the beginning, middle and the end
    std::set<std::shared_ptr<ChunkInfo>> chunkGaps;
    chunkGaps.insert(chunks[0]);
    chunkGaps.insert(chunks[4]);
    chunkGaps.insert(chunks[chunks.size() - 1]);

    chunks.erase(chunks.begin() + 4);
    chunks.erase(chunks.begin());
    chunks.erase(chunks.end() - 1);

    auto chunkMapWithGaps = makeChunkMapWithGaps(chunks);

    for (auto it = chunkMapWithoutGaps.begin(); it != chunkMapWithoutGaps.end(); it++) {
        auto chunk = *it;
        auto intermediateShardKey = calculateIntermediateShardKey(chunk->getMin(), chunk->getMax());
        auto chunkWithoutGapsItForIntermediate = chunkMapWithoutGaps.find(intermediateShardKey);
        auto chunkWithoutGapsItForMin = chunkMapWithoutGaps.find(chunk->getMin());
        auto chunkWithGapsItForIntermediate = chunkMapWithGaps.find(intermediateShardKey);
        auto chunkWithGapsItForMin = chunkMapWithGaps.find(chunk->getMin());
        if (chunkGaps.contains(chunk)) {
            ASSERT_EQ(chunkWithGapsItForIntermediate, chunkMapWithGaps.end());
            ASSERT_EQ(chunkWithGapsItForMin, chunkMapWithGaps.end());
        } else {
            ASSERT_EQ(*chunkWithGapsItForIntermediate, *chunkWithoutGapsItForIntermediate);
            ASSERT_EQ(*chunkWithGapsItForMin, *chunkWithoutGapsItForMin);
        }
    }
}

TEST_P(ChunkMapWithGapsTest, TestNewShardChunkMigration) {
    auto chunkToMigrate = ChunkType{
        collUuid(), ChunkRange{BSON("a" << -100), BSON("a" << 0)}, nextCollVersion(), thisShard()};
    std::vector<std::shared_ptr<ChunkInfo>> chunks = {
        std::make_shared<ChunkInfo>(chunkToMigrate),
        std::make_shared<ChunkInfo>(ChunkType{collUuid(),
                                              ChunkRange{BSON("a" << 100), BSON("a" << 200)},
                                              nextCollVersion(),
                                              thisShard()}),
    };
    auto chunkMap = makeChunkMapWithGaps(chunks);

    auto chunkVersion = chunkToMigrate.getVersion();
    chunkVersion.incMajor();
    chunkToMigrate.setVersion(chunkVersion);
    chunkToMigrate.setShard(anotherShard());

    auto migratedChunk = std::make_shared<ChunkInfo>(chunkToMigrate);
    chunkMap = chunkMap.createMerged({migratedChunk});

    validateChunkMap(chunkMap, {migratedChunk, chunks[1]}, true);

    ASSERT_EQ(chunksForShard(thisShard(), chunkMap).at(0), chunks[1]);
    ASSERT_EQ(chunksForShard(anotherShard(), chunkMap).at(0), migratedChunk);
}

TEST_P(ChunkMapWithGapsTest, TestOldShardChunkMigration) {
    ChunkVersion version{{collEpoch(), collTimestamp()}, {2, 1}};
    auto chunkToMigrate =
        ChunkType{collUuid(), ChunkRange{BSON("a" << -100), BSON("a" << 0)}, version, thisShard()};
    auto chunkToStay =
        ChunkType{collUuid(), ChunkRange{BSON("a" << 100), BSON("a" << 200)}, version, thisShard()};
    std::vector<std::shared_ptr<ChunkInfo>> chunks = {
        std::make_shared<ChunkInfo>(chunkToMigrate),
        std::make_shared<ChunkInfo>(chunkToStay),
    };
    auto chunkMap = makeChunkMapWithGaps(chunks);

    auto olderChunkVersion = ChunkVersion{{collEpoch(), collTimestamp()}, {1, 1}};
    chunkToMigrate.setVersion(olderChunkVersion);
    chunkToMigrate.setShard(anotherShard());

    auto migratedChunk = std::make_shared<ChunkInfo>(chunkToMigrate);

    // insertion of older versions are throwing
    ASSERT_THROWS_CODE(chunkMap.createMerged({migratedChunk}), AssertionException, 626840);
}

// Splitting an owned chunk into several pieces leaves the surrounding gaps untouched.
TEST_P(ChunkMapWithGapsTest, SplitWithinOwnedRangePreservesGaps) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});

    // Split [0, 10) into [0, 3), [3, 7), [7, 10) (minor-version bumps, same owner).
    auto updated = chunkMap.createMerged({makeChunkInfo(key(0), key(3), version(1, 2)),
                                          makeChunkInfo(key(3), key(7), version(1, 3)),
                                          makeChunkInfo(key(7), key(10), version(1, 4))});

    ASSERT_EQ(updated.size(), 4);
    ASSERT_EQ(updated.getVersion(), version(1, 4));
    ASSERT(updated.findIntersectingChunk(key(5)));
    ASSERT(updated.findIntersectingChunk(key(25)));
    // The gap between the owned ranges is preserved.
    ASSERT(!updated.findIntersectingChunk(key(15)));
}

// One incoming chunk that overlaps several existing chunks replaces them all (a merge). The gaps
// around the merged range stay.
TEST_P(ChunkMapWithGapsTest, MergeOverlappingMultipleChunks) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(3), version(1, 0)),
                                          makeChunkInfo(key(3), key(7), version(1, 1)),
                                          makeChunkInfo(key(7), key(10), version(1, 2)),
                                          makeChunkInfo(key(20), key(30), version(1, 3))});
    ASSERT_EQ(chunkMap.size(), 4);

    // One chunk covering [0, 10) overlaps the three smaller chunks and merges them.
    auto updated = chunkMap.createMerged({makeChunkInfo(key(0), key(10), version(2, 0))});

    ASSERT_EQ(updated.size(), 2);
    ASSERT_EQ(updated.getVersion(), version(2, 0));
    auto merged = updated.findIntersectingChunk(key(5));
    ASSERT(merged);
    ASSERT_EQ(merged->getLastmod(), version(2, 0));
    ASSERT(!updated.findIntersectingChunk(key(15)));
}

// A split whose pieces change ownership: half of [0, 10) stays on this shard and the other half
// moves to another shard, in a single update.
TEST_P(ChunkMapWithGapsTest, SplitWithOwnershipChange) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});

    auto updated =
        chunkMap.createMerged({makeChunkInfo(key(0), key(5), version(2, 0), thisShard()),
                               makeChunkInfo(key(5), key(10), version(2, 1), anotherShard())});

    ASSERT_EQ(updated.size(), 3);
    ASSERT_EQ(updated.findIntersectingChunk(key(2))->getShardRef(), thisShard());
    ASSERT_EQ(updated.findIntersectingChunk(key(7))->getShardRef(), anotherShard());
    ASSERT_EQ(updated.findIntersectingChunk(key(25))->getShardRef(), thisShard());
    ASSERT(!updated.findIntersectingChunk(key(15)));
    ASSERT_EQ(getShardVersionMap(updated).size(), 2);
}

// Shrinking a chunk opens a gap where it used to reach. A complete map rejects this; a gap map
// allows it.
TEST_P(ChunkMapWithGapsTest, ShrinkChunkCreatesGapDoesNotThrow) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});

    // Shrink [0, 10) to [0, 8), opening a gap at [8, 10).
    auto updated = chunkMap.createMerged({makeChunkInfo(key(0), key(8), version(2, 0))});

    ASSERT_EQ(updated.size(), 2);
    ASSERT(updated.findIntersectingChunk(key(5)));
    // The newly opened gap is not present.
    ASSERT(!updated.findIntersectingChunk(key(9)));
    ASSERT(updated.findIntersectingChunk(key(25)));
}

// An incoming chunk whose boundaries fall inside existing chunks drops every overlapped old chunk
// wholesale (old chunks are never trimmed), leaving gaps on either side of the new chunk.
TEST_P(ChunkMapWithGapsTest, InteriorBoundariesDropOverlappedChunks) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(10), key(20), version(1, 1)),
                                          makeChunkInfo(key(20), key(30), version(1, 2))});

    // [5, 15) partially overlaps [0, 10) and [10, 20); neither boundary matches an existing one.
    auto updated = chunkMap.createMerged({makeChunkInfo(key(5), key(15), version(2, 0))});

    ASSERT_EQ(updated.size(), 2);  // [5, 15) and the surviving [20, 30)
    ASSERT_EQ(updated.getVersion(), version(2, 0));
    ASSERT(!updated.findIntersectingChunk(key(2)));  // gap opened on the left
    ASSERT(updated.findIntersectingChunk(key(7)));   // the new chunk
    ASSERT(updated.findIntersectingChunk(key(12)));
    ASSERT(!updated.findIntersectingChunk(key(17)));  // gap opened on the right
    ASSERT(updated.findIntersectingChunk(key(25)));   // untouched old chunk
}

// Inserting a chunk strictly contained within a larger existing chunk drops the whole enclosing
// chunk (no trimming), leaving the inner chunk surrounded by gaps.
TEST_P(ChunkMapWithGapsTest, InnerChunkDropsEnclosingChunk) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(100), version(1, 0))});
    ASSERT_EQ(chunkMap.size(), 1);

    // [10, 20) is wholly inside [0, 100); the enclosing chunk overlaps it and is discarded.
    auto updated = chunkMap.createMerged({makeChunkInfo(key(10), key(20), version(2, 0))});

    ASSERT_EQ(updated.size(), 1);  // only [10, 20) remains
    ASSERT_EQ(updated.getVersion(), version(2, 0));
    ASSERT(!updated.findIntersectingChunk(key(5)));   // gap before
    ASSERT(updated.findIntersectingChunk(key(15)));   // the inner chunk
    ASSERT(!updated.findIntersectingChunk(key(50)));  // [0, 100) is gone, not trimmed
}

// Merging coalesces the contiguous chunks a shard owns. Ranges separated by a gap stay independent.
TEST_P(ChunkMapWithGapsTest, MergeAllContiguousOwnedChunks) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(5), version(1, 0)),
                                          makeChunkInfo(key(5), key(10), version(1, 1)),
                                          makeChunkInfo(key(20), key(30), version(1, 2))});
    ASSERT_EQ(chunkMap.size(), 3);

    // Merge the contiguous [0, 5) + [5, 10) into [0, 10); the separated range is left alone.
    auto updated = chunkMap.createMerged({makeChunkInfo(key(0), key(10), version(2, 0))});

    ASSERT_EQ(updated.size(), 2);
    ASSERT(updated.findIntersectingChunk(key(7)));
    ASSERT(updated.findIntersectingChunk(key(25)));
    ASSERT(!updated.findIntersectingChunk(key(15)));
}

// An update with no changed chunks is a no-op and leaves the map and its gaps unchanged.
TEST_P(ChunkMapWithGapsTest, EmptyChangedChunksIsNoOp) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});

    auto updated = chunkMap.createMerged({});

    ASSERT_EQ(updated.size(), 2);
    ASSERT_EQ(updated.getVersion(), version(1, 1));
    ASSERT(updated.findIntersectingChunk(key(5)));
    ASSERT(updated.findIntersectingChunk(key(25)));
    ASSERT(!updated.findIntersectingChunk(key(15)));
}

// A gap map whose first chunk does not start at MinKey can still be updated.
TEST_P(ChunkMapWithGapsTest, FirstChunkNotAtMinKey) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(5), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});

    // Split the first owned chunk, which begins after MinKey.
    auto updated = chunkMap.createMerged({makeChunkInfo(key(5), key(7), version(2, 0)),
                                          makeChunkInfo(key(7), key(10), version(2, 1))});

    ASSERT_EQ(updated.size(), 3);
    ASSERT(updated.findIntersectingChunk(key(6)));
    ASSERT(updated.findIntersectingChunk(key(8)));
    // The leading gap before the first chunk is preserved.
    ASSERT(!updated.findIntersectingChunk(key(2)));
}

// A gap map whose last chunk does not end at MaxKey can still be updated.
TEST_P(ChunkMapWithGapsTest, LastChunkNotAtMaxKey) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});

    // Split the last owned chunk, which ends before MaxKey.
    auto updated = chunkMap.createMerged({makeChunkInfo(key(20), key(25), version(2, 0)),
                                          makeChunkInfo(key(25), key(30), version(2, 1))});

    ASSERT_EQ(updated.size(), 3);
    ASSERT(updated.findIntersectingChunk(key(22)));
    ASSERT(updated.findIntersectingChunk(key(27)));
    // The trailing gap after the last chunk is preserved.
    ASSERT(!updated.findIntersectingChunk(key(35)));
}

// A single update can touch several separate regions at once: here it splits one owned range and
// fills a separate gap in the same call.
TEST_P(ChunkMapWithGapsTest, MultipleDisjointRegionsInOneCall) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});

    auto updated = chunkMap.createMerged({makeChunkInfo(key(0), key(5), version(2, 0)),
                                          makeChunkInfo(key(5), key(10), version(2, 1)),
                                          makeChunkInfo(key(10), key(20), version(2, 2))});

    ASSERT_EQ(updated.size(), 4);
    ASSERT(updated.findIntersectingChunk(key(2)));
    ASSERT(updated.findIntersectingChunk(key(7)));
    // The former gap [10, 20) is now present.
    ASSERT(updated.findIntersectingChunk(key(15)));
    ASSERT(updated.findIntersectingChunk(key(25)));
    ASSERT(!updated.findIntersectingChunk(key(-5)));
    ASSERT(!updated.findIntersectingChunk(key(35)));
}

// An update chunk that spans a gap absorbs it: createMerged cannot tell the spanned range belongs
// to another shard. Callers must never produce a chunk that crosses a range they do not own. This
// documents the current behavior.
TEST_P(ChunkMapWithGapsTest, ChunkSpanningGapAbsorbsIt) {
    auto chunkMap = makeChunkMapWithGaps({makeChunkInfo(key(0), key(10), version(1, 0)),
                                          makeChunkInfo(key(20), key(30), version(1, 1))});
    ASSERT(!chunkMap.findIntersectingChunk(key(15)));

    // [0, 30) overlaps both owned chunks and spans the gap [10, 20) between them.
    auto updated = chunkMap.createMerged({makeChunkInfo(key(0), key(30), version(2, 0))});

    ASSERT_EQ(updated.size(), 1);
    // The previously absent range is now covered (incorrectly, ownership-wise).
    ASSERT(updated.findIntersectingChunk(key(15)));
}

// Random stress test: build a partial map from a random subset of a fixed key grid (leaving random
// gaps), then apply random in-place splits to the owned chunks. Splits never cross a gap, so the
// set of owned cells stays the same. Checks that updates never throw and that owned cells stay
// present while gap cells stay absent.
TEST_P(ChunkMapWithGapsTest, RandomGapUpdatesNeverThrow) {
    static constexpr int kNumCells = 10;
    static constexpr int kCellWidth = 10;

    // Randomly pick which grid cells this shard owns, keeping at least one owned cell and one gap.
    std::vector<bool> ownedCell(kNumCells);
    for (int i = 0; i < kNumCells; ++i) {
        ownedCell[i] = _random.nextInt64(2) == 0;
    }
    if (std::all_of(ownedCell.begin(), ownedCell.end(), [](bool b) { return b; })) {
        ownedCell[_random.nextInt64(kNumCells)] = false;
    }
    if (std::none_of(ownedCell.begin(), ownedCell.end(), [](bool b) { return b; })) {
        ownedCell[_random.nextInt64(kNumCells)] = true;
    }

    std::vector<std::shared_ptr<ChunkInfo>> initialChunks;
    for (int i = 0; i < kNumCells; ++i) {
        if (ownedCell[i]) {
            initialChunks.push_back(makeChunkInfo(key(i * kCellWidth),
                                                  key((i + 1) * kCellWidth),
                                                  version(1, static_cast<uint32_t>(i))));
        }
    }

    auto chunkMap = makeChunkMapWithGaps(initialChunks);

    // Run a random number of split rounds. Each round splits one owned chunk in half, within its
    // own range, so no gap is ever crossed.
    const int rounds = 1 + _random.nextInt32(5);
    for (int r = 0; r < rounds; ++r) {
        std::vector<std::pair<int, int>> splittable;
        chunkMap.forEach([&](const auto& chunkInfo) {
            if (chunkInfo->getShardRef() == thisShard()) {
                const int lo = chunkInfo->getMin()["a"].numberInt();
                const int hi = chunkInfo->getMax()["a"].numberInt();
                if (hi - lo >= 2) {
                    splittable.emplace_back(lo, hi);
                }
            }
            return true;
        });
        if (splittable.empty()) {
            break;
        }

        const auto [lo, hi] = splittable[_random.nextInt64(splittable.size())];
        const int mid = lo + (hi - lo) / 2;
        auto v1 = chunkMap.getVersion();
        v1.incMinor();
        auto v2 = v1;
        v2.incMinor();
        chunkMap = chunkMap.createMerged(
            {makeChunkInfo(key(lo), key(mid), v1), makeChunkInfo(key(mid), key(hi), v2)});
    }

    // The splits must not change which cells are owned and which are gaps.
    for (int i = 0; i < kNumCells; ++i) {
        const auto found = chunkMap.findIntersectingChunk(key(i * kCellWidth + kCellWidth / 2));
        ASSERT_EQ(static_cast<bool>(found), static_cast<bool>(ownedCell[i])) << "cell " << i;
        if (found) {
            ASSERT_EQ(found->getShardRef(), thisShard());
        }
    }
}

enum class MixedShardRef {
    InitialStringUpdatesUuid,
    InitialUuidUpdatesString,
};

class ChunkMapMixedShardRefTest : public unittest::Test,
                                  public testing::WithParamInterface<MixedShardRef> {
public:
    // Helper function to determine whether to use a UUID or shardId for the shard ref based on the
    // test parameter and whether this will be used for the initial state or subsequent updates.
    bool shardRefUsesUuid(bool initial) const {
        const bool initialUsesUuid = GetParam() == MixedShardRef::InitialUuidUpdatesString;
        const bool updateUsesUuid = GetParam() == MixedShardRef::InitialStringUpdatesUuid;
        return initial ? initialUsesUuid : updateUsesUuid;
    }

    // Returns the shard ref for kThisShard. If "initial" is true, returns the shard ref to be used
    // when constructing the initial chunk map, prior to any modifications. If "initial" is false,
    // returns the shard ref to be used when for chunk modifications after constructing the initial
    // state.
    ShardRef thisShardRef(bool initial) const {
        return shardRefUsesUuid(initial) ? ShardRef(*kThisShard.uuid())
                                         : ShardRef(kThisShard.name());
    }

    // Returns the shard ref for kAnotherShard. If "initial" is true, returns the shard ref to be
    // used when constructing the initial chunk map, prior to any modifications. If "initial" is
    // false, returns the shard ref to be used when for chunk modifications after constructing the
    // initial state.
    ShardRef anotherShardRef(bool initial) const {
        return shardRefUsesUuid(initial) ? ShardRef(*kAnotherShard.uuid())
                                         : ShardRef(kAnotherShard.name());
    }

    const KeyPattern& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const UUID& collUuid() const {
        return _collUuid;
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
        return ChunkMap{collEpoch(), collTimestamp(), chunkBucketSize}.createMerged(chunks);
    }

    std::shared_ptr<ChunkInfo> makeChunk(const BSONObj& min,
                                         const BSONObj& max,
                                         const ChunkVersion& version,
                                         const ShardRef& shardRef) const {
        return std::make_shared<ChunkInfo>(
            ChunkType{collUuid(), ChunkRange{min, max}, version, shardRef});
    }

    ChunkVersion version(uint32_t major, uint32_t minor) const {
        return ChunkVersion{{collEpoch(), collTimestamp()}, {major, minor}};
    }

    void assertPlacementVersion(const ChunkMap& chunkMap,
                                const ShardRef& shardRef,
                                const ChunkVersion& expectedVersion) const {
        const auto shardVersions = getShardVersionMap(chunkMap);
        const auto it = shardVersions.find(shardRef);
        ASSERT_NE(it, shardVersions.end());
        ASSERT_EQ(expectedVersion, it->second.placementVersion);
    }

    void assertNoPlacementVersion(const ChunkMap& chunkMap, const ShardRef& shardRef) const {
        const auto shardVersions = getShardVersionMap(chunkMap);
        ASSERT_EQ(shardVersions.end(), shardVersions.find(shardRef));
    }

private:
    KeyPattern _shardKeyPattern{chunks_test_util::kShardKeyPattern};
    const UUID _collUuid = UUID::gen();
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
};

INSTANTIATE_TEST_SUITE_P(MixedShardRef,
                         ChunkMapMixedShardRefTest,
                         testing::Values(MixedShardRef::InitialStringUpdatesUuid,
                                         MixedShardRef::InitialUuidUpdatesString),
                         [](const testing::TestParamInfo<MixedShardRef>& info) {
                             switch (info.param) {
                                 case MixedShardRef::InitialStringUpdatesUuid:
                                     return "InitialStringUpdatesUuid";
                                 case MixedShardRef::InitialUuidUpdatesString:
                                     return "InitialUuidUpdatesString";
                             }
                             MONGO_UNREACHABLE;
                         });

TEST_P(ChunkMapMixedShardRefTest, MixedShardRefSplitOneShard) {
    const auto& keyPattern = getShardKeyPattern();
    const auto splitKey = BSON("a" << 0);

    const auto initialChunk = makeChunk(keyPattern.globalMin(),
                                        keyPattern.globalMax(),
                                        version(1, 0),
                                        thisShardRef(true /* initial */));
    const auto initialChunkMap = makeChunkMap({initialChunk});

    const auto leftChild = makeChunk(
        keyPattern.globalMin(), splitKey, version(1, 1), thisShardRef(false /* initial */));
    const auto rightChild = makeChunk(
        splitKey, keyPattern.globalMax(), version(1, 2), thisShardRef(false /* initial */));
    const auto updatedChunkMap = initialChunkMap.createMerged({leftChild, rightChild});

    const std::vector<std::shared_ptr<ChunkInfo>> expectedChunks = {leftChild, rightChild};
    validateChunkMap(updatedChunkMap, expectedChunks, true);

    ASSERT_EQ(version(1, 2), updatedChunkMap.getVersion());

    const auto shardVersions = getShardVersionMap(updatedChunkMap);
    ASSERT_EQ(1UL, shardVersions.size());
    assertPlacementVersion(updatedChunkMap, thisShardRef(false /* initial */), version(1, 2));
    assertNoPlacementVersion(updatedChunkMap, thisShardRef(true /* initial */));
}

TEST_P(ChunkMapMixedShardRefTest, MixedShardRefMergeOneShard) {
    const auto& keyPattern = getShardKeyPattern();
    const auto splitKey = BSON("a" << 0);

    const std::vector<std::shared_ptr<ChunkInfo>> initialChunks = {
        makeChunk(
            keyPattern.globalMin(), splitKey, version(1, 1), thisShardRef(true /* initial */)),
        makeChunk(
            splitKey, keyPattern.globalMax(), version(1, 2), thisShardRef(true /* initial */)),
    };
    const auto initialChunkMap = makeChunkMap(initialChunks);

    const auto mergedChunk = makeChunk(keyPattern.globalMin(),
                                       keyPattern.globalMax(),
                                       version(1, 3),
                                       thisShardRef(false /* initial */));
    const auto updatedChunkMap = initialChunkMap.createMerged({mergedChunk});

    validateChunkMap(updatedChunkMap, {mergedChunk}, true);

    ASSERT_EQ(version(1, 3), updatedChunkMap.getVersion());

    const auto shardVersions = getShardVersionMap(updatedChunkMap);
    ASSERT_EQ(1UL, shardVersions.size());
    assertPlacementVersion(updatedChunkMap, thisShardRef(false /* initial */), version(1, 3));
    assertNoPlacementVersion(updatedChunkMap, thisShardRef(true /* initial */));
}

TEST_P(ChunkMapMixedShardRefTest, MixedShardRefSplitOneShardWithOtherShardUntouched) {
    const auto& keyPattern = getShardKeyPattern();
    const auto splitKey = BSON("a" << -50);

    const auto testShardInitialChunk = makeChunk(
        keyPattern.globalMin(), BSON("a" << 0), version(1, 0), thisShardRef(true /* initial */));
    const auto anotherTestShardInitialChunk = makeChunk(
        BSON("a" << 0), keyPattern.globalMax(), version(2, 0), anotherShardRef(true /* initial */));
    const auto initialChunkMap =
        makeChunkMap({testShardInitialChunk, anotherTestShardInitialChunk});

    const auto leftChild = makeChunk(
        keyPattern.globalMin(), splitKey, version(2, 1), thisShardRef(false /* initial */));
    const auto rightChild =
        makeChunk(splitKey, BSON("a" << 0), version(2, 2), thisShardRef(false /* initial */));
    const auto updatedChunkMap = initialChunkMap.createMerged({leftChild, rightChild});

    const std::vector<std::shared_ptr<ChunkInfo>> expectedChunks = {
        leftChild, rightChild, anotherTestShardInitialChunk};
    validateChunkMap(updatedChunkMap, expectedChunks, true);

    ASSERT_EQ(version(2, 2), updatedChunkMap.getVersion());

    const auto shardVersions = getShardVersionMap(updatedChunkMap);
    ASSERT_EQ(2UL, shardVersions.size());
    assertPlacementVersion(updatedChunkMap, thisShardRef(false /* initial */), version(2, 2));
    assertPlacementVersion(updatedChunkMap, anotherShardRef(true /* initial */), version(2, 0));
    assertNoPlacementVersion(updatedChunkMap, thisShardRef(true /* initial */));
    assertNoPlacementVersion(updatedChunkMap, anotherShardRef(false /* initial */));
}

TEST_P(ChunkMapMixedShardRefTest, MixedShardRefMergeOneShardWithOtherShardUntouched) {
    const auto& keyPattern = getShardKeyPattern();
    const auto boundaryKey = BSON("a" << -50);
    const auto upperBoundaryKey = BSON("a" << 0);

    const std::vector<std::shared_ptr<ChunkInfo>> initialChunks = {
        makeChunk(
            keyPattern.globalMin(), boundaryKey, version(2, 0), thisShardRef(true /* initial */)),
        makeChunk(boundaryKey, upperBoundaryKey, version(2, 1), thisShardRef(true /* initial */)),
        makeChunk(upperBoundaryKey,
                  keyPattern.globalMax(),
                  version(2, 2),
                  anotherShardRef(true /* initial */)),
    };
    const auto initialChunkMap = makeChunkMap(initialChunks);

    const auto mergedChunk = makeChunk(
        keyPattern.globalMin(), upperBoundaryKey, version(2, 3), thisShardRef(false /* initial */));
    const auto updatedChunkMap = initialChunkMap.createMerged({mergedChunk});

    validateChunkMap(updatedChunkMap, {mergedChunk, initialChunks[2]}, true);

    ASSERT_EQ(version(2, 3), updatedChunkMap.getVersion());

    const auto shardVersions = getShardVersionMap(updatedChunkMap);
    ASSERT_EQ(2UL, shardVersions.size());
    assertPlacementVersion(updatedChunkMap, thisShardRef(false /* initial */), version(2, 3));
    assertPlacementVersion(updatedChunkMap, anotherShardRef(true /* initial */), version(2, 2));
    assertNoPlacementVersion(updatedChunkMap, thisShardRef(true /* initial */));
    assertNoPlacementVersion(updatedChunkMap, anotherShardRef(false /* initial */));
}

TEST_P(ChunkMapMixedShardRefTest, MixedShardRefMoveBetweenTwoShards) {
    const auto& keyPattern = getShardKeyPattern();
    const auto boundaryKey = BSON("a" << -100);
    const auto upperBoundaryKey = BSON("a" << 0);

    const auto chunkToMoveInitial = makeChunk(
        keyPattern.globalMin(), boundaryKey, version(1, 0), thisShardRef(true /* initial */));
    const auto controlChunkInitial =
        makeChunk(boundaryKey, upperBoundaryKey, version(1, 1), thisShardRef(true /* initial */));
    const auto anotherTestShardInitialChunk = makeChunk(upperBoundaryKey,
                                                        keyPattern.globalMax(),
                                                        version(1, 2),
                                                        anotherShardRef(true /* initial */));
    const auto initialChunkMap =
        makeChunkMap({chunkToMoveInitial, controlChunkInitial, anotherTestShardInitialChunk});

    ChunkType movedChunkType{collUuid(),
                             ChunkRange{keyPattern.globalMin(), boundaryKey},
                             version(2, 1),
                             anotherShardRef(false /* initial */)};
    movedChunkType.setHistory(
        {ChunkHistory{Timestamp{Date_t::now()}, anotherShardRef(false /* initial */)}});
    const auto movedChunk = std::make_shared<ChunkInfo>(movedChunkType);

    const auto controlChunk =
        makeChunk(boundaryKey, upperBoundaryKey, version(2, 0), thisShardRef(false /* initial */));
    const auto updatedChunkMap = initialChunkMap.createMerged({movedChunk, controlChunk});

    validateChunkMap(
        updatedChunkMap, {movedChunk, controlChunk, anotherTestShardInitialChunk}, true);

    ASSERT_EQ(version(2, 1), updatedChunkMap.getVersion());

    const auto shardVersions = getShardVersionMap(updatedChunkMap);
    ASSERT_EQ(3UL, shardVersions.size());
    assertPlacementVersion(updatedChunkMap, anotherShardRef(false /* initial */), version(2, 1));
    assertPlacementVersion(updatedChunkMap, thisShardRef(false /* initial */), version(2, 0));
    assertPlacementVersion(updatedChunkMap, anotherShardRef(true /* initial */), version(1, 2));
    assertNoPlacementVersion(updatedChunkMap, thisShardRef(true /* initial */));
}

}  // namespace

}  // namespace mongo
