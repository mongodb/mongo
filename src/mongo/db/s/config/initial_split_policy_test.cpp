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

#include "mongo/bson/json.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

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
        ASSERT_BSONOBJ_EQ((*expectedIt).toShardBSON().removeField("lastmod").removeField("history"),
                          (*actualIt).toShardBSON().removeField("lastmod").removeField("history"));
    }
}

/**
 * Calls calculateHashedSplitPoints according to the given arguments
 * and asserts that calculated split points match with the expected split points.
 */
void checkCalculatedHashedSplitPoints(const ShardKeyPattern& shardKeyPattern,
                                      int numShards,
                                      int numInitialChunks,
                                      const std::vector<BSONObj>* expectedSplitPoints,
                                      int expectNumChunkPerShard) {
    SplitPointsBasedSplitPolicy policy(shardKeyPattern, numShards, numInitialChunks);
    assertBSONObjVectorsAreEqual(*expectedSplitPoints, policy.getSplitPoints());
    ASSERT_EQUALS(expectNumChunkPerShard, policy.getNumContiguousChunksPerShard());
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixMoreChunksThanShardsWithEqualDistribution) {
    auto shardKeyPattern = ShardKeyPattern(BSON("x"
                                                << "hashed"
                                                << "y" << 1));
    const std::vector<BSONObj> expectedSplitPoints = {
        BSON("x" << -4611686018427387902LL << "y" << MINKEY),
        BSON("x" << 0 << "y" << MINKEY),
        BSON("x" << 4611686018427387902LL << "y" << MINKEY)};
    int expectNumChunkPerShard = 2;
    checkCalculatedHashedSplitPoints(
        shardKeyPattern, 2, 4, &expectedSplitPoints, expectNumChunkPerShard);
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixMoreChunksThanShardsWithUnequalDistribution) {
    auto shardKeyPattern = ShardKeyPattern(BSON("x"
                                                << "hashed"));
    const std::vector<BSONObj> expectedSplitPoints = {
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    int expectNumChunkPerShard = 1;
    checkCalculatedHashedSplitPoints(
        shardKeyPattern, 3, 4, &expectedSplitPoints, expectNumChunkPerShard);
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixChunksEqualToShards) {
    auto shardKeyPattern = ShardKeyPattern(BSON("x"
                                                << "hashed"
                                                << "y" << 1));
    const std::vector<BSONObj> expectedSplitPoints = {
        BSON("x" << -3074457345618258602LL << "y" << MINKEY),
        BSON("x" << 3074457345618258602LL << "y" << MINKEY)};
    int expectNumChunkPerShard = 1;
    checkCalculatedHashedSplitPoints(
        shardKeyPattern, 3, 3, &expectedSplitPoints, expectNumChunkPerShard);
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixChunksLessThanShards) {
    const std::vector<BSONObj> expectedSplitPoints = {BSON("x" << 0)};
    int expectNumChunkPerShard = 1;
    checkCalculatedHashedSplitPoints(ShardKeyPattern(BSON("x"
                                                          << "hashed")),
                                     5,
                                     2,
                                     &expectedSplitPoints,
                                     expectNumChunkPerShard);
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixChunksOneReturnsNoSplitPoints) {
    const std::vector<BSONObj> expectedSplitPoints;
    int expectNumChunkPerShard = 1;
    checkCalculatedHashedSplitPoints(ShardKeyPattern(BSON("x"
                                                          << "hashed")),
                                     2,
                                     1,
                                     &expectedSplitPoints,
                                     expectNumChunkPerShard);
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixChunksZeroUsesDefault) {
    const std::vector<BSONObj> expectedSplitPoints = {
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    int expectNumChunkPerShard = 2;
    checkCalculatedHashedSplitPoints(ShardKeyPattern(BSON("x"
                                                          << "hashed")),
                                     2,
                                     0,
                                     &expectedSplitPoints,
                                     expectNumChunkPerShard);
}

TEST(CalculateHashedSplitPointsTest, HashedSuffix) {
    auto shardKeyPattern = ShardKeyPattern(BSON("x.a" << 1 << "y.b" << 1 << "z.c"
                                                      << "hashed"));
    const auto preDefinedPrefix = fromjson("{'x.a': {p: 1}, 'y.b': 'val'}");
    const std::vector<BSONObj> expectedSplitPoints = {
        BSONObjBuilder(preDefinedPrefix).append("z.c", -4611686018427387902LL).obj(),
        BSONObjBuilder(preDefinedPrefix).append("z.c", 0LL).obj(),
        BSONObjBuilder(preDefinedPrefix).append("z.c", 4611686018427387902LL).obj()};
    assertBSONObjVectorsAreEqual(
        expectedSplitPoints,
        InitialSplitPolicy::calculateHashedSplitPoints(shardKeyPattern, preDefinedPrefix, 4));
}

TEST(CalculateHashedSplitPointsTest, HashedInfix) {
    auto shardKeyPattern = ShardKeyPattern(BSON("x.a" << 1 << "y.b"
                                                      << "hashed"
                                                      << "z.c" << 1 << "a" << 1));
    const auto preDefinedPrefix = fromjson("{'x.a': {p: 1}}");
    const std::vector<BSONObj> expectedSplitPoints = {BSONObjBuilder(preDefinedPrefix)
                                                          .append("y.b", -4611686018427387902LL)
                                                          .appendMinKey("z.c")
                                                          .appendMinKey("a")
                                                          .obj(),
                                                      BSONObjBuilder(preDefinedPrefix)
                                                          .append("y.b", 0LL)
                                                          .appendMinKey("z.c")
                                                          .appendMinKey("a")
                                                          .obj(),
                                                      BSONObjBuilder(preDefinedPrefix)
                                                          .append("y.b", 4611686018427387902LL)
                                                          .appendMinKey("z.c")
                                                          .appendMinKey("a")
                                                          .obj()};
    assertBSONObjVectorsAreEqual(
        expectedSplitPoints,
        InitialSplitPolicy::calculateHashedSplitPoints(shardKeyPattern, preDefinedPrefix, 4));
}

class GenerateInitialSplitChunksTestBase : public ConfigServerTestFixture {
public:
    /**
     * Returns a vector of ChunkType objects for the given chunk ranges.
     * shardIds[i] is the id of shard for the chunk for chunkRanges[i].
     * Checks that chunkRanges and shardIds have the same length.
     */
    std::vector<ChunkType> makeChunks(const std::vector<ChunkRange> chunkRanges,
                                      const std::vector<ShardId> shardIds,
                                      Timestamp timeStamp) {
        ASSERT_EQ(chunkRanges.size(), shardIds.size());
        std::vector<ChunkType> chunks;

        for (unsigned long i = 0; i < chunkRanges.size(); ++i) {
            ChunkVersion version({OID::gen(), Timestamp(1, 1)}, {1, 0});
            ChunkType chunk(_uuid, chunkRanges[i], version, shardIds[i]);
            chunk.setHistory({ChunkHistory(timeStamp, shardIds[i])});
            chunks.push_back(chunk);
        }
        return chunks;
    }

    /**
     * Returns a vector of numShards shard ids with shard names
     * prefixed by _shardName
     */
    std::vector<ShardId> makeShardIds(const int numShards) {
        std::vector<ShardId> shardIds;
        for (int i = 0; i < numShards; i++) {
            shardIds.push_back(shardId(std::to_string(i)));
        }
        return shardIds;
    }

    NamespaceString nss() {
        return _nss;
    }

    const UUID& uuid() {
        return _uuid;
    }

    const ShardKeyPattern& shardKeyPattern() {
        return _shardKeyPattern;
    }

    const KeyPattern& keyPattern() {
        return _shardKeyPattern.getKeyPattern();
    }

    ShardId shardId(std::string shardNum) {
        return ShardId(_shardName + shardNum);
    }

    Timestamp timeStamp() {
        return _timeStamp;
    }

private:
    const NamespaceString _nss{"test.foo"};
    const UUID _uuid{UUID::gen()};
    const ShardKeyPattern _shardKeyPattern = ShardKeyPattern(BSON("x"
                                                                  << "hashed"));
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
        {UUID::gen(), shardIds[0]}, shardKeyPattern(), timeStamp(), splitPoints, shardIds, 1);

    // there should only be one chunk
    const auto expectedChunks =
        makeChunks({ChunkRange(keyPattern().globalMin(), keyPattern().globalMax())},
                   {shardId("0")},
                   timeStamp());
    assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
}

TEST_F(GenerateInitialHashedSplitChunksTest, SplitPointsMoreThanAvailableShards) {
    const std::vector<ShardId> shardIds = makeShardIds(2);
    const auto shardCollectionConfig =
        InitialSplitPolicy::generateShardCollectionInitialChunks({UUID::gen(), shardIds[0]},
                                                                 shardKeyPattern(),
                                                                 timeStamp(),
                                                                 hashedSplitPoints(),
                                                                 shardIds,
                                                                 1);

    // chunks should be distributed in a round-robin manner
    const std::vector<ChunkType> expectedChunks = makeChunks(
        hashedChunkRanges(), {shardId("0"), shardId("1"), shardId("0"), shardId("1")}, timeStamp());
    assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
}

TEST_F(GenerateInitialHashedSplitChunksTest,
       SplitPointsNumContiguousChunksPerShardsGreaterThanOne) {
    const std::vector<ShardId> shardIds = makeShardIds(2);
    const auto shardCollectionConfig =
        InitialSplitPolicy::generateShardCollectionInitialChunks({UUID::gen(), shardIds[0]},
                                                                 shardKeyPattern(),
                                                                 timeStamp(),
                                                                 hashedSplitPoints(),
                                                                 shardIds,
                                                                 2);

    // chunks should be distributed in a round-robin manner two chunks at a time
    const std::vector<ChunkType> expectedChunks = makeChunks(
        hashedChunkRanges(), {shardId("0"), shardId("0"), shardId("1"), shardId("1")}, timeStamp());
    assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
}

class SingleChunkPerTagSplitPolicyTest : public GenerateInitialSplitChunksTestBase {
public:
    /**
     * Calls SingleChunkPerTagSplitPolicy::createFirstChunks() according to the given arguments and
     * asserts that returned chunks match with the chunks created using expectedChunkRanges and
     * expectedShardIds.
     */
    void checkGeneratedInitialZoneChunks(const std::vector<ShardType> shards,
                                         const std::vector<TagsType>& tags,
                                         const std::vector<ChunkRange>& expectedChunkRanges,
                                         const std::vector<ShardId>& expectedShardIds,
                                         const ShardKeyPattern& shardKeyPattern) {
        auto opCtx = operationContext();
        setupShards(shards);
        shardRegistry()->reload(opCtx);
        SingleChunkPerTagSplitPolicy splitPolicy(opCtx, tags);
        const auto shardCollectionConfig = splitPolicy.createFirstChunks(
            opCtx, shardKeyPattern, {UUID::gen(), expectedShardIds.front()});

        const auto currentTime = VectorClock::get(opCtx)->getTime();
        const std::vector<ChunkType> expectedChunks = makeChunks(
            expectedChunkRanges, expectedShardIds, currentTime.clusterTime().asTimestamp());
        assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
    }

    std::string shardKey() {
        return _shardKey;
    }

    std::string zoneName(std::string zoneNum) {
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

private:
    const ShardKeyPattern _shardKeyPattern = ShardKeyPattern(BSON("x"
                                                                  << "hashed"));
    const std::string _zoneName = "zoneName";
    const std::string _shardKey = "x";
};

TEST_F(SingleChunkPerTagSplitPolicyTest, PredefinedZonesSpanFromMinToMax) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123")};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), keyPattern().globalMax()),  // corresponds to a zone
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[0], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0")};
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, PredefinedZonesSpanDoNotSpanFromMinToMax) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123")};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 10), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(kShards,
                                    tags,
                                    expectedChunkRanges,
                                    expectedShardIds,
                                    ShardKeyPattern(BSON("x"
                                                         << "hashed")));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, PredefinedZonesContainGlobalMin) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123")};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[0], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0")};
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, PredefinedZonesContainGlobalMax) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123")};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()),  // corresponds to a zone
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("0"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0")};
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, PredefinedZonesContainGlobalMinAndMax) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("1")})};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
        ChunkRange(BSON(shardKey() << 10), keyPattern().globalMax()),  // corresponds to a zone
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[0], zoneName("0")),
                                        makeTag(expectedChunkRanges[2], zoneName("1"))};
    const std::vector<ShardId> expectedShardIds = {shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, PredefinedZonesContiguous) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("1")})};
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
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, PredefinedZonesNotContiguous) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("1")}),
        ShardType(shardId("2").toString(), "rs1/shard1:123")};
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
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, NumRemainingChunksGreaterThanNumShards) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("1")})};
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
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, MultipleChunksToOneZoneWithMultipleShards) {
    const auto zone0 = zoneName("Z0");
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zone0}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zone0})};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),   // zone0
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),     // gap
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),    // zone0
        ChunkRange(BSON(shardKey() << 20), keyPattern().globalMax()),  // zone0
    };
    const std::vector<TagsType> tags = {
        makeTag(expectedChunkRanges.at(0), zone0),
        makeTag(expectedChunkRanges.at(2), zone0),
        makeTag(expectedChunkRanges.at(3), zone0),
    };
    const std::vector<ShardId> expectedShardIds = {
        shardId("0"), shardId("0"), shardId("1"), shardId("0")};
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, MultipleChunksToInterleavedZonesWithMultipleShards) {
    const auto zone0 = zoneName("Z0");
    const auto zone1 = zoneName("Z1");
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zone0, zone1}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zone0, zone1})};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),   // zone0
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),     // zone1
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),    // gap
        ChunkRange(BSON(shardKey() << 20), keyPattern().globalMax()),  // zone0
    };
    const std::vector<TagsType> tags = {
        makeTag(expectedChunkRanges.at(0), zone0),
        makeTag(expectedChunkRanges.at(1), zone1),
        makeTag(expectedChunkRanges.at(3), zone0),
    };

    const std::vector<ShardId> expectedShardIds = {
        shardId("0"), shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(
        kShards, tags, expectedChunkRanges, expectedShardIds, ShardKeyPattern(BSON("x" << 1)));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, ZoneNotAssociatedWithAnyShardShouldFail) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123")};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());
    const auto zone1 = zoneName("0");
    const auto zone2 = zoneName("1");

    const std::vector<TagsType> tags{
        makeTag(ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)), zone1),
        makeTag(ChunkRange(BSON(shardKey() << 0), keyPattern().globalMax()), zone2)};

    SingleChunkPerTagSplitPolicy splitPolicy(operationContext(), tags);

    ASSERT_THROWS_CODE(splitPolicy.createFirstChunks(operationContext(),
                                                     shardKeyPattern(),
                                                     {UUID::gen(), ShardId("shardId")}),
                       AssertionException,
                       50973);
}

class PresplitHashedZonesChunksTest : public SingleChunkPerTagSplitPolicyTest {
public:
    /**
     * Calls PresplitHashedZonesSplitPolicy::createFirstChunks() according to the given arguments
     * and asserts that returned chunks match with the chunks created using expectedChunkRanges and
     * expectedShardIds.
     */
    void checkGeneratedInitialZoneChunks(const std::vector<TagsType>& tags,
                                         const std::vector<ChunkRange>& expectedChunkRanges,
                                         const std::vector<ShardId>& expectedShardIds,
                                         const ShardKeyPattern& shardKeyPattern,
                                         int numInitialChunk,
                                         bool isCollEmpty = true) {
        PresplitHashedZonesSplitPolicy splitPolicy(
            operationContext(), shardKeyPattern, tags, numInitialChunk, isCollEmpty);
        const auto shardCollectionConfig = splitPolicy.createFirstChunks(
            operationContext(), shardKeyPattern, {UUID::gen(), expectedShardIds.front()});

        const auto currentTime = VectorClock::get(operationContext())->getTime();
        const std::vector<ChunkType> expectedChunks = makeChunks(
            expectedChunkRanges, expectedShardIds, currentTime.clusterTime().asTimestamp());
        assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
    }
};

/**
 * Builds a BSON object using the lower bound of the given tag. The hashed field will be replaced
 * with the input value.
 */
BSONObj buildObj(const ShardKeyPattern& shardKeyPattern, TagsType tag, long long value) {
    BSONObjBuilder bob;
    for (auto&& elem : tag.getMinKey()) {
        if (shardKeyPattern.getHashedField().fieldNameStringData() == elem.fieldNameStringData()) {
            bob.appendNumber(elem.fieldNameStringData(), value);
        } else {
            bob.append(elem);
        }
    }
    return bob.obj();
}

/**
 * Generates chunk ranges for each tag using the split points.
 */
std::vector<ChunkRange> buildExpectedChunkRanges(const std::vector<TagsType>& tags,
                                                 const ShardKeyPattern& shardKeyPattern,
                                                 const std::vector<int> numChunksPerTag) {

    // The hashed values are intentionally hard coded here, so that the behaviour of
    // 'InitialSplitPolicy::calculateHashedSplitPoints()' is also tested.
    auto getHashedSplitPoints = [&](int numChunks) -> std::vector<long long> {
        switch (numChunks) {
            case 1:
                return {};
            case 2:
                return {0LL};
            case 3:
                return {-3074457345618258602LL, 3074457345618258602LL};
            case 4:
                return {-4611686018427387902LL, 0LL, 4611686018427387902LL};
            case 5:
                return {-5534023222112865483LL,
                        -1844674407370955161LL,
                        1844674407370955161LL,
                        5534023222112865483LL};
            case 6:
                return {-6148914691236517204LL,
                        -3074457345618258602LL,
                        0LL,
                        3074457345618258602LL,
                        6148914691236517204LL};
            default:
                auto splitPoints = InitialSplitPolicy::calculateHashedSplitPoints(
                    shardKeyPattern, BSONObj(), numChunks);
                auto field = shardKeyPattern.getHashedField();
                std::vector<long long> output;
                for (auto&& splitPoint : splitPoints) {
                    output.push_back(splitPoint[field.fieldName()].numberLong());
                }
                return output;
        };
        MONGO_UNREACHABLE;
    };
    ASSERT(!tags.empty() && tags.size() == numChunksPerTag.size());

    std::vector<ChunkRange> output;
    output.reserve(numChunksPerTag.size() * tags.size());

    // Global MinKey to first tag's start value. We only add this chunk if the first tag's min bound
    // isn't already global MinKey.
    if (tags[0].getMinKey().woCompare(shardKeyPattern.getKeyPattern().globalMin())) {
        output.push_back(
            ChunkRange(shardKeyPattern.getKeyPattern().globalMin(), tags[0].getMinKey()));
    }
    for (size_t tagIdx = 0; tagIdx < tags.size(); tagIdx++) {
        auto tag = tags[tagIdx];

        // If there is a gap between consecutive tags (previous tag's MaxKey and current tag's
        // MinKey), create a chunk to fill the gap.
        if ((tagIdx != 0 && (tags[tagIdx - 1].getMaxKey().woCompare(tag.getMinKey())))) {
            output.push_back(ChunkRange(tags[tagIdx - 1].getMaxKey(), tag.getMinKey()));
        }

        std::vector<long long> hashedSplitValues = getHashedSplitPoints(numChunksPerTag[tagIdx]);
        // Generated single chunk for tag if there are no split points.
        if (hashedSplitValues.empty()) {
            output.push_back(ChunkRange(tag.getMinKey(), tag.getMaxKey()));
            continue;
        }
        output.push_back(
            ChunkRange(tag.getMinKey(), buildObj(shardKeyPattern, tag, hashedSplitValues[0])));

        // Generate 'n-1' chunks using the split values.
        for (size_t i = 0; i < hashedSplitValues.size() - 1; i++) {
            output.push_back(ChunkRange(buildObj(shardKeyPattern, tag, hashedSplitValues[i]),
                                        buildObj(shardKeyPattern, tag, hashedSplitValues[i + 1])));
        }
        output.push_back(ChunkRange(
            buildObj(shardKeyPattern, tag, hashedSplitValues[hashedSplitValues.size() - 1]),
            tag.getMaxKey()));
    }
    // Last tag's end value to global MaxKey. We only add this chunk if the last tag's max bound
    // isn't already global MaxKey.
    if (tags[tags.size() - 1].getMaxKey().woCompare(shardKeyPattern.getKeyPattern().globalMax())) {
        output.push_back(ChunkRange(tags[tags.size() - 1].getMaxKey(),
                                    shardKeyPattern.getKeyPattern().globalMax()));
    }
    return output;
}

TEST_F(PresplitHashedZonesChunksTest, WithHashedPrefix) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("0")}),
        ShardType(shardId("2").toString(), "rs1/shard1:123", {zoneName("0")})};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x"
                                                << "hashed"
                                                << "y" << 1));
    const auto zoneRange =
        ChunkRange(BSON("x" << MINKEY << "y" << MINKEY), BSON("x" << MAXKEY << "y" << MAXKEY));
    const std::vector<TagsType> tags = {makeTag(zoneRange, zoneName("0"))};

    // numInitialChunks = 0.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {2 * 3});
    std::vector<ShardId> expectedShardIds = {
        shardId("0"), shardId("0"), shardId("1"), shardId("1"), shardId("2"), shardId("2")};
    checkGeneratedInitialZoneChunks(
        tags, expectedChunkRanges, expectedShardIds, shardKeyPattern, 0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {3});
    expectedShardIds = {shardId("0"), shardId("1"), shardId("2")};
    checkGeneratedInitialZoneChunks(
        tags, expectedChunkRanges, expectedShardIds, shardKeyPattern, 1 /* numInitialChunks*/);

    // numInitialChunks = 4.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {6});
    expectedShardIds = {
        shardId("0"), shardId("0"), shardId("1"), shardId("1"), shardId("2"), shardId("2")};
    checkGeneratedInitialZoneChunks(
        tags, expectedChunkRanges, expectedShardIds, shardKeyPattern, 4 /* numInitialChunks*/);
}


TEST_F(PresplitHashedZonesChunksTest, SingleZone) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123")};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x" << 1 << "y"
                                                    << "hashed"));
    const auto zoneRange = ChunkRange(BSON("x"
                                           << "UK"
                                           << "y" << MINKEY),
                                      BSON("x"
                                           << "US"
                                           << "y" << MINKEY));
    const std::vector<TagsType> tags = {makeTag(zoneRange, zoneName("0"))};

    // numInitialChunks = 0.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {2});
    std::vector<ShardId> expectedShardIds = {
        shardId("0"), shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(
        tags, expectedChunkRanges, expectedShardIds, shardKeyPattern, 0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {1});
    expectedShardIds = {shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(
        tags, expectedChunkRanges, expectedShardIds, shardKeyPattern, 1 /* numInitialChunks*/);

    // numInitialChunks = 3.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {3});
    expectedShardIds = {shardId("0"), shardId("0"), shardId("0"), shardId("0"), shardId("1")};
    checkGeneratedInitialZoneChunks(
        tags, expectedChunkRanges, expectedShardIds, shardKeyPattern, 3 /* numInitialChunks*/);
}

TEST_F(PresplitHashedZonesChunksTest, WithMultipleZonesContiguous) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("1")}),
        ShardType(shardId("2").toString(), "rs1/shard1:123", {zoneName("2")})};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("country" << 1 << "city" << 1 << "hashedField"
                                                          << "hashed"
                                                          << "suffix" << 1));
    const std::vector<TagsType> tags = {
        makeTag(ChunkRange(BSON("country"
                                << "IE"
                                << "city"
                                << "Dublin"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "UK"
                                << "city"
                                << "London"
                                << "hashedField" << MINKEY << "suffix" << MINKEY)),
                zoneName("0")),
        makeTag(ChunkRange(BSON("country"
                                << "UK"
                                << "city"
                                << "London"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "US"
                                << "city"
                                << "NewYork"
                                << "hashedField" << MINKEY << "suffix" << MINKEY)),
                zoneName("1")),
        makeTag(
            ChunkRange(BSON("country"
                            << "US"
                            << "city"
                            << "NewYork"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "US"
                            << "city" << MAXKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("2"))};

    // numInitialChunks = 0.
    // This should have 8 chunks, 2 for each zone and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {2, 2, 2});

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),
        shardId("0"),
        shardId("1"),
        shardId("1"),
        shardId("2"),
        shardId("2"),
        shardId("1")  // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    // This should have 5 chunks, 1 for each zone and 2 boundaries.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1});

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),
        shardId("1"),
        shardId("2"),
        shardId("1")  // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    1 /* numInitialChunks*/);

    // numInitialChunks = 10.
    // This should have 14 chunks, 4 for each zone and 2 boundaries.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {4, 4, 4});

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),
        shardId("0"),
        shardId("0"),
        shardId("0"),
        shardId("1"),
        shardId("1"),
        shardId("1"),
        shardId("1"),
        shardId("2"),
        shardId("2"),
        shardId("2"),
        shardId("2"),
        shardId("1")  // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    10 /* numInitialChunks*/);
}

TEST_F(PresplitHashedZonesChunksTest, MultipleContiguousZonesWithEachZoneHavingMultipleShards) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard5:123"),
        ShardType(shardId("2").toString(), "rs0/shard1:123", {zoneName("1")}),
        ShardType(shardId("3").toString(), "rs0/shard2:123", {zoneName("0")}),
        ShardType(shardId("4").toString(), "rs1/shard3:123", {zoneName("1")}),
        ShardType(shardId("5").toString(), "rs1/shard4:123", {zoneName("0")})};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("country" << 1 << "city" << 1 << "hashedField"
                                                          << "hashed"
                                                          << "suffix" << 1));
    const std::vector<TagsType> tags = {
        makeTag(ChunkRange(BSON("country"
                                << "IE"
                                << "city"
                                << "Dublin"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "UK"
                                << "city"
                                << "London"
                                << "hashedField" << MINKEY << "suffix" << MINKEY)),
                zoneName("0")),
        makeTag(ChunkRange(BSON("country"
                                << "UK"
                                << "city"
                                << "London"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "US"
                                << "city"
                                << "Los Angeles"
                                << "hashedField" << MINKEY << "suffix" << MINKEY)),
                zoneName("1"))};

    // numInitialChunks = 0.
    // This should have 12 chunks, 6 for zone0, 4 for zone1 and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {6, 4});

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone 0.
        shardId("0"),  // zone 0.
        shardId("3"),  // zone 0.
        shardId("3"),  // zone 0.
        shardId("5"),  // zone 0.
        shardId("5"),  // zone 0.
        shardId("2"),  // zone 1.
        shardId("2"),  // zone 1.
        shardId("4"),  // zone 1.
        shardId("4"),  // zone 1.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    // This should have 7 chunks, 5 for all zones and 2 boundaries.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {3, 2});

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("3"),  // zone0.
        shardId("5"),  // zone0.
        shardId("2"),  // zone1.
        shardId("4"),  // zone1.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    1 /* numInitialChunks*/);

    // numInitialChunks = 5.
    // This should have 7 chunks, 5 for all zones and 2 boundaries.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {3, 2});

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("3"),  // zone0.
        shardId("5"),  // zone0.
        shardId("2"),  // zone1.
        shardId("4"),  // zone1.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    5 /* numInitialChunks*/);
}

TEST_F(PresplitHashedZonesChunksTest, MultipleZonesWithGaps) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("noZone").toString(), "rs1/shard1:123"),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("1")}),
        ShardType(shardId("2").toString(), "rs1/shard1:123", {zoneName("2")})};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("country" << 1 << "city" << 1 << "hashedField"
                                                          << "hashed"
                                                          << "suffix" << 1));
    const std::vector<TagsType> tags = {
        makeTag(ChunkRange(BSON("country"
                                << "IE"
                                << "city"
                                << "Dublin"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "UK"
                                << "city" << MINKEY << "hashedField" << MINKEY << "suffix"
                                << "SomeValue")),
                zoneName("0")),
        makeTag(ChunkRange(BSON("country"
                                << "UK"
                                << "city"
                                << "London"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "US"
                                << "city" << MINKEY << "hashedField" << 100LL << "suffix"
                                << "someValue")),
                zoneName("1")),
        makeTag(
            ChunkRange(BSON("country"
                            << "US"
                            << "city"
                            << "NewYork"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "US"
                            << "city" << MAXKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("2"))};

    // numInitialChunks = 0.
    // This should have 10 chunks, 2 for each zone (6), 2 gaps and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {2, 2, 2});
    // The holes should use round-robin to choose a shard.
    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // LowerBound.
        shardId("0"),
        shardId("0"),
        shardId("1"),  // Hole.
        shardId("1"),
        shardId("1"),
        shardId("2"),  // Hole.
        shardId("2"),
        shardId("2"),
        shardId("noZone"),  // UpperBound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    // This should have 7 chunks, 1 for each zone (3), 2 gaps and 2 boundaries.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1});
    // The holes should use round-robin to choose a shard.
    expectedShardForEachChunk = {
        shardId("0"),  // LowerBound.
        shardId("0"),
        shardId("1"),  // Hole.
        shardId("1"),
        shardId("2"),  // Hole.
        shardId("2"),
        shardId("noZone"),  // UpperBound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    1 /* numInitialChunks*/);

    // numInitialChunks = 12.
    // This should have 16 chunks, 4 for each zone (12), 2 gaps and 2 boundaries.
    expectedChunkRanges = buildExpectedChunkRanges(tags, shardKeyPattern, {4, 4, 4});
    // The holes should use round-robin to choose a shard.
    expectedShardForEachChunk = {
        shardId("0"),  // LowerBound.
        shardId("0"),
        shardId("0"),
        shardId("0"),
        shardId("0"),
        shardId("1"),  // Hole.
        shardId("1"),
        shardId("1"),
        shardId("1"),
        shardId("1"),
        shardId("2"),  // Hole.
        shardId("2"),
        shardId("2"),
        shardId("2"),
        shardId("2"),
        shardId("noZone"),  // UpperBound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    12 /* numInitialChunks*/);
}

TEST_F(PresplitHashedZonesChunksTest, MultipleContiguousZonesWithEachShardHavingMultipleZones) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0"), zoneName("2")}),
        ShardType(shardId("1").toString(),
                  "rs1/shard1:123",
                  {zoneName("1"), zoneName("2"), zoneName("3")}),
        ShardType(shardId("2").toString(), "rs1/shard1:123")};
    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("country" << 1 << "city" << 1 << "hashedField"
                                                          << "hashed"
                                                          << "suffix" << 1));

    // No gap between the zone ranges.
    const std::vector<TagsType> tags = {
        makeTag(ChunkRange(BSON("country"
                                << "IE"
                                << "city"
                                << "Dublin"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "UK"
                                << "city"
                                << "London"
                                << "hashedField" << MINKEY << "suffix" << MINKEY)),
                zoneName("0")),
        makeTag(ChunkRange(BSON("country"
                                << "UK"
                                << "city"
                                << "London"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "US"
                                << "city"
                                << "Los Angeles"
                                << "hashedField" << MINKEY << "suffix" << MINKEY)),
                zoneName("1")),
        makeTag(ChunkRange(BSON("country"
                                << "US"
                                << "city"
                                << "Los Angeles"
                                << "hashedField" << MINKEY << "suffix" << MINKEY),
                           BSON("country"
                                << "US"
                                << "city"
                                << "New York"
                                << "hashedField" << MINKEY << "suffix" << MINKEY)),
                zoneName("2")),
        makeTag(
            ChunkRange(BSON("country"
                            << "US"
                            << "city"
                            << "New York"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "US"
                            << "city" << MAXKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("3"))};

    // numInitialChunks = 0.
    // This should have 7 chunks, 5 for all zones and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 2, 1} /* numChunksPerTag*/);

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("1"),  // zone1.
        shardId("0"),  // zone2.
        shardId("1"),  // zone2.
        shardId("1"),  // zone3.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    // This should have 7 chunks, 5 for all zones and 2 boundaries.
    expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 2, 1} /* numChunksPerTag*/);

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("1"),  // zone1.
        shardId("0"),  // zone2.
        shardId("1"),  // zone2.
        shardId("1"),  // zone3.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    1 /* numInitialChunks*/);

    // numInitialChunks = 7.
    // This should have 10 chunks, 10 for all zones and 2 boundaries.
    expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {2, 2, 4, 2} /* numChunksPerTag*/);

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("0"),  // zone0.
        shardId("1"),  // zone1.
        shardId("1"),  // zone1.
        shardId("0"),  // zone2.
        shardId("0"),  // zone2.
        shardId("1"),  // zone2.
        shardId("1"),  // zone2.
        shardId("1"),  // zone3.
        shardId("1"),  // zone3.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    7 /* numInitialChunks*/);
}

TEST_F(PresplitHashedZonesChunksTest, OneLargeZoneAndOtherSmallZonesSharingASingleShard) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(),
                  "rs0/shard0:123",
                  {zoneName("0"), zoneName("1"), zoneName("2"), zoneName("4")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123"),
        ShardType(shardId("2").toString(), "rs1/shard1:123", {zoneName("3")}),
        ShardType(shardId("3").toString(), "rs1/shard1:123", {zoneName("3")}),
        ShardType(shardId("4").toString(), "rs1/shard1:123", {zoneName("3")}),
        ShardType(shardId("5").toString(), "rs1/shard1:123", {zoneName("3")}),
        ShardType(shardId("6").toString(), "rs1/shard1:123", {zoneName("3")}),
    };
    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("country" << 1 << "city" << 1 << "hashedField"
                                                          << "hashed"
                                                          << "suffix" << 1));

    // With gaps after each zone ranges.
    const std::vector<TagsType> tags = {
        makeTag(
            ChunkRange(BSON("country"
                            << "country1"
                            << "city"
                            << "city1"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country2"
                            << "city" << MINKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("0")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country2"
                            << "city"
                            << "city2"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country3"
                            << "city" << MINKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("1")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country3"
                            << "city"
                            << "city3"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country4"
                            << "city" << MINKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("2")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country4"
                            << "city"
                            << "city4"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country5"
                            << "city" << MAXKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("3")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country6"  // Skip country 5.
                            << "city"
                            << "city5"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country7"
                            << "city" << MAXKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("4"))};

    // numInitialChunks = 0.
    // This should have 20 chunks, 14 for all zones, 4 gaps and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1, 2 * 5, 1} /* numChunksPerTag*/);

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("1"),  // hole.
        shardId("0"),  // zone1.
        shardId("2"),  // hole.
        shardId("0"),  // zone2.
        shardId("3"),  // hole.
        shardId("2"),  // zone3.
        shardId("2"),  // zone3.
        shardId("3"),  // zone3.
        shardId("3"),  // zone3.
        shardId("4"),  // zone3.
        shardId("4"),  // zone3.
        shardId("5"),  // zone3.
        shardId("5"),  // zone3.
        shardId("6"),  // zone3.
        shardId("6"),  // zone3.
        shardId("4"),  // hole.
        shardId("0"),  // zone4.
        shardId("5")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    // This should have 15 chunks, 9 for all zones, 4 gap and 2 boundaries.
    expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1, 5, 1} /* numChunksPerTag*/);

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("1"),  // hole.
        shardId("0"),  // zone1.
        shardId("2"),  // hole.
        shardId("0"),  // zone2.
        shardId("3"),  // hole.
        shardId("2"),  // zone3.
        shardId("3"),  // zone3.
        shardId("4"),  // zone3.
        shardId("5"),  // zone3.
        shardId("6"),  // zone3.
        shardId("4"),  // hole.
        shardId("0"),  // zone4.
        shardId("5")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    1 /* numInitialChunks*/);

    // numInitialChunks = 11.
    // This should have 10 chunks, 10 for all zones and 2 boundaries.
    expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1, 10, 1} /* numChunksPerTag*/);

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // zone0.
        shardId("1"),  // hole.
        shardId("0"),  // zone1.
        shardId("2"),  // hole.
        shardId("0"),  // zone2.
        shardId("3"),  // hole.
        shardId("2"),  // zone3.
        shardId("2"),  // zone3.
        shardId("3"),  // zone3.
        shardId("3"),  // zone3.
        shardId("4"),  // zone3.
        shardId("4"),  // zone3.
        shardId("5"),  // zone3.
        shardId("5"),  // zone3.
        shardId("6"),  // zone3.
        shardId("6"),  // zone3.
        shardId("4"),  // hole.
        shardId("0"),  // zone4.
        shardId("5")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    11 /* numInitialChunks*/);
}

TEST_F(PresplitHashedZonesChunksTest, InterweavingZones) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0"), zoneName("2")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123"),
        ShardType(shardId("2").toString(), "rs1/shard1:123", {zoneName("1")}),
        ShardType(shardId("3").toString(), "rs1/shard1:123", {zoneName("1")}),
    };
    setupShards(kShards);
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("country" << 1 << "city" << 1 << "hashedField"
                                                          << "hashed"
                                                          << "suffix" << 1));

    // With gaps after each zone ranges.
    const std::vector<TagsType> tags = {
        makeTag(
            ChunkRange(BSON("country"
                            << "country1"
                            << "city"
                            << "city1"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country2"
                            << "city" << MINKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("0")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country2"
                            << "city"
                            << "city2"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country3"
                            << "city" << MINKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("1")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country3"
                            << "city"
                            << "city3"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country4"
                            << "city" << MINKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("2")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country4"
                            << "city"
                            << "city4"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country5"
                            << "city" << MAXKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("0")),
        makeTag(
            ChunkRange(BSON("country"
                            << "country6"
                            << "city"
                            << "city5"
                            << "hashedField" << MINKEY << "suffix" << MINKEY),
                       BSON("country"
                            << "country7"
                            << "city" << MAXKEY << "hashedField" << MINKEY << "suffix" << MINKEY)),
            zoneName("1"))};

    // numInitialChunks = 0.
    // This should have 13 chunks, 7 for all zones, 4 gaps and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges = buildExpectedChunkRanges(
        tags, shardKeyPattern, {1, 1 * 2, 1, 1, 1 * 2} /* numChunksPerTag*/);

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // tag0.
        shardId("1"),  // hole.
        shardId("2"),  // tag1.
        shardId("3"),  // tag1.
        shardId("2"),  // hole.
        shardId("0"),  // tag2.
        shardId("3"),  // hole.
        shardId("0"),  // tag3.
        shardId("0"),  // hole.
        shardId("2"),  // tag4.
        shardId("3"),  // tag4.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    0 /* numInitialChunks*/);

    // numInitialChunks = 1.
    // This should have 13 chunks, 7 for all zones, 4 gaps and 2 boundaries.
    expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 2, 1, 1, 2} /* numChunksPerTag*/);
    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // tag0.
        shardId("1"),  // hole.
        shardId("2"),  // tag1.
        shardId("3"),  // tag1.
        shardId("2"),  // hole.
        shardId("0"),  // tag2.
        shardId("3"),  // hole.
        shardId("0"),  // tag3.
        shardId("0"),  // hole.
        shardId("2"),  // tag4.
        shardId("3"),  // tag4.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    1 /* numInitialChunks*/);

    // numInitialChunks = 7.
    // This should have 17 chunks, 11 for all zones and 6 gaps + boundary.
    expectedChunkRanges = buildExpectedChunkRanges(
        tags, shardKeyPattern, {1, 2 * 2, 1, 1, 2 * 2} /* numChunksPerTag*/);

    expectedShardForEachChunk = {
        shardId("0"),  // Lower bound.
        shardId("0"),  // tag0.
        shardId("1"),  // hole.
        shardId("2"),  // tag1.
        shardId("2"),  // tag1.
        shardId("3"),  // tag1.
        shardId("3"),  // tag1.
        shardId("2"),  // hole.
        shardId("0"),  // tag2.
        shardId("3"),  // hole.
        shardId("0"),  // tag3.
        shardId("0"),  // hole.
        shardId("2"),  // tag4.
        shardId("2"),  // tag4.
        shardId("3"),  // tag4.
        shardId("3"),  // tag4.
        shardId("1")   // Upper bound.
    };
    checkGeneratedInitialZoneChunks(tags,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk,
                                    shardKeyPattern,
                                    7 /* numInitialChunks*/);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenNoZones) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("x" << 1 << "y"
                                                    << "hashed"));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(operationContext(), shardKeyPattern, {}, 0, true),
        DBException,
        31387);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenCollectionNotEmpty) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x" << 1 << "y"
                                                    << "hashed"));
    const auto zoneRange = ChunkRange(BSON("x"
                                           << "UK"
                                           << "y" << MINKEY),
                                      BSON("x"
                                           << "US"
                                           << "y" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, false),
        DBException,
        31387);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenShardKeyNotHashed) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x" << 1 << "y" << 1));
    const auto zoneRange = ChunkRange(BSON("x"
                                           << "UK"
                                           << "y" << MINKEY),
                                      BSON("x"
                                           << "US"
                                           << "y" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31387);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundPrecedingHashedFieldHasMinKeyOrMaxKey) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b" << 1 << "c"
                                                    << "hashed"
                                                    << "d" << 1));
    auto zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << MINKEY << "d" << MINKEY),
                                BSON("a" << 2 << "b" << 1 << "c" << MINKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31388);
    zoneRange = ChunkRange(BSON("a" << 1 << "b" << MAXKEY << "c" << MINKEY << "d" << MINKEY),
                           BSON("a" << 2 << "b" << 1 << "c" << MINKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31388);
    zoneRange = ChunkRange(BSON("a" << MINKEY << "b" << MINKEY << "c" << MINKEY << "d" << MINKEY),
                           BSON("a" << 2 << "b" << 2 << "c" << MINKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31388);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundOfTheHashedFieldIsNotMinKey) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x" << 1 << "y"
                                                    << "hashed"));
    auto zoneRange = ChunkRange(BSON("x" << 1 << "y" << 1), BSON("x" << 2 << "y" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31389);
    zoneRange = ChunkRange(BSON("x" << 1 << "y" << MAXKEY), BSON("x" << 2 << "y" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31389);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundPrecedingHashedFieldIsSameAsUpperBound) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b" << 1 << "c"
                                                    << "hashed"
                                                    << "d" << 1));
    auto zoneRange = ChunkRange(BSON("a" << 2 << "b" << 2 << "c" << MINKEY << "d" << MINKEY),
                                BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31390);
    zoneRange = ChunkRange(BSON("a" << 1 << "b" << 1 << "c" << MINKEY << "d" << MINKEY),
                           BSON("a" << 1 << "b" << 1 << "c" << MINKEY << "d" << MAXKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31390);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundAfterHashedFieldIsNotMinKey) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b"
                                                    << "hashed"
                                                    << "c" << 1 << "d" << 1));
    auto zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << 1 << "d" << MINKEY),
                                BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31391);
    zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << MINKEY << "d" << 1),
                           BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31391);

    zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << MINKEY << "d" << MAXKEY),
                           BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true),
        DBException,
        31391);
}

TEST_F(PresplitHashedZonesChunksTest, RestrictionsDoNotApplyToUpperBound) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("1")})});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b"
                                                    << "hashed"
                                                    << "c" << 1 << "d" << 1));
    auto zoneRange =
        ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << MINKEY << "d" << MINKEY),
                   BSON("a" << MAXKEY << "b" << MAXKEY << "c" << MAXKEY << "d" << MAXKEY));
    PresplitHashedZonesSplitPolicy(
        operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, 0, true);
}

class MockPipelineSource : public ReshardingSplitPolicy::SampleDocumentSource {
public:
    MockPipelineSource(std::list<BSONObj> toReturn) : _toReturn(std::move(toReturn)) {}

    boost::optional<BSONObj> getNext() override {
        if (_toReturn.empty()) {
            return {};
        }

        auto next = _toReturn.front();
        _toReturn.pop_front();
        return next;
    }

private:
    std::list<BSONObj> _toReturn;
};

class ReshardingInitSplitTest : public SingleChunkPerTagSplitPolicyTest {
public:
    /**
     * Calls createFirstChunks() according to the given arguments and asserts that returned chunks
     * match with the chunks created using expectedChunkRanges and expectedShardIds.
     */
    void checkGeneratedInitialZoneChunks(ReshardingSplitPolicy* splitPolicy,
                                         const ShardKeyPattern& shardKeyPattern,
                                         const std::vector<ChunkRange>& expectedChunkRanges,
                                         const std::vector<ShardId>& expectedShardIds) {
        const auto shardCollectionConfig = splitPolicy->createFirstChunks(
            operationContext(), shardKeyPattern, {UUID::gen(), expectedShardIds.front()});

        const auto currentTime = VectorClock::get(operationContext())->getTime();
        const std::vector<ChunkType> expectedChunks = makeChunks(
            expectedChunkRanges, expectedShardIds, currentTime.clusterTime().asTimestamp());
        assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
    }
};

TEST_F(ReshardingInitSplitTest, NoZones) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    mockSamples.push_back(BSON("y" << 10));
    mockSamples.push_back(BSON("y" << 20));
    mockSamples.push_back(BSON("y" << 30));

    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    ReshardingSplitPolicy initSplitPolicy(
        4 /* numInitialChunks */, boost::none /* zones */, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 10)),
        ChunkRange(BSON("y" << 10), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << MAXKEY))};

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"), shardId("1"), shardId("0"), shardId("1")};

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, HashedShardKey) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y"
                                        << "hashed"));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    mockSamples.push_back(BSON("y" << 7766103514953448109LL));
    mockSamples.push_back(BSON("y" << -9117533237618642180LL));
    mockSamples.push_back(BSON("y" << -1196399207910989725LL));

    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    ReshardingSplitPolicy initSplitPolicy(
        4 /* numInitialChunks */, boost::none /* zones */, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << -9117533237618642180LL)),
        ChunkRange(BSON("y" << -9117533237618642180LL), BSON("y" << -1196399207910989725LL)),
        ChunkRange(BSON("y" << -1196399207910989725LL), BSON("y" << 7766103514953448109LL)),
        ChunkRange(BSON("y" << 7766103514953448109LL), BSON("y" << MAXKEY))};

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"), shardId("1"), shardId("0"), shardId("1")};

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, SingleInitialChunk) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;

    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    ReshardingSplitPolicy initSplitPolicy(
        1 /* numInitialChunks */, boost::none /* zones */, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << MAXKEY))};

    std::vector<ShardId> expectedShardForEachChunk = {shardId("0")};

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, ZonesCoversEntireDomainButInsufficient) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    mockSamples.push_back(BSON("y" << 10));
    mockSamples.push_back(BSON("y" << 20));
    mockSamples.push_back(BSON("y" << 30));

    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    ReshardingSplitPolicy initSplitPolicy(
        4 /* numInitialChunks */, zones, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
        ChunkRange(BSON("y" << 0), BSON("y" << 10)),
        ChunkRange(BSON("y" << 10), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << MAXKEY))};

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("1"), shardId("0"), shardId("0"), shardId("0")};

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, SamplesCoincidingWithZones) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    mockSamples.push_back(BSON("y" << 10));
    mockSamples.push_back(BSON("y" << 20));
    mockSamples.push_back(BSON("y" << 30));

    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 10), BSON("y" << 20)));

    ReshardingSplitPolicy initSplitPolicy(
        4 /* numInitialChunks */, zones, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 10)),
        ChunkRange(BSON("y" << 10), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << MAXKEY))};

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // hole
        shardId("0"),  // zoneA
        shardId("1"),  // hole
        shardId("1"),  // hole
    };

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, ZoneWithHoles) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;

    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << 0), BSON("y" << 20)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 30), BSON("y" << 40)));

    ReshardingSplitPolicy initSplitPolicy(
        4 /* numInitialChunks */, zones, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
        ChunkRange(BSON("y" << 0), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << 40)),
        ChunkRange(BSON("y" << 40), BSON("y" << MAXKEY))};

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // hole
        shardId("1"),  // zoneB
        shardId("0"),  // hole
        shardId("0"),  // zoneA
        shardId("1"),  // hole
    };

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, UnsortedZoneWithHoles) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;

    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 30), BSON("y" << 40)));
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << 0), BSON("y" << 20)));

    ReshardingSplitPolicy initSplitPolicy(
        4 /* numInitialChunks */, zones, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
        ChunkRange(BSON("y" << 0), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << 40)),
        ChunkRange(BSON("y" << 40), BSON("y" << MAXKEY))};

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"),  // hole
        shardId("1"),  // zoneB
        shardId("0"),  // hole
        shardId("0"),  // zoneA
        shardId("1"),  // hole
    };

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, ZonesIsPrefixOfReshardKey) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1 << "z" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    ReshardingSplitPolicy initSplitPolicy(
        2 /* numInitialChunks */, zones, std::move(mockSampleSource));

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY << "z" << MINKEY), BSON("y" << 0 << "z" << MINKEY)),
        ChunkRange(BSON("y" << 0 << "z" << MINKEY), BSON("y" << MAXKEY << "z" << MINKEY)),
        ChunkRange(BSON("y" << MAXKEY << "z" << MINKEY), BSON("y" << MAXKEY << "z" << MAXKEY)),
    };

    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("1"),
        shardId("0"),
        shardId("0"),
    };

    checkGeneratedInitialZoneChunks(
        &initSplitPolicy, shardKey, expectedChunkRanges, expectedShardForEachChunk);
}

TEST_F(ReshardingInitSplitTest, ZonesHasIncompatibleReshardKey) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1 << "z" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("x" << MINKEY), BSON("x" << 0)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("x" << 0), BSON("x" << MAXKEY)));

    ReshardingSplitPolicy initSplitPolicy(
        2 /* numInitialChunks */, zones, std::move(mockSampleSource));

    SplitPolicyParams params{UUID::gen(), shardId("0")};
    ASSERT_THROWS(initSplitPolicy.createFirstChunks(operationContext(), shardKey, params),
                  DBException);
}

TEST_F(ReshardingInitSplitTest, InsufficientSamples) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    mockSamples.push_back(BSON("x" << 10 << "y" << 10));
    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    ReshardingSplitPolicy initSplitPolicy(
        10 /* numInitialChunks */, boost::none /* zones */, std::move(mockSampleSource));

    SplitPolicyParams params{UUID::gen(), shardId("0")};
    ASSERT_THROWS(initSplitPolicy.createFirstChunks(operationContext(), shardKey, params),
                  DBException);
}

TEST_F(ReshardingInitSplitTest, ZeroInitialChunks) {
    const NamespaceString ns("reshard", "foo");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;
    auto mockSampleSource = std::make_unique<MockPipelineSource>(std::move(mockSamples));

    ASSERT_THROWS(ReshardingSplitPolicy(0 /* numInitialChunks */,
                                        boost::none /* zones */,
                                        std::move(mockSampleSource)),
                  DBException);
}

}  // namespace
}  // namespace mongo
