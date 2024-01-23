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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <iterator>
#include <list>
#include <set>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/create_collection_coordinator.h"
#include "mongo/db/vector_clock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

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

void assertChunkRangesMatch(const std::vector<ChunkRange> expectedRanges,
                            const std::vector<ChunkType>& actualChunks) {
    const auto actualRanges = [&]() {
        std::vector<ChunkRange> actualRanges;
        std::transform(actualChunks.begin(),
                       actualChunks.end(),
                       std::back_inserter(actualRanges),
                       [](auto& chunk) { return chunk.getRange(); });
        return actualRanges;
    }();

    ASSERT_EQ(actualRanges, expectedRanges);
}

int getNumberOfChunksOnShard(const std::vector<ChunkType>& chunks, const ShardId& shardId) {
    return std::count_if(chunks.begin(), chunks.end(), [&shardId](const ChunkType& chunk) {
        return chunk.getShard() == shardId;
    });
}

/**
 * Calls calculateHashedSplitPoints according to the given arguments
 * and asserts that calculated split points match with the expected split points.
 */
void checkCalculatedHashedSplitPoints(const std::vector<BSONObj>& expectedSplitPoints,
                                      const ShardKeyPattern& shardKeyPattern,
                                      size_t numShards) {
    SplitPointsBasedSplitPolicy policy(shardKeyPattern, numShards);
    assertBSONObjVectorsAreEqual(expectedSplitPoints, policy.getSplitPoints());
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixEvenNumberShards) {
    const std::vector<BSONObj> expectedSplitPoints = {
        BSON("x" << -4611686018427387902LL), BSON("x" << 0), BSON("x" << 4611686018427387902LL)};
    checkCalculatedHashedSplitPoints(expectedSplitPoints,
                                     ShardKeyPattern(BSON("x"
                                                          << "hashed")),
                                     4);
}

TEST(CalculateHashedSplitPointsTest, HashedPrefixUnevenNumberShards) {
    const std::vector<BSONObj> expectedSplitPoints = {BSON("x" << -5534023222112865483LL),
                                                      BSON("x" << -1844674407370955161LL),
                                                      BSON("x" << 1844674407370955161LL),
                                                      BSON("x" << 5534023222112865483LL)};
    checkCalculatedHashedSplitPoints(expectedSplitPoints,
                                     ShardKeyPattern(BSON("x"
                                                          << "hashed")),
                                     5);
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

class InitialSingleChunkOnShardSplitChunks : public ConfigServerTestFixture {
public:
    OperationContext* opCtx() {
        return operationContext();
    }

protected:
    const ShardId kShardId0 = ShardId("shard0");

    const ShardId kShardId1 = ShardId("shard1");

    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);

    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12345);

    const ShardType kShard0{kShardId0.toString(), kShardHost0.toString()};

    const ShardType kShard1{kShardId1.toString(), kShardHost1.toString()};

    const ShardKeyPattern kCorrectShardKey = ShardKeyPattern(BSON("_id" << 1));
};

TEST_F(InitialSingleChunkOnShardSplitChunks, InitialSingleChunkOnShardSplitChunks) {
    setupShards({kShard0, kShard1});

    // Wrong shard key.
    ASSERT_THROWS_CODE(
        create_collection_util::createPolicy(
            opCtx(), ShardKeyPattern(BSON("x" << 1)), false, {}, 1, true, true, kShardId0),
        DBException,
        ErrorCodes::InvalidOptions);

    // Non existing shard.
    ASSERT_THROWS_CODE(create_collection_util::createPolicy(
                           opCtx(), kCorrectShardKey, false, {}, 1, true, true, ShardId("shard2")),
                       DBException,
                       ErrorCodes::ShardNotFound);

    // No presplit hashed zones should be passed.
    ASSERT_THROWS_CODE(create_collection_util::createPolicy(
                           opCtx(), kCorrectShardKey, true, {}, 1, true, true, kShardId0),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // Collection must be empty.
    ASSERT_THROWS_CODE(create_collection_util::createPolicy(
                           opCtx(), kCorrectShardKey, false, {}, 1, false, true, kShardId0),
                       DBException,
                       ErrorCodes::InvalidOptions);

    auto singleChunksOnShardPolicy = create_collection_util::createPolicy(
        opCtx(), kCorrectShardKey, false, {}, 1, true, true /*unsplittable*/, kShardId0);

    auto config = singleChunksOnShardPolicy->createFirstChunks(
        opCtx(), kCorrectShardKey, {UUID::gen(), kShardId0});

    ASSERT_EQ(config.chunks.size(), 1);
    ASSERT_EQ(config.chunks[0].getShard(), kShardId0);

    auto singleChunksOnNonPrimaryShardPolicy = create_collection_util::createPolicy(
        opCtx(), kCorrectShardKey, false, {}, 1, true, true /*unsplittable*/, kShardId1);

    auto nonPrimaryConfig = singleChunksOnNonPrimaryShardPolicy->createFirstChunks(
        opCtx(), kCorrectShardKey, {UUID::gen(), kShardId1});

    ASSERT_EQ(nonPrimaryConfig.chunks.size(), 1);
    ASSERT_EQ(nonPrimaryConfig.chunks[0].getShard(), kShardId1);

    auto shardedSingleChunksOnShardPolicy = create_collection_util::createPolicy(
        opCtx(), kCorrectShardKey, false, {}, 1, true, false /*unsplittable*/, kShardId0);

    auto shardedConfig = shardedSingleChunksOnShardPolicy->createFirstChunks(
        opCtx(), kCorrectShardKey, {UUID::gen(), kShardId0});

    ASSERT_EQ(shardedConfig.chunks.size(), 1);
    ASSERT_EQ(shardedConfig.chunks[0].getShard(), kShardId0);

    auto shardedSingleChunksOnNonPrimaryShardPolicy = create_collection_util::createPolicy(
        opCtx(), kCorrectShardKey, false, {}, 1, true, false /*unsplittable*/, kShardId1);

    auto shardedNonPrimaryConfig = shardedSingleChunksOnNonPrimaryShardPolicy->createFirstChunks(
        opCtx(), kCorrectShardKey, {UUID::gen(), kShardId1});

    ASSERT_EQ(shardedNonPrimaryConfig.chunks.size(), 1);
    ASSERT_EQ(shardedNonPrimaryConfig.chunks[0].getShard(), kShardId1);
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
            chunk.setOnCurrentShardSince(timeStamp);
            chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), shardIds[i])});
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
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.foo");
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
        {UUID::gen(), shardIds[0]}, shardKeyPattern(), timeStamp(), splitPoints, shardIds);

    // there should only be one chunk
    const auto expectedChunks =
        makeChunks({ChunkRange(keyPattern().globalMin(), keyPattern().globalMax())},
                   {shardId("0")},
                   timeStamp());
    assertChunkVectorsAreEqual(expectedChunks, shardCollectionConfig.chunks);
}

TEST_F(GenerateInitialHashedSplitChunksTest, SplitPointsMoreThanAvailableShards) {
    const std::vector<ShardId> shardIds = makeShardIds(2);
    const auto shardCollectionConfig = InitialSplitPolicy::generateShardCollectionInitialChunks(
        {UUID::gen(), shardIds[0]}, shardKeyPattern(), timeStamp(), hashedSplitPoints(), shardIds);

    // chunks should be distributed in a round-robin manner
    const std::vector<ChunkType> expectedChunks = makeChunks(
        hashedChunkRanges(), {shardId("0"), shardId("1"), shardId("0"), shardId("1")}, timeStamp());
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

    std::vector<ChunkType> generateInitialZoneChunks(const std::vector<ShardType> shards,
                                                     const std::vector<TagsType>& tags,
                                                     const ShardKeyPattern& shardKeyPattern,
                                                     const ShardId& primaryShard) {
        auto opCtx = operationContext();
        setupShards(shards);
        shardRegistry()->reload(opCtx);
        SingleChunkPerTagSplitPolicy splitPolicy(opCtx, tags);
        return splitPolicy.createFirstChunks(opCtx, shardKeyPattern, {UUID::gen(), primaryShard})
            .chunks;
    }

    std::string shardKey() {
        return _shardKey;
    }

    std::string zoneName(std::string zoneNum) {
        return _zoneName + zoneNum;
    }

    TagsType makeTag(const ChunkRange range, std::string zoneName) {
        BSONObjBuilder tagDocBuilder;
        tagDocBuilder.append(
            "_id", BSON(TagsType::ns(nss().toString_forTest()) << TagsType::min(range.getMin())));
        tagDocBuilder.append(TagsType::ns(), nss().ns_forTest());
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

    const auto generatedChunks = generateInitialZoneChunks(kShards,
                                                           tags,
                                                           ShardKeyPattern(BSON("x"
                                                                                << "hashed")),
                                                           shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    ASSERT_EQ(shardId("0"), generatedChunks[1].getShard());  // corresponds to a zone

    // Shard0 owns the chunk corresponding to the zone plus another one. Shard1 owns just one.
    ASSERT_EQ(2, getNumberOfChunksOnShard(generatedChunks, shardId("0")));
    ASSERT_EQ(1, getNumberOfChunksOnShard(generatedChunks, shardId("1")));
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
    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    ASSERT_EQ(shardId("0"), generatedChunks[0].getShard());  // corresponds to a zone

    // Shard0 owns the chunk corresponding to the zone. The other chunk (gap chunk) is randomly
    // assigned to either shard.
    const auto numChunkOnShard0 = getNumberOfChunksOnShard(generatedChunks, shardId("0"));
    const auto numChunkOnShard1 = getNumberOfChunksOnShard(generatedChunks, shardId("1"));
    ASSERT_TRUE((numChunkOnShard0 == 2 || numChunkOnShard1 == 1));
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
    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    ASSERT_EQ(shardId("0"), generatedChunks[1].getShard());  // corresponds to a zone

    // Shard0 owns the chunk corresponding to the zone. The other chunk (gap chunk) is randomly
    // assigned to either shard.
    const auto numChunkOnShard0 = getNumberOfChunksOnShard(generatedChunks, shardId("0"));
    const auto numChunkOnShard1 = getNumberOfChunksOnShard(generatedChunks, shardId("1"));
    ASSERT_TRUE((numChunkOnShard0 == 2 || numChunkOnShard1 == 1));
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
    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    ASSERT_EQ(shardId("0"), generatedChunks[0].getShard());  // corresponds to a zone
    ASSERT_EQ(shardId("1"), generatedChunks[2].getShard());  // corresponds to a zone
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
    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    ASSERT_EQ(shardId("1"), generatedChunks[1].getShard());  // corresponds to a zone
    ASSERT_EQ(shardId("0"), generatedChunks[2].getShard());  // corresponds to a zone

    // The other two chunks are evenly spread over the two shards.
    ASSERT_EQ(2, getNumberOfChunksOnShard(generatedChunks, shardId("0")));
    ASSERT_EQ(2, getNumberOfChunksOnShard(generatedChunks, shardId("1")));
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
    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    ASSERT_EQ(shardId("0"), generatedChunks[1].getShard());  // corresponds to a zone
    ASSERT_EQ(shardId("1"), generatedChunks[3].getShard());  // corresponds to a zone

    // The three gap chunks get spread evenly over the three shards
    ASSERT_EQ(2, getNumberOfChunksOnShard(generatedChunks, shardId("0")));
    ASSERT_EQ(2, getNumberOfChunksOnShard(generatedChunks, shardId("1")));
    ASSERT_EQ(1, getNumberOfChunksOnShard(generatedChunks, shardId("2")));
}

TEST_F(SingleChunkPerTagSplitPolicyTest, NumRemainingChunksGreaterThanNumShards) {
    const std::vector<ShardType> kShards{
        ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(shardId("1").toString(), "rs1/shard1:123", {zoneName("1")})};
    const std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 10), BSON(shardKey() << 20)),
        ChunkRange(BSON(shardKey() << 20), BSON(shardKey() << 30)),  // corresponds to a zone
        ChunkRange(BSON(shardKey() << 30), keyPattern().globalMax()),
    };
    const std::vector<TagsType> tags = {makeTag(expectedChunkRanges[1], zoneName("0")),
                                        makeTag(expectedChunkRanges[3], zoneName("1"))};
    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    ASSERT_EQ(shardId("0"), generatedChunks[1].getShard());  // corresponds to a zone
    ASSERT_EQ(shardId("1"), generatedChunks[3].getShard());  // corresponds to a zone

    // The three gap chunks get spread over the two shards. One shard will get two of them, and the
    // other just one.
    ASSERT_GTE(getNumberOfChunksOnShard(generatedChunks, shardId("0")), 2);
    ASSERT_GTE(getNumberOfChunksOnShard(generatedChunks, shardId("1")), 2);
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
    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    assertChunkRangesMatch(expectedChunkRanges, generatedChunks);
    // Zoned chunks are assigned round-robin.
    ASSERT_EQ(generatedChunks[0].getShard(), generatedChunks[3].getShard());
    ASSERT_NE(generatedChunks[0].getShard(), generatedChunks[2].getShard());
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

    const auto generatedChunks =
        generateInitialZoneChunks(kShards, tags, ShardKeyPattern(BSON("x" << 1)), shardId("0"));

    // For each tag, chunks are assigned round-robin.
    ASSERT_NE(generatedChunks[0].getShard(), generatedChunks[3].getShard());
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

    ASSERT_THROWS_CODE(SingleChunkPerTagSplitPolicy(operationContext(), tags),
                       DBException,
                       ErrorCodes::CannotCreateChunkDistribution);
}

class PresplitHashedZonesChunksTest : public SingleChunkPerTagSplitPolicyTest {
public:
    /**
     * Calls PresplitHashedZonesSplitPolicy::createFirstChunks() according to the given arguments
     * and asserts that returned chunks match with the chunks created using expectedChunkRanges and
     * expectedShardIds.
     * A 'boost::none' value on expectedShardIds means that the corresponding chunk can be on any
     * shard (because it is a gap/boundary chunk).
     */
    void checkGeneratedInitialZoneChunks(
        const std::vector<ChunkRange>& expectedChunkRanges,
        const std::vector<boost::optional<ShardId>>& expectedShardIds,
        const ShardKeyPattern& shardKeyPattern,
        const std::vector<TagsType>& tags,
        boost::optional<size_t> numInitialChunk = boost::none,
        bool isCollEmpty = true) {
        ShardId primaryShard("doesntMatter");

        PresplitHashedZonesSplitPolicy splitPolicy(
            operationContext(), shardKeyPattern, tags, isCollEmpty);
        const auto shardCollectionConfig = splitPolicy.createFirstChunks(
            operationContext(), shardKeyPattern, {UUID::gen(), primaryShard});

        ASSERT_EQ(expectedShardIds.size(), expectedChunkRanges.size());
        ASSERT_EQ(expectedChunkRanges.size(), shardCollectionConfig.chunks.size());

        ShardId lastHoleShardId("dummy");
        for (size_t i = 0; i < shardCollectionConfig.chunks.size(); ++i) {
            // Check the chunk range matches the expected range.
            ASSERT_EQ(expectedChunkRanges[i], shardCollectionConfig.chunks[i].getRange());

            // Check that the shardId matches the expected.
            if (expectedShardIds[i]) {
                ASSERT_EQ(expectedShardIds[i], shardCollectionConfig.chunks[i].getShard());
            } else {
                // Boundary/hole chunks are assigned to any shard in a round-robin fashion.
                // Note this assert is only valid if there's more than one shard.
                ASSERT_NE(lastHoleShardId, shardCollectionConfig.chunks[i].getShard());
                lastHoleShardId = shardCollectionConfig.chunks[i].getShard();
            }
        }
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

    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {3});
    std::vector<boost::optional<ShardId>> expectedShardIds = {
        shardId("0"), shardId("1"), shardId("2")};
    checkGeneratedInitialZoneChunks(expectedChunkRanges, expectedShardIds, shardKeyPattern, tags);
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

    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1});
    std::vector<boost::optional<ShardId>> expectedShardIds = {boost::none,   // Lower bound.
                                                              shardId("0"),  // Zone 0
                                                              boost::none};  // Upper bound.
    checkGeneratedInitialZoneChunks(expectedChunkRanges, expectedShardIds, shardKeyPattern, tags);
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

    // This should have 5 chunks, 1 for each zone and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1});

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // Lower bound.
        shardId("0"),  // Zone 0
        shardId("1"),  // Zone 1
        shardId("2"),  // Zone 2
        boost::none    // Upper bound.
    };
    checkGeneratedInitialZoneChunks(
        expectedChunkRanges, expectedShardForEachChunk, shardKeyPattern, tags);
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

    // This should have 7 chunks, 3 for zone0, 2 for zone1 and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {3, 2});

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // Lower bound.
        shardId("0"),  // zone 0.
        shardId("3"),  // zone 0.
        shardId("5"),  // zone 0.
        shardId("2"),  // zone 1.
        shardId("4"),  // zone 1.
        boost::none    // Upper bound.
    };
    checkGeneratedInitialZoneChunks(
        expectedChunkRanges, expectedShardForEachChunk, shardKeyPattern, tags);
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

    // This should have 7 chunks, 1 for each zone (3), 2 gaps and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1});
    // The holes should use round-robin to choose a shard.
    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,  // LowerBound.
        shardId("0"),
        boost::none,  // Hole.
        shardId("1"),
        boost::none,  // Hole.
        shardId("2"),
        boost::none,  // UpperBound.
    };
    checkGeneratedInitialZoneChunks(
        expectedChunkRanges, expectedShardForEachChunk, shardKeyPattern, tags);
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

    // This should have 7 chunks, 5 for all zones and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 2, 1} /* numChunksPerTag*/);

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // Lower bound.
        shardId("0"),  // zone0.
        shardId("1"),  // zone1.
        shardId("0"),  // zone2.
        shardId("1"),  // zone2.
        shardId("1"),  // zone3.
        boost::none    // Upper bound.
    };
    checkGeneratedInitialZoneChunks(
        expectedChunkRanges, expectedShardForEachChunk, shardKeyPattern, tags);
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

    // This should have 15 chunks, 9 for all zones, 4 gap and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges =
        buildExpectedChunkRanges(tags, shardKeyPattern, {1, 1, 1, 5, 1} /* numChunksPerTag*/);

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // Lower bound.
        shardId("0"),  // zone0.
        boost::none,   // hole.
        shardId("0"),  // zone1.
        boost::none,   // hole.
        shardId("0"),  // zone2.
        boost::none,   // hole.
        shardId("2"),  // zone3.
        shardId("3"),  // zone3.
        shardId("4"),  // zone3.
        shardId("5"),  // zone3.
        shardId("6"),  // zone3.
        boost::none,   // hole.
        shardId("0"),  // zone4.
        boost::none    // Upper bound.
    };
    checkGeneratedInitialZoneChunks(
        expectedChunkRanges, expectedShardForEachChunk, shardKeyPattern, tags);
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

    // This should have 13 chunks, 7 for all zones, 4 gaps and 2 boundaries.
    std::vector<ChunkRange> expectedChunkRanges = buildExpectedChunkRanges(
        tags, shardKeyPattern, {1, 1 * 2, 1, 1, 1 * 2} /* numChunksPerTag*/);

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // Lower bound.
        shardId("0"),  // tag0.
        boost::none,   // hole.
        shardId("2"),  // tag1.
        shardId("3"),  // tag1.
        boost::none,   // hole.
        shardId("0"),  // tag2.
        boost::none,   // hole.
        shardId("0"),  // tag3.
        boost::none,   // hole.
        shardId("2"),  // tag4.
        shardId("3"),  // tag4.
        boost::none    // Upper bound.
    };
    checkGeneratedInitialZoneChunks(
        expectedChunkRanges, expectedShardForEachChunk, shardKeyPattern, tags);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenNoZones) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123")});
    shardRegistry()->reload(operationContext());

    auto shardKeyPattern = ShardKeyPattern(BSON("x" << 1 << "y"
                                                    << "hashed"));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(operationContext(), shardKeyPattern, {}, true),
        DBException,
        31387);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenCollectionNotEmpty) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("1")})});
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
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, false),
        DBException,
        31387);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenShardKeyNotHashed) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("1")})});
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
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31387);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundPrecedingHashedFieldHasMinKeyOrMaxKey) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("1")})});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b" << 1 << "c"
                                                    << "hashed"
                                                    << "d" << 1));
    auto zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << MINKEY << "d" << MINKEY),
                                BSON("a" << 2 << "b" << 1 << "c" << MINKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31388);
    zoneRange = ChunkRange(BSON("a" << 1 << "b" << MAXKEY << "c" << MINKEY << "d" << MINKEY),
                           BSON("a" << 2 << "b" << 1 << "c" << MINKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31388);
    zoneRange = ChunkRange(BSON("a" << MINKEY << "b" << MINKEY << "c" << MINKEY << "d" << MINKEY),
                           BSON("a" << 2 << "b" << 2 << "c" << MINKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31388);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundOfTheHashedFieldIsNotMinKey) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("1")})});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x" << 1 << "y"
                                                    << "hashed"));
    auto zoneRange = ChunkRange(BSON("x" << 1 << "y" << 1), BSON("x" << 2 << "y" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31389);
    zoneRange = ChunkRange(BSON("x" << 1 << "y" << MAXKEY), BSON("x" << 2 << "y" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31389);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundPrecedingHashedFieldIsSameAsUpperBound) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("1")})});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b" << 1 << "c"
                                                    << "hashed"
                                                    << "d" << 1));
    auto zoneRange = ChunkRange(BSON("a" << 2 << "b" << 2 << "c" << MINKEY << "d" << MINKEY),
                                BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31390);
    zoneRange = ChunkRange(BSON("a" << 1 << "b" << 1 << "c" << MINKEY << "d" << MINKEY),
                           BSON("a" << 1 << "b" << 1 << "c" << MINKEY << "d" << MAXKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31390);
}

TEST_F(PresplitHashedZonesChunksTest, FailsWhenLowerBoundAfterHashedFieldIsNotMinKey) {
    setupShards({ShardType(shardId("0").toString(), "rs0/shard0:123", {zoneName("1")})});
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("a" << 1 << "b"
                                                    << "hashed"
                                                    << "c" << 1 << "d" << 1));
    auto zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << 1 << "d" << MINKEY),
                                BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31391);
    zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << MINKEY << "d" << 1),
                           BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
        DBException,
        31391);

    zoneRange = ChunkRange(BSON("a" << 1 << "b" << MINKEY << "c" << MINKEY << "d" << MAXKEY),
                           BSON("a" << 2 << "b" << 2 << "c" << MAXKEY << "d" << MINKEY));
    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true),
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
        operationContext(), shardKeyPattern, {makeTag(zoneRange, zoneName("1"))}, true);
}

// Validates that building a presplitHashedZones split policy with a set of available shards equal
// to the total of shards works.
TEST_F(PresplitHashedZonesChunksTest, WithAvailableShards) {
    const std::vector<ShardId> kAllShardsIds{shardId("0"), shardId("1")};
    const std::vector<ShardType> kAllShards{
        ShardType(kAllShardsIds[0].toString(), "rs0/shard0:123", {zoneName("0")}),
        ShardType(kAllShardsIds[1].toString(), "rs1/shard1:123", {})};
    setupShards(kAllShards);
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x"
                                                << "hashed"));
    const auto zoneRange = ChunkRange(BSON("x" << MINKEY), BSON("x" << MAXKEY));
    const std::vector<TagsType> tags = {makeTag(zoneRange, zoneName("0"))};

    PresplitHashedZonesSplitPolicy splitPolicy(
        operationContext(), shardKeyPattern, tags, true /* isCollEmpty */, kAllShardsIds);

    const auto config = splitPolicy.createFirstChunks(
        operationContext(), shardKeyPattern, {UUID::gen(), shardId("0")} /* primaryShard */);

    ASSERT_EQ(1, config.chunks.size());
    ASSERT_EQ(shardId("0"), config.chunks[0].getShard());
}

// Validates that building a presplitHashedZones split policy with a set of available shards
// incompatible with the zone distribution throws an error.
TEST_F(PresplitHashedZonesChunksTest, CannotCreateChunkDistribution) {
    const std::vector<ShardId> kAllShardsIds{
        shardId("0"), shardId("1"), shardId("2"), shardId("3")};
    const std::vector<ShardType> kAllShards{
        ShardType(kAllShardsIds[0].toString(), "rs0/shard0:123", {}), /* Available shard */
        ShardType(kAllShardsIds[1].toString(), "rs1/shard1:123", {}), /* Available shard */
        ShardType(kAllShardsIds[2].toString(), "rs2/shard2:123", {}), /* Available shard */
        ShardType(kAllShardsIds[3].toString(),
                  "rs3/shard3:123",
                  {zoneName("0")}) /* Not Available shard */
    };
    const std::vector<ShardId> kAvailableShardIds{shardId("0"), shardId("1"), shardId("2")};
    setupShards(kAllShards);
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x"
                                                << "hashed"));
    const auto zoneRange = ChunkRange(BSON("x" << MINKEY), BSON("x" << MAXKEY));
    const std::vector<TagsType> tags = {makeTag(zoneRange, zoneName("0"))};

    ASSERT_THROWS_CODE(
        PresplitHashedZonesSplitPolicy(
            operationContext(), shardKeyPattern, tags, true /* isCollEmpty */, kAvailableShardIds),
        DBException,
        ErrorCodes::CannotCreateChunkDistribution);
}

TEST_F(PresplitHashedZonesChunksTest, ChunkDistributionWillIgnoreShardNotInAvaiableShards) {
    const std::vector<ShardId> kAllShardIds{shardId("0"), shardId("1"), shardId("2"), shardId("3")};
    const std::vector<ShardType> kAllShards{
        ShardType(kAllShardIds[0].toString(),
                  "rs0/shard0:123",
                  {zoneName("0")}), /* Not available shard with zone */
        ShardType(
            kAllShardIds[1].toString(), "rs1/shard1:123", {}), /* Available shard without zone */
        ShardType(
            kAllShardIds[2].toString(), "rs2/shard2:123", {}), /* Available shard without zone */
        ShardType(kAllShardIds[3].toString(),
                  "rs3/shard3:123",
                  {zoneName("0")}) /* Available shard with zone */};
    const std::vector<ShardId> kAvailableShards{shardId("1"), shardId("2"), shardId("3")};
    setupShards(kAllShards);
    shardRegistry()->reload(operationContext());
    auto shardKeyPattern = ShardKeyPattern(BSON("x"
                                                << "hashed"));
    const auto zoneRange = ChunkRange(BSON("x" << MINKEY), BSON("x" << MAXKEY));
    const std::vector<TagsType> tags = {makeTag(zoneRange, zoneName("0"))};

    PresplitHashedZonesSplitPolicy splitPolicy{
        operationContext(), shardKeyPattern, tags, true /* isCollEmpty */, kAvailableShards};

    const auto config = splitPolicy.createFirstChunks(
        operationContext(), shardKeyPattern, {UUID::gen(), shardId("0")} /* primaryShard */);

    // Validate that shard not in kAvailableShards is not contain in the final chunk
    // distribution.
    std::set<ShardId> involvedShards;
    for (auto&& chunk : config.chunks) {
        const auto shard = chunk.getShard();
        ASSERT_NE(shardId("0"), shard);
        involvedShards.insert(shard);
    }

    // Validate that we have less shards than total.
    ASSERT_LT(involvedShards.size(), kAllShards.size());
}

class MockPipelineSource : public SamplingBasedSplitPolicy::SampleDocumentSource {
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

    Pipeline* getPipeline_forTest() override {
        return nullptr;
    }

private:
    std::list<BSONObj> _toReturn;
};

class SamplingBasedInitSplitTest : public SingleChunkPerTagSplitPolicyTest {
public:
    std::unique_ptr<SamplingBasedSplitPolicy> makeInitialSplitPolicy(
        int numInitialChunks,
        boost::optional<std::vector<TagsType>> zones,
        std::list<BSONObj> samples,
        boost::optional<std::vector<ShardId>> availableShardIds) {
        auto sampleSource = std::make_unique<MockPipelineSource>(std::move(samples));
        return std::make_unique<SamplingBasedSplitPolicy>(
            numInitialChunks, zones, std::move(sampleSource), availableShardIds);
    }

    /**
     * Calls createFirstChunks() according to the given arguments and asserts that returned
     * chunks match with the chunks created using expectedChunkRanges and expectedShardIds.
     */
    void checkGeneratedInitialZoneChunks(
        SamplingBasedSplitPolicy* splitPolicy,
        const ShardKeyPattern& shardKeyPattern,
        const std::vector<ShardType>& shardList,
        const std::vector<ChunkRange>& expectedChunkRanges,
        const std::vector<boost::optional<ShardId>>& expectedShardIds) {
        const ShardId primaryShard("doesntMatter");

        const auto shardCollectionConfig = splitPolicy->createFirstChunks(
            operationContext(), shardKeyPattern, {UUID::gen(), primaryShard});

        ASSERT_EQ(expectedShardIds.size(), expectedChunkRanges.size());
        ASSERT_EQ(expectedChunkRanges.size(), shardCollectionConfig.chunks.size());

        stdx::unordered_map<ShardId, int> shardToNumChunksMap;
        for (const auto& shard : shardList) {
            shardToNumChunksMap[ShardId(shard.getName())] = 0;
        }

        for (size_t i = 0; i < shardCollectionConfig.chunks.size(); ++i) {
            // Check the chunk range matches the expected range.
            ASSERT_EQ(expectedChunkRanges[i], shardCollectionConfig.chunks[i].getRange());

            // Check that the shardId matches the expected.
            const auto& actualShardId = shardCollectionConfig.chunks[i].getShard();
            if (expectedShardIds[i]) {
                ASSERT_EQ(expectedShardIds[i], actualShardId);
            } else {
                // If not in a zone, this chunk goes to whatever shard owns the least number of
                // chunks.
                int minNumChunks = shardToNumChunksMap.begin()->second;
                std::set<ShardId> candidateShards;
                for (const auto& it : shardToNumChunksMap) {
                    if (it.second < minNumChunks) {
                        candidateShards = {it.first};
                        minNumChunks = it.second;
                    } else if (it.second == minNumChunks) {
                        candidateShards.insert(it.first);
                    }
                }
                ASSERT_TRUE(candidateShards.find(actualShardId) != candidateShards.end());
            }

            shardToNumChunksMap[actualShardId]++;
        }
    }

    /**
     * Calls createFirstSplitPoints() according to the given arguments and asserts that returned
     * split points match with the expectedChunkRanges.
     */
    void checkGeneratedInitialSplitPoints(SamplingBasedSplitPolicy* splitPolicy,
                                          const ShardKeyPattern& shardKeyPattern,
                                          const std::vector<ChunkRange>& expectedChunkRanges) {
        const ShardId primaryShard("doesntMatter");
        const auto splitPoints = splitPolicy->createFirstSplitPoints(
            operationContext(), shardKeyPattern, {UUID::gen(), primaryShard});

        const auto expectedNumSplitPoints = expectedChunkRanges.size() - 1;
        ASSERT_EQ(splitPoints.size(), expectedNumSplitPoints);
        auto splitPointsIt = splitPoints.begin();
        for (size_t i = 0; i < expectedNumSplitPoints; i++) {
            ASSERT_BSONOBJ_EQ(*splitPointsIt, expectedChunkRanges[i].getMax());
            std::advance(splitPointsIt, 1);
        }
    }
};

TEST_F(SamplingBasedInitSplitTest, NoZones) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
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

    auto numInitialChunks = 4;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 10)),
        ChunkRange(BSON("y" << 10), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << MAXKEY))};

    // No zones. Chunks assigned round-robin.
    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none, boost::none, boost::none, boost::none};

    checkGeneratedInitialZoneChunks(makeInitialSplitPolicy(numInitialChunks,
                                                           boost::none /* zones */,
                                                           mockSamples,
                                                           boost::none /* availableShardIds */)
                                        .get(),
                                    shardKey,
                                    shardList,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(makeInitialSplitPolicy(numInitialChunks,
                                                            boost::none /* zones */,
                                                            mockSamples,
                                                            boost::none /* availableShardIds */)
                                         .get(),
                                     shardKey,
                                     expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, HashedShardKey) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
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

    auto numInitialChunks = 4;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << -9117533237618642180LL)),
        ChunkRange(BSON("y" << -9117533237618642180LL), BSON("y" << -1196399207910989725LL)),
        ChunkRange(BSON("y" << -1196399207910989725LL), BSON("y" << 7766103514953448109LL)),
        ChunkRange(BSON("y" << 7766103514953448109LL), BSON("y" << MAXKEY))};

    // No zones. Chunks assigned round-robin.
    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none, boost::none, boost::none, boost::none};

    checkGeneratedInitialZoneChunks(makeInitialSplitPolicy(numInitialChunks,
                                                           boost::none /* zones */,
                                                           mockSamples,
                                                           boost::none /* availableShardIds */)
                                        .get(),
                                    shardKey,
                                    shardList,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(makeInitialSplitPolicy(numInitialChunks,
                                                            boost::none /* zones */,
                                                            mockSamples,
                                                            boost::none /* availableShardIds */)
                                         .get(),
                                     shardKey,
                                     expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, SingleInitialChunk) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    auto numInitialChunks = 1;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << MAXKEY))};

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none  // Not in any zone. Can go to any shard.
    };

    checkGeneratedInitialZoneChunks(makeInitialSplitPolicy(numInitialChunks,
                                                           boost::none /* zones */,
                                                           {} /* samples */,
                                                           boost::none /* availableShardIds */)
                                        .get(),
                                    shardKey,
                                    shardList,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(makeInitialSplitPolicy(numInitialChunks,
                                                            boost::none /* zones */,
                                                            {} /* samples */,
                                                            boost::none /* availableShardIds */)
                                         .get(),
                                     shardKey,
                                     expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, ZonesCoversEntireDomainButInsufficient) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
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

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    auto numInitialChunks = 4;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
        ChunkRange(BSON("y" << 0), BSON("y" << 10)),
        ChunkRange(BSON("y" << 10), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << MAXKEY))};

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        shardId("1"), shardId("0"), shardId("0"), shardId("0")};

    checkGeneratedInitialZoneChunks(
        makeInitialSplitPolicy(
            numInitialChunks, zones, mockSamples, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        shardList,
        expectedChunkRanges,
        expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(
        makeInitialSplitPolicy(
            numInitialChunks, zones, mockSamples, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, SamplesCoincidingWithZones) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
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

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 10), BSON("y" << 20)));

    auto numInitialChunks = 4;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 10)),
        ChunkRange(BSON("y" << 10), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << MAXKEY))};

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // hole
        shardId("0"),  // zoneA
        boost::none,   // hole
        boost::none,   // hole
    };

    checkGeneratedInitialZoneChunks(
        makeInitialSplitPolicy(
            numInitialChunks, zones, mockSamples, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        shardList,
        expectedChunkRanges,
        expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(
        makeInitialSplitPolicy(
            numInitialChunks, zones, mockSamples, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, ZoneWithHoles) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << 0), BSON("y" << 20)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 30), BSON("y" << 40)));

    auto numInitialChunks = 4;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
        ChunkRange(BSON("y" << 0), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << 40)),
        ChunkRange(BSON("y" << 40), BSON("y" << MAXKEY))};

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // hole
        shardId("1"),  // zoneB
        boost::none,   // hole
        shardId("0"),  // zoneA
        boost::none,   // hole
    };

    checkGeneratedInitialZoneChunks(
        makeInitialSplitPolicy(
            numInitialChunks, zones, {} /* samples */, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        shardList,
        expectedChunkRanges,
        expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(
        makeInitialSplitPolicy(
            numInitialChunks, zones, {} /* samples */, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, UnsortedZoneWithHoles) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 30), BSON("y" << 40)));
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << 0), BSON("y" << 20)));

    auto numInitialChunks = 4;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
        ChunkRange(BSON("y" << 0), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << 30)),
        ChunkRange(BSON("y" << 30), BSON("y" << 40)),
        ChunkRange(BSON("y" << 40), BSON("y" << MAXKEY))};

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        boost::none,   // hole
        shardId("1"),  // zoneB
        boost::none,   // hole
        shardId("0"),  // zoneA
        boost::none,   // hole
    };

    checkGeneratedInitialZoneChunks(
        makeInitialSplitPolicy(
            numInitialChunks, zones, {} /* samples */, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        shardList,
        expectedChunkRanges,
        expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(
        makeInitialSplitPolicy(
            numInitialChunks, zones, {} /* samples */, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, ZonesIsPrefixOfShardKey) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1 << "z" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    auto numInitialChunks = 2;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY << "z" << MINKEY), BSON("y" << 0 << "z" << MINKEY)),
        ChunkRange(BSON("y" << 0 << "z" << MINKEY), BSON("y" << MAXKEY << "z" << MINKEY)),
        ChunkRange(BSON("y" << MAXKEY << "z" << MINKEY), BSON("y" << MAXKEY << "z" << MAXKEY)),
    };

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        shardId("1"),  // ZoneB
        shardId("0"),  // ZoneA
        boost::none,   // hole
    };

    checkGeneratedInitialZoneChunks(
        makeInitialSplitPolicy(
            numInitialChunks, zones, {} /* samples */, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        shardList,
        expectedChunkRanges,
        expectedShardForEachChunk);
    checkGeneratedInitialSplitPoints(
        makeInitialSplitPolicy(
            numInitialChunks, zones, {} /* samples */, boost::none /* availableShardIds */)
            .get(),
        shardKey,
        expectedChunkRanges);
}

TEST_F(SamplingBasedInitSplitTest, ZonesHasIncompatibleShardKey) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1 << "z" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("x" << MINKEY), BSON("x" << 0)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("x" << 0), BSON("x" << MAXKEY)));

    auto numInitialChunks = 2;
    SplitPolicyParams params{UUID::gen(), shardId("0")};
    {
        auto initSplitPolicy = makeInitialSplitPolicy(
            numInitialChunks, zones, mockSamples, boost::none /* availableShardIds */);
        ASSERT_THROWS(initSplitPolicy->createFirstChunks(operationContext(), shardKey, params),
                      DBException);
    }
    {
        auto initSplitPolicy = makeInitialSplitPolicy(
            numInitialChunks, zones, mockSamples, boost::none /* availableShardIds */);
        ASSERT_THROWS(initSplitPolicy->createFirstSplitPoints(operationContext(), shardKey, params),
                      DBException);
    }
}

TEST_F(SamplingBasedInitSplitTest, InsufficientSamples) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
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

    auto numInitialChunks = 10;
    SplitPolicyParams params{UUID::gen(), shardId("0")};
    {
        auto initSplitPolicy = makeInitialSplitPolicy(numInitialChunks,
                                                      boost::none /* zones */,
                                                      mockSamples,
                                                      boost::none /* availableShardIds */);
        ASSERT_THROWS(initSplitPolicy->createFirstChunks(operationContext(), shardKey, params),
                      DBException);
    }
    {
        auto initSplitPolicy = makeInitialSplitPolicy(numInitialChunks,
                                                      boost::none /* zones */,
                                                      mockSamples,
                                                      boost::none /* availableShardIds */);
        ASSERT_THROWS(initSplitPolicy->createFirstSplitPoints(operationContext(), shardKey, params),
                      DBException);
    }
}

TEST_F(SamplingBasedInitSplitTest, ZeroInitialChunks) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::list<BSONObj> mockSamples;

    auto numInitialChunks = 10;
    SplitPolicyParams params{UUID::gen(), shardId("0")};
    {
        auto initSplitPolicy = makeInitialSplitPolicy(numInitialChunks,
                                                      boost::none /* zones */,
                                                      mockSamples,
                                                      boost::none /* availableShardIds */);
        ASSERT_THROWS(initSplitPolicy->createFirstChunks(operationContext(), shardKey, params),
                      DBException);
    }
    {
        auto initSplitPolicy = makeInitialSplitPolicy(numInitialChunks,
                                                      boost::none /* zones */,
                                                      mockSamples,
                                                      boost::none /* availableShardIds */);
        ASSERT_THROWS(initSplitPolicy->createFirstSplitPoints(operationContext(), shardKey, params),
                      DBException);
    }
}

TEST_F(SamplingBasedInitSplitTest, WithShardIds) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
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

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    std::vector<ShardId> availableShardIds = {shardId("0"), shardId("1")};

    auto numInitialChunks = 4;

    std::vector<ChunkRange> expectedChunkRanges = {
        ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
        ChunkRange(BSON("y" << 0), BSON("y" << 10)),
        ChunkRange(BSON("y" << 10), BSON("y" << 20)),
        ChunkRange(BSON("y" << 20), BSON("y" << MAXKEY))};

    std::vector<boost::optional<ShardId>> expectedShardForEachChunk = {
        shardId("0"), shardId("1"), shardId("1"), shardId("1")};

    checkGeneratedInitialZoneChunks(
        makeInitialSplitPolicy(numInitialChunks, zones, mockSamples, availableShardIds).get(),
        shardKey,
        shardList,
        expectedChunkRanges,
        expectedShardForEachChunk);
}

TEST_F(SamplingBasedInitSplitTest, NoAvailableShardInZone) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    std::vector<ShardId> availableShardIds = {shardId("0")};

    std::list<BSONObj> mockSamples;

    auto numInitialChunks = 10;
    SplitPolicyParams params{UUID::gen(), shardId("0")};
    {
        auto initSplitPolicy = makeInitialSplitPolicy(
            numInitialChunks, boost::none /* zones */, mockSamples, availableShardIds);
        ASSERT_THROWS(initSplitPolicy->createFirstChunks(operationContext(), shardKey, params),
                      DBException);
    }
}


class ShardDistributionInitSplitTest : public SingleChunkPerTagSplitPolicyTest {
public:
    std::unique_ptr<ShardDistributionSplitPolicy> makeInitialSplitPolicy(
        std::vector<ShardKeyRange>& shardDistribution,
        boost::optional<std::vector<TagsType>> zones) {
        return std::make_unique<ShardDistributionSplitPolicy>(shardDistribution, zones);
    }

    /**
     * Calls createFirstChunks() according to the given arguments and asserts that returned
     * chunks match with the chunks created using expectedChunkRanges and expectedShardIds.
     */
    void checkGeneratedInitialZoneChunks(std::unique_ptr<ShardDistributionSplitPolicy> splitPolicy,
                                         const ShardKeyPattern& shardKeyPattern,
                                         const std::vector<ShardType>& shardList,
                                         const std::vector<ShardKeyRange>& shardDistribution,
                                         const std::vector<ChunkRange>& expectedChunkRanges,
                                         const std::vector<ShardId>& expectedShardIds) {
        const ShardId primaryShard("doesntMatter");

        const auto shardCollectionConfig = splitPolicy->createFirstChunks(
            operationContext(), shardKeyPattern, {UUID::gen(), primaryShard});

        ASSERT_EQ(expectedShardIds.size(), expectedChunkRanges.size());
        ASSERT_EQ(expectedChunkRanges.size(), shardCollectionConfig.chunks.size());
        for (size_t i = 0; i < shardCollectionConfig.chunks.size(); ++i) {
            // Check the chunk range matches the expected range.
            ASSERT_EQ(expectedChunkRanges[i], shardCollectionConfig.chunks[i].getRange());

            // Check that the shardId matches the expected.
            const auto& actualShardId = shardCollectionConfig.chunks[i].getShard();
            ASSERT_EQ(expectedShardIds[i], actualShardId);
        }
    }
};

TEST_F(ShardDistributionInitSplitTest, WithoutZones) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());
    ShardKeyRange range0(shardId("0"));
    range0.setMin(BSON("y" << MINKEY));
    range0.setMax(BSON("y" << 0));
    ShardKeyRange range1(shardId("1"));
    range1.setMin(BSON("y" << 0));
    range1.setMax(BSON("y" << MAXKEY));
    std::vector<ShardKeyRange> shardDistribution = {range0, range1};

    std::vector<ChunkRange> expectedChunkRanges = {ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)),
                                                   ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY))};
    std::vector<ShardId> expectedShardForEachChunk = {shardId("0"), shardId("1")};

    checkGeneratedInitialZoneChunks(
        makeInitialSplitPolicy(shardDistribution, boost::none /*zones*/),
        shardKey,
        shardList,
        shardDistribution,
        expectedChunkRanges,
        expectedShardForEachChunk);
}

TEST_F(ShardDistributionInitSplitTest, ZonesConflictShardDistribution) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(
        ShardType(shardId("0").toString(), "rs0/fakeShard0:123", {std::string("zoneA")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    ShardKeyRange range0(shardId("0"));
    range0.setMin(BSON("y" << MINKEY));
    range0.setMax(BSON("y" << 0));
    ShardKeyRange range1(shardId("1"));
    range1.setMin(BSON("y" << 0));
    range1.setMax(BSON("y" << MAXKEY));
    std::vector<ShardKeyRange> shardDistribution = {range0, range1};

    SplitPolicyParams params{UUID::gen(), shardId("0")};
    auto initSplitPolicy = makeInitialSplitPolicy(shardDistribution, zones);
    ASSERT_THROWS(initSplitPolicy->createFirstChunks(operationContext(), shardKey, params),
                  DBException);
}

TEST_F(ShardDistributionInitSplitTest, InterleaveWithZones) {
    const NamespaceString ns = NamespaceString::createNamespaceString_forTest("foo", "bar");
    const ShardKeyPattern shardKey(BSON("y" << 1));

    std::vector<ShardType> shardList;
    shardList.emplace_back(ShardType(shardId("0").toString(),
                                     "rs0/fakeShard0:123",
                                     {std::string("zoneA"), std::string("zoneB")}));
    shardList.emplace_back(
        ShardType(shardId("1").toString(), "rs1/fakeShard1:123", {std::string("zoneB")}));

    setupShards(shardList);
    shardRegistry()->reload(operationContext());

    std::vector<TagsType> zones;
    zones.emplace_back(nss(), "zoneA", ChunkRange(BSON("y" << MINKEY), BSON("y" << 0)));
    zones.emplace_back(nss(), "zoneB", ChunkRange(BSON("y" << 0), BSON("y" << MAXKEY)));

    ShardKeyRange range0(shardId("0"));
    range0.setMin(BSON("y" << MINKEY));
    range0.setMax(BSON("y" << -1));
    ShardKeyRange range1(shardId("0"));
    range1.setMin(BSON("y" << -1));
    range1.setMax(BSON("y" << 1));
    ShardKeyRange range2(shardId("1"));
    range2.setMin(BSON("y" << 1));
    range2.setMax(BSON("y" << MAXKEY));
    std::vector<ShardKeyRange> shardDistribution = {range0, range1, range2};

    std::vector<ChunkRange> expectedChunkRanges = {ChunkRange(BSON("y" << MINKEY), BSON("y" << -1)),
                                                   ChunkRange(BSON("y" << -1), BSON("y" << 0)),
                                                   ChunkRange(BSON("y" << 0), BSON("y" << 1)),
                                                   ChunkRange(BSON("y" << 1), BSON("y" << MAXKEY))};
    std::vector<ShardId> expectedShardForEachChunk = {
        shardId("0"), shardId("0"), shardId("0"), shardId("1")};

    checkGeneratedInitialZoneChunks(makeInitialSplitPolicy(shardDistribution, zones),
                                    shardKey,
                                    shardList,
                                    shardDistribution,
                                    expectedChunkRanges,
                                    expectedShardForEachChunk);
}

}  // namespace
}  // namespace mongo
