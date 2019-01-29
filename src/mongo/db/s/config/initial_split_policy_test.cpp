
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
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using unittest::assertGet;

/**
 * Asserts that the given vectors of BSON objects are equal
 */
void assertBSONObjVectorsAreEqual(const std::vector<BSONObj>& expected,
                                  const std::vector<BSONObj>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (auto expectedIt = expected.begin(), actualIt = actual.begin();
         expectedIt != expected.end() && actualIt != actual.end();
         ++expectedIt, ++actualIt) {
        ASSERT_BSONOBJ_EQ(*expectedIt, *actualIt);
    }
}

/**
 * Asserts that the given vectors of ChunkType objects are equal
 */
void assertChunkVectorsAreEqual(const std::vector<ChunkType>& expected,
                                const std::vector<ChunkType>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (auto expectedIt = expected.begin(), actualIt = actual.begin();
         expectedIt != expected.end() && actualIt != actual.end();
         ++expectedIt, ++actualIt) {
        ASSERT_BSONOBJ_EQ((*expectedIt).toShardBSON().removeField("lastmod"),
                          (*actualIt).toShardBSON().removeField("lastmod"));
    }
}

/**
 * Returns a test hashed shard key pattern if isHashed is true.
 * Otherwise, returns a regular shard key pattern.
 */
ShardKeyPattern makeShardKeyPattern(bool isHashed) {
    if (isHashed)
        return ShardKeyPattern(BSON("x"
                                    << "hashed"));
    return ShardKeyPattern(BSON("x" << 1));
}

/**
 * Calls calculateHashedSplitPointsForEmptyCollection according to the given arguments
 * and asserts that calculated split points match with the expected split points.
 */
void checkCalculatedHashedSplitPoints(bool isHashed,
                                      bool isEmpty,
                                      int numShards,
                                      int numInitialChunks,
                                      const std::vector<BSONObj>* expectedInitialSplitPoints,
                                      const std::vector<BSONObj>* expectedFinalSplitPoints) {
    std::vector<BSONObj> initialSplitPoints;
    std::vector<BSONObj> finalSplitPoints;
    InitialSplitPolicy::calculateHashedSplitPointsForEmptyCollection(makeShardKeyPattern(isHashed),
                                                                     isEmpty,
                                                                     numShards,
                                                                     numInitialChunks,
                                                                     &initialSplitPoints,
                                                                     &finalSplitPoints);
    assertBSONObjVectorsAreEqual(*expectedInitialSplitPoints, initialSplitPoints);
    assertBSONObjVectorsAreEqual(*expectedFinalSplitPoints, finalSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionMoreChunksThanShards) {
    const std::vector<BSONObj> expectedInitialSplitPoints = {BSON("x" << 0)};
    const std::vector<BSONObj> expectedFinalSplitPoints = {
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    checkCalculatedHashedSplitPoints(
        true, true, 2, 4, &expectedInitialSplitPoints, &expectedFinalSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionChunksEqualToShards) {
    const std::vector<BSONObj> expectedSplitPoints = {BSON("x" << -3074457345618258602LL),
                                                      BSON("x" << 3074457345618258602LL)};
    checkCalculatedHashedSplitPoints(true, true, 3, 3, &expectedSplitPoints, &expectedSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionHashedWithNoInitialSplitsReturnsEmptySplits) {
    const std::vector<BSONObj> expectedSplitPoints;
    checkCalculatedHashedSplitPoints(true, true, 2, 1, &expectedSplitPoints, &expectedSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, EmptyCollectionNumInitialChunksZero) {
    const std::vector<BSONObj> expectedInitialSplitPoints = {BSON("x" << 0)};
    const std::vector<BSONObj> expectedFinalSplitPoints = {
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    checkCalculatedHashedSplitPoints(
        true, true, 2, 0, &expectedInitialSplitPoints, &expectedFinalSplitPoints);
}

TEST(CalculateHashedSplitPointsTest, NonEmptyCollectionHashedWithInitialSplitsFails) {
    std::vector<BSONObj> expectedSplitPoints;
    ASSERT_THROWS_CODE(checkCalculatedHashedSplitPoints(
                           true, false, 2, 3, &expectedSplitPoints, &expectedSplitPoints),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

TEST(CalculateHashedSplitPointsTest, NotHashedWithInitialSplitsFails) {
    std::vector<BSONObj> expectedSplitPoints;
    ASSERT_THROWS_CODE(checkCalculatedHashedSplitPoints(
                           false, true, 2, 3, &expectedSplitPoints, &expectedSplitPoints),
                       AssertionException,
                       ErrorCodes::InvalidOptions);
}

class GenerateInitialSplitChunksTestBase : public unittest::Test {
public:
    /**
     * Returns a vector of ChunkType objects for the given chunk ranges.
     * shardIds[i] is the id of shard for the chunk for chunkRanges[i].
     * Checks that chunkRanges and shardIds have the same length.
     */
    const std::vector<ChunkType> makeChunks(const std::vector<ChunkRange> chunkRanges,
                                            const std::vector<ShardId> shardIds) {
        ASSERT_EQ(chunkRanges.size(), shardIds.size());
        std::vector<ChunkType> chunks;

        for (unsigned long i = 0; i < chunkRanges.size(); ++i) {
            ChunkVersion version(1, 0, OID::gen());
            ChunkType chunk(_nss, chunkRanges[i], version, shardIds[i]);
            chunk.setHistory({ChunkHistory(_timeStamp, shardIds[i])});
            chunks.push_back(chunk);
        }
        return chunks;
    }

    /**
     * Returns a vector of numShards shard ids with shard names
     * prefixed by _shardName
     */
    const std::vector<ShardId> makeShardIds(const int numShards) {
        std::vector<ShardId> shardIds;
        for (int i = 0; i < numShards; i++) {
            shardIds.push_back(shardId(std::to_string(i)));
        }
        return shardIds;
    }

    const NamespaceString nss() {
        return _nss;
    }

    const ShardKeyPattern& shardKeyPattern() {
        return _shardKeyPattern;
    }

    const KeyPattern& keyPattern() {
        return _shardKeyPattern.getKeyPattern();
    }

    const ShardId shardId(std::string shardNum) {
        return ShardId(_shardName + shardNum);
    }

    const Timestamp timeStamp() {
        return _timeStamp;
    }

private:
    const NamespaceString _nss{"test.foo"};
    const ShardKeyPattern _shardKeyPattern = makeShardKeyPattern(true);
    const std::string _shardName = "testShard";
    const Timestamp _timeStamp{Date_t::now()};
};

class GenerateInitialHashedSplitChunksTest : public GenerateInitialSplitChunksTestBase {
public:
    const std::vector<BSONObj>& hashedSplitPoints() {
        return _splitPoints;
    }

    const std::vector<ChunkRange>& hashedChunkRanges() {
        return _chunkRanges;
    }

private:
    const std::vector<BSONObj> _splitPoints{
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    const std::vector<ChunkRange> _chunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON("x" << -4611686018427387902LL)),
        ChunkRange(BSON("x" << -4611686018427387902LL), BSON("x" << 0)),
        ChunkRange(BSON("x" << 0), BSON("x" << 4611686018427387902LL)),
        ChunkRange(BSON("x" << 4611686018427387902LL), keyPattern().globalMax()),
    };
};

TEST_F(GenerateInitialHashedSplitChunksTest, NoSplitPoints) {
    const std::vector<BSONObj> splitPoints;
    const std::vector<ShardId> shardIds = makeShardIds(2);
    const auto shardCollectionConfig = InitialSplitPolicy::generateShardCollectionInitialChunks(
        nss(), shardKeyPattern(), shardIds[0], timeStamp(), splitPoints, shardIds);

    // there should only be one chunk
    const auto expectedChunks = makeChunks(
        {ChunkRange(keyPattern().globalMin(), keyPattern().globalMax())}, {shardId("0")});
    assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
}

TEST_F(GenerateInitialHashedSplitChunksTest, SplitPointsMoreThanAvailableShards) {
    const std::vector<ShardId> shardIds = makeShardIds(2);
    const auto shardCollectionConfig = InitialSplitPolicy::generateShardCollectionInitialChunks(
        nss(), shardKeyPattern(), shardIds[0], timeStamp(), hashedSplitPoints(), shardIds);

    // // chunks should be distributed in a round-robin manner
    const std::vector<ChunkType> expectedChunks =
        makeChunks(hashedChunkRanges(), {shardId("0"), shardId("1"), shardId("0"), shardId("1")});
    assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
}

TEST_F(GenerateInitialHashedSplitChunksTest,
       SplitPointsNumContiguousChunksPerShardsGreaterThanOne) {
    const std::vector<ShardId> shardIds = makeShardIds(2);
    const auto shardCollectionConfig = InitialSplitPolicy::generateShardCollectionInitialChunks(
        nss(), shardKeyPattern(), shardIds[0], timeStamp(), hashedSplitPoints(), shardIds, 2);

    // chunks should be distributed in a round-robin manner two chunks at a time
    const std::vector<ChunkType> expectedChunks =
        makeChunks(hashedChunkRanges(), {shardId("0"), shardId("0"), shardId("1"), shardId("1")});
    assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
}

class GenerateShardCollectionInitialZonedChunksTest : public GenerateInitialSplitChunksTestBase {
public:
    /**
     * Calls generateShardCollectionInitialZonedChunks according to the given arguments
     * and asserts that returned chunks match with the chunks created using expectedChunkRanges
     * and expectedShardIds.
     */
    void checkGeneratedInitialZoneChunks(const std::vector<TagsType>& tags,
                                         const int numShards,
                                         const std::vector<ChunkRange>& expectedChunkRanges,
                                         const std::vector<ShardId>& expectedShardIds) {
        const auto shardCollectionConfig =
            InitialSplitPolicy::generateShardCollectionInitialZonedChunks(
                nss(),
                shardKeyPattern(),
                timeStamp(),
                tags,
                makeTagToShards(numShards),
                makeShardIds(numShards));
        const std::vector<ChunkType> expectedChunks =
            makeChunks(expectedChunkRanges, expectedShardIds);
        assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
    }

    const std::string shardKey() {
        return _shardKey;
    }

    const std::string zoneName(std::string zoneNum) {
        return _zoneName + zoneNum;
    }

    TagsType makeTag(const ChunkRange range, std::string zoneName) {
        BSONObjBuilder tagDocBuilder;
        tagDocBuilder.append("_id",
                             BSON(TagsType::ns(nss().ns()) << TagsType::min(range.getMin())));
        tagDocBuilder.append(TagsType::ns(), nss().ns());
        tagDocBuilder.append(TagsType::min(), range.getMin());
        tagDocBuilder.append(TagsType::max(), range.getMax());
        tagDocBuilder.append(TagsType::tag(), zoneName);
        return assertGet(TagsType::fromBSON(tagDocBuilder.obj()));
    }

    /**
     * Returns a map of size numTags mapping _zoneName\d to _shardName\d
     */
    StringMap<std::vector<ShardId>> makeTagToShards(const int numTags) {
        StringMap<std::vector<ShardId>> tagToShards;
        for (int i = 0; i < numTags; i++) {
            tagToShards[zoneName(std::to_string(i))] = {shardId(std::to_string(i))};
        }
        return tagToShards;
    }

private:
    const ShardKeyPattern _shardKeyPattern = makeShardKeyPattern(true);
    const std::string _zoneName = "zoneName";
    const std::string _shardKey = "x";
};

TEST_F(GenerateShardCollectionInitialZonedChunksTest, PredefinedZonesSpanFromMinToMax) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), keyPattern().globalMax()),  // corresponds to a zone
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[0], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0")};
    checkGeneratedInitialZoneChunks(tags, 1, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, PredefinedZonesSpanDoNotSpanFromMinToMax) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 10), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(tags, 2, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, PredefinedZonesContainGlobalMin) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[0], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0")};
    checkGeneratedInitialZoneChunks(tags, 2, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, PredefinedZonesContainGlobalMax) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()),  // corresponds to a zone
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0")};
    checkGeneratedInitialZoneChunks(tags, 2, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, PredefinedZonesContainGlobalMinAndMax) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
        ChunkRange(BSON(shardKey() << 10), keyPattern().globalMax()),  // corresponds to a zone
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[0], zoneName("0")),
                                        makeTag(expectedChunkRanges[2], zoneName("1"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(tags, 2, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, PredefinedZonesContiguous) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),   // corresponds to a zone
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 20), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("1")),
                                        makeTag(expectedChunkRanges[2], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {
        shardId("0"), shardId("1"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(tags, 2, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, PredefinedZonesNotContiguous) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),
        ChunkRange(BSON(shardKey() << 20), BSON(shardKey() << 30)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 30), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("0")),
                                        makeTag(expectedChunkRanges[3], zoneName("1"))};
    const std::vector<ShardId> expectedShardIds = {
        shardId("0"), shardId("0"), shardId("1"), shardId("1"), shardId("2")};
    checkGeneratedInitialZoneChunks(tags, 3, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, NumRemainingChunksGreaterThanNumShards) {
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),
        ChunkRange(BSON(shardKey() << 20), BSON(shardKey() << 30)),
        ChunkRange(BSON(shardKey() << 30), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("0")),
                                        makeTag(expectedChunkRanges[3], zoneName("1"))};
    // shard assignment should wrap around to the first shard
    const std::vector<ShardId> expectedShardIds = {
        shardId("0"), shardId("0"), shardId("1"), shardId("1"), shardId("0")};
    checkGeneratedInitialZoneChunks(tags, 2, expectedChunkRanges, expectedShardIds);
}

TEST_F(GenerateShardCollectionInitialZonedChunksTest, ZoneNotAssociatedWithAnyShardShouldFail) {
    const auto zone1 = zoneName("0");
    const auto zone2 = zoneName("1");

    const std::vector<TagsType> tags{
        makeTag(ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)), zone1),
        makeTag(ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()), zone2)};
    const StringMap<std::vector<ShardId>> tagToShards{{zone1, {ShardId("Shard0")}}, {zone2, {}}};

    ASSERT_THROWS_CODE(
        InitialSplitPolicy::generateShardCollectionInitialZonedChunks(
            nss(), shardKeyPattern(), timeStamp(), tags, tagToShards, makeShardIds(1)),
        AssertionException,
        50973);
}

}  // namespace
}  // namespace mongo
