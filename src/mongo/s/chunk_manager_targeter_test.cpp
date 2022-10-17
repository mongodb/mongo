/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/hasher.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");

auto buildUpdate(const NamespaceString& nss, BSONObj query, BSONObj update, bool upsert) {
    write_ops::UpdateCommandRequest updateOp(nss);
    write_ops::UpdateOpEntry entry;
    entry.setQ(query);
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
    entry.setUpsert(upsert);
    updateOp.setUpdates(std::vector{entry});
    return BatchedCommandRequest{std::move(updateOp)};
}

auto buildDelete(const NamespaceString& nss, BSONObj query) {
    write_ops::DeleteCommandRequest deleteOp(nss);
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(false);
    deleteOp.setDeletes(std::vector{entry});
    return BatchedCommandRequest{std::move(deleteOp)};
}

class ChunkManagerTargeterTest : public CatalogCacheTestFixture {
public:
    ChunkManagerTargeter prepare(BSONObj shardKeyPattern, const std::vector<BSONObj>& splitPoints) {
        chunkManager =
            makeChunkManager(kNss, ShardKeyPattern(shardKeyPattern), nullptr, false, splitPoints);
        return ChunkManagerTargeter(operationContext(), kNss);
    };
    boost::optional<ChunkManager> chunkManager;

protected:
    bool checkChunkRanges = false;

    void testTargetInsertWithRangePrefixHashedShardKeyCommon(
        OperationContext* opCtx, const ChunkManagerTargeter& cmTargeter);
    void testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager();
    void testTargetInsertWithRangePrefixHashedShardKey();
    void testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix();
    void testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix();
    void testTargetUpdateWithRangePrefixHashedShardKey();
    void testTargetUpdateWithHashedPrefixHashedShardKey();
    void testTargetDeleteWithRangePrefixHashedShardKey();
    void testTargetDeleteWithHashedPrefixHashedShardKey();
    void testTargetDeleteWithExactId();
};

class ChunkManagerTargeterWithChunkRangesTest : public ChunkManagerTargeterTest {
public:
    ChunkManagerTargeterWithChunkRangesTest() {
        checkChunkRanges = true;
    };

protected:
    void testTargetInsertWithRangePrefixHashedShardKey() {
        ChunkManagerTargeterTest::testTargetInsertWithRangePrefixHashedShardKey();
    };

    void testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager() {
        ChunkManagerTargeterTest::testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager();
    };

    void testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix() {
        ChunkManagerTargeterTest::testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix();
    };

    void testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix() {
        ChunkManagerTargeterTest::testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix();
    };

    void testTargetUpdateWithRangePrefixHashedShardKey() {
        ChunkManagerTargeterTest::testTargetUpdateWithRangePrefixHashedShardKey();
    };

    void testTargetUpdateWithHashedPrefixHashedShardKey() {
        ChunkManagerTargeterTest::testTargetUpdateWithHashedPrefixHashedShardKey();
    }

    void testTargetDeleteWithRangePrefixHashedShardKey() {
        ChunkManagerTargeterTest::testTargetDeleteWithRangePrefixHashedShardKey();
    }

    void testTargetDeleteWithHashedPrefixHashedShardKey() {
        ChunkManagerTargeterTest::testTargetDeleteWithHashedPrefixHashedShardKey();
    }

    void testTargetDeleteWithExactId() {
        ChunkManagerTargeterTest::testTargetDeleteWithExactId();
    }
};

/**
 * This is the common part of test TargetInsertWithRangePrefixHashedShardKey and
 * TargetInsertWithRangePrefixHashedShardKeyCustomChunkManager
 * Tests that the destination shard is the correct one as defined from the split points
 * when the ChunkManager was constructed.
 */
void ChunkManagerTargeterTest::testTargetInsertWithRangePrefixHashedShardKeyCommon(
    OperationContext* opCtx, const ChunkManagerTargeter& cmTargeter) {
    std::set<ChunkRange> chunkRanges;

    // Caller has created 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1'
    // has chunk [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    auto res = cmTargeter.targetInsert(
        opCtx, fromjson("{a: {b: -111}, c: {d: '1'}}"), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "1");

    res = cmTargeter.targetInsert(
        opCtx, fromjson("{a: {b: -10}}"), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "2");

    res = cmTargeter.targetInsert(
        opCtx, fromjson("{a: {b: 0}, c: {d: 4}}"), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "3");

    res = cmTargeter.targetInsert(opCtx,
                                  fromjson("{a: {b: 1000}, c: null, d: {}}"),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "4");

    // Missing field will be treated as null and will be targeted to the chunk which holds null,
    // which is shard '1'.
    res = cmTargeter.targetInsert(opCtx, BSONObj(), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "1");

    res =
        cmTargeter.targetInsert(opCtx, BSON("a" << 10), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "1");

    // Arrays along shard key path are not allowed.
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(opCtx,
                                               fromjson("{a: [1,2]}"),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(opCtx,
                                               fromjson("{c: [1,2]}"),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(opCtx,
                                               fromjson("{c: {d: [1,2]}}"),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
}

TEST_F(ChunkManagerTargeterTest, TargetInsertWithRangePrefixHashedShardKey) {
    testTargetInsertWithRangePrefixHashedShardKey();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest, TargetInsertWithRangePrefixHashedShardKey) {
    testTargetInsertWithRangePrefixHashedShardKey();
}

void ChunkManagerTargeterTest::testTargetInsertWithRangePrefixHashedShardKey() {
    std::set<ChunkRange> chunkRanges;
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto cmTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                         << "hashed"),
                              splitPoints);
    testTargetInsertWithRangePrefixHashedShardKeyCommon(operationContext(), cmTargeter);
}

/**
 * Build and return a custom ChunkManager with the given shard pattern and split points.
 * This is similar to CatalogCacheTestFixture::makeChunkManager() which prepare() calls
 * with the distinction that it simply creates and returns a ChunkManager object
 * and does not assign it to the Global Catalog Cache ChunkManager.
 */
ChunkManager makeCustomChunkManager(const ShardKeyPattern& shardKeyPattern,
                                    const std::vector<BSONObj>& splitPoints) {
    std::vector<ChunkType> chunks;
    auto splitPointsIncludingEnds(splitPoints);
    splitPointsIncludingEnds.insert(splitPointsIncludingEnds.begin(),
                                    shardKeyPattern.getKeyPattern().globalMin());
    splitPointsIncludingEnds.push_back(shardKeyPattern.getKeyPattern().globalMax());
    const Timestamp timestamp{Timestamp(0, 42)};
    ChunkVersion version({OID::gen(), timestamp}, {1, 0});
    const auto uuid = UUID::gen();
    for (size_t i = 1; i < splitPointsIncludingEnds.size(); ++i) {
        ChunkType chunk(
            uuid,
            {shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i - 1],
                                                              false),
             shardKeyPattern.getKeyPattern().extendRangeBound(splitPointsIncludingEnds[i], false)},
            version,
            ShardId{str::stream() << (i - 1)});
        chunk.setName(OID::gen());

        chunks.push_back(chunk);
        version.incMajor();
    }

    auto routingTableHistory = RoutingTableHistory::makeNew(kNss,
                                                            uuid,
                                                            shardKeyPattern.getKeyPattern(),
                                                            {},     // collator
                                                            false,  // unique
                                                            OID::gen(),
                                                            timestamp,
                                                            boost::none,  // time series fields
                                                            boost::none,  // resharding fields
                                                            boost::none,  // chunk size bytes
                                                            true,         // allowMigration
                                                            chunks);

    return ChunkManager(ShardId("dummyShardPrimary"),
                        DatabaseVersion(UUID::gen(), timestamp),
                        RoutingTableHistoryValueHandle(
                            std::make_shared<RoutingTableHistory>(std::move(routingTableHistory))),
                        boost::none);
}


TEST_F(ChunkManagerTargeterTest, TargetInsertWithRangePrefixHashedShardKeyCustomChunkManager) {
    testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest,
       TargetInsertWithRangePrefixHashedShardKeyCustomChunkManager) {
    testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager();
}

void ChunkManagerTargeterTest::testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager() {
    std::set<ChunkRange> chunkRanges;
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto shardKeyPattern = ShardKeyPattern(BSON("a.b" << 1 << "c.d"
                                                      << "hashed"));

    auto cm = makeCustomChunkManager(shardKeyPattern, splitPoints);
    auto cmTargeter = ChunkManagerTargeter(cm);
    ASSERT_EQ(cmTargeter.getRoutingInfo().numChunks(), 5);

    // Cause the global chunk manager to have some other configuration.
    std::vector<BSONObj> differentPoints = {BSON("c" << BSONNULL), BSON("c" << 0)};
    auto cm2 = makeChunkManager(kNss,
                                ShardKeyPattern(BSON("c" << 1 << "d"
                                                         << "hashed")),
                                nullptr,
                                false,
                                differentPoints);
    ASSERT_EQ(cm2.numChunks(), 3);

    // Run common test on the custom ChunkManager of ChunkManagerTargeter.
    testTargetInsertWithRangePrefixHashedShardKeyCommon(operationContext(), cmTargeter);
}

TEST_F(ChunkManagerTargeterTest, TargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix) {
    testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest,
       TargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix) {
    testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix();
}

void ChunkManagerTargeterTest::testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix() {
    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto cmTargeter = prepare(BSON("a.b"
                                   << "hashed"
                                   << "c.d" << 1),
                              splitPoints);

    for (int i = 0; i < 1000; i++) {
        std::set<ChunkRange> chunkRanges;
        auto insertObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));
        auto res = cmTargeter.targetInsert(
            operationContext(), insertObj, checkChunkRanges ? &chunkRanges : nullptr);

        // Verify that the given document is being routed based on hashed value of 'i'.
        auto hashValue =
            BSONElementHasher::hash64(insertObj["a"]["b"], BSONElementHasher::DEFAULT_HASH_SEED);
        auto chunk =
            chunkManager->findIntersectingChunkWithSimpleCollation(BSON("a.b" << hashValue));
        ASSERT_EQUALS(res.shardName, chunk.getShardId());
        if (checkChunkRanges) {
            // Verify that the chunk range returned is correct and contains the hashValue.
            ASSERT_EQUALS(chunkRanges.size(), 1);
            ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), chunk.getMin());
            ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), chunk.getMax());
            ASSERT_BSONOBJ_LTE(chunk.getMin(), BSON("a.b" << hashValue));
            ASSERT_BSONOBJ_LT(BSON("a.b" << hashValue), chunk.getMax());
        }
    }

    // Arrays along shard key path are not allowed.
    std::set<ChunkRange> chunkRanges;
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(operationContext(),
                                               fromjson("{a: [1,2]}"),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
}  // namespace

TEST_F(ChunkManagerTargeterTest, TargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix) {
    testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest,
       TargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix) {
    testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix();
}

void ChunkManagerTargeterTest::testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix() {
    std::set<ChunkRange> chunkRanges;
    // For the purpose of this test, we will keep the hashed field constant to 0 so that we can
    // correctly test the targeting based on range field.
    auto hashedValueOfZero = BSONElementHasher::hash64(BSON("" << 0).firstElement(),
                                                       BSONElementHasher::DEFAULT_HASH_SEED);
    // Create 5 chunks and 5 shards such that shardId
    // '0' has chunk [{'a.b': hash(0), 'c.d': MinKey}, {'a.b': hash(0), 'c.d': null}),
    // '1' has chunk [{'a.b': hash(0), 'c.d': null},   {'a.b': hash(0), 'c.d': -100}),
    // '2' has chunk [{'a.b': hash(0), 'c.d': -100},   {'a.b': hash(0), 'c.d':  0}),
    // '3' has chunk [{'a.b': hash(0), 'c.d':0},       {'a.b': hash(0), 'c.d': 100}) and
    // '4' has chunk [{'a.b': hash(0), 'c.d': 100},    {'a.b': hash(0), 'c.d': MaxKey}).
    std::vector<BSONObj> splitPoints = {BSON("a.b" << hashedValueOfZero << "c.d" << BSONNULL),
                                        BSON("a.b" << hashedValueOfZero << "c.d" << -100),
                                        BSON("a.b" << hashedValueOfZero << "c.d" << 0),
                                        BSON("a.b" << hashedValueOfZero << "c.d" << 100)};
    auto cmTargeter = prepare(BSON("a.b"
                                   << "hashed"
                                   << "c.d" << 1),
                              splitPoints);

    auto res = cmTargeter.targetInsert(operationContext(),
                                       fromjson("{a: {b: 0}, c: {d: -111}}"),
                                       checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "1");
    if (this->checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << BSONNULL));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100));
        chunkRanges.clear();
    }

    res = cmTargeter.targetInsert(operationContext(),
                                  fromjson("{a: {b: 0}, c: {d: -11}}"),
                                  this->checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "2");
    if (this->checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 0));
        chunkRanges.clear();
    }

    res = cmTargeter.targetInsert(operationContext(),
                                  fromjson("{a: {b: 0}, c: {d: 0}}"),
                                  this->checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "3");
    if (this->checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 0));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 100));
        chunkRanges.clear();
    }

    res = cmTargeter.targetInsert(operationContext(),
                                  fromjson("{a: {b: 0}, c: {d: 111}}"),
                                  this->checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "4");
    if (this->checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 100));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << MAXKEY << "c.d" << MAXKEY));
        chunkRanges.clear();
    }

    // Missing field will be treated as null and will be targeted to the chunk which holds null,
    // which is shard '1'.
    res = cmTargeter.targetInsert(
        operationContext(), fromjson("{a: {b: 0}}"), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "1");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << BSONNULL));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100));
        chunkRanges.clear();
    }

    res = cmTargeter.targetInsert(operationContext(),
                                  fromjson("{a: {b: 0}}, c: 5}"),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.shardName, "1");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << BSONNULL));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100));
        chunkRanges.clear();
    }
}

TEST_F(ChunkManagerTargeterTest, TargetUpdateWithRangePrefixHashedShardKey) {
    testTargetUpdateWithRangePrefixHashedShardKey();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest, TargetUpdateWithRangePrefixHashedShardKey) {
    testTargetUpdateWithRangePrefixHashedShardKey();
}

void ChunkManagerTargeterTest::testTargetUpdateWithRangePrefixHashedShardKey() {
    std::set<ChunkRange> chunkRanges;
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto cmTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                         << "hashed"),
                              splitPoints);

    // When update targets using replacement object.
    auto request = buildUpdate(
        kNss, fromjson("{'a.b': {$gt : 2}}"), fromjson("{a: {b: -1}}"), /*upsert=*/false);
    auto res = cmTargeter.targetUpdate(
        operationContext(), BatchItemRef(&request, 0), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "2");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), BSON("a.b" << -100 << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << 0 << "c.d" << MINKEY));
        chunkRanges.clear();
    }

    // When update targets using query.
    auto requestAndSet = buildUpdate(kNss,
                                     fromjson("{$and: [{'a.b': {$gte : 0}}, {'a.b': {$lt: 99}}]}}"),
                                     fromjson("{$set: {p : 1}}"),
                                     false);
    res = cmTargeter.targetUpdate(operationContext(),
                                  BatchItemRef(&requestAndSet, 0),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "3");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), BSON("a.b" << 0 << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << 100 << "c.d" << MINKEY));
        chunkRanges.clear();
    }

    auto requestLT =
        buildUpdate(kNss, fromjson("{'a.b': {$lt : -101}}"), fromjson("{a: {b: 111}}"), false);
    res = cmTargeter.targetUpdate(
        operationContext(), BatchItemRef(&requestLT, 0), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << BSONNULL << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << -100 << "c.d" << MINKEY));
        chunkRanges.clear();
    }

    // For op-style updates, query on _id gets targeted to all shards.
    auto requestOpUpdate =
        buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{$set: {p: 111}}"), false);
    res = cmTargeter.targetUpdate(operationContext(),
                                  BatchItemRef(&requestOpUpdate, 0),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 5);
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 5);
        auto itRange = chunkRanges.cbegin();
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << MINKEY << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << BSONNULL << "c.d" << MINKEY));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << BSONNULL << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << -100 << "c.d" << MINKEY));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << -100 << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << 0 << "c.d" << MINKEY));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << 0 << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << 100 << "c.d" << MINKEY));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << 100 << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << MAXKEY << "c.d" << MAXKEY));
        chunkRanges.clear();
    }

    // For replacement style updates, query on _id uses replacement doc to target. If the
    // replacement doc doesn't have shard key fields, then update should be routed to the shard
    // holding 'null' shard key documents.
    auto requestReplUpdate = buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{p: 111}}"), false);
    res = cmTargeter.targetUpdate(operationContext(),
                                  BatchItemRef(&requestReplUpdate, 0),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << BSONNULL << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << -100 << "c.d" << MINKEY));
        chunkRanges.clear();
    }

    // Upsert requires full shard key in query, even if the query can target a single shard.
    auto requestFullKey = buildUpdate(kNss,
                                      fromjson("{'a.b':  100, 'c.d' : {$exists: false}}}"),
                                      fromjson("{a: {b: -111}}"),
                                      true);
    ASSERT_THROWS_CODE(cmTargeter.targetUpdate(operationContext(),
                                               BatchItemRef(&requestFullKey, 0),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);

    // Upsert success case.
    auto requestSuccess =
        buildUpdate(kNss, fromjson("{'a.b': 100, 'c.d': 'val'}"), fromjson("{a: {b: -111}}"), true);
    res = cmTargeter.targetUpdate(operationContext(),
                                  BatchItemRef(&requestSuccess, 0),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "4");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), BSON("a.b" << 100 << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << MAXKEY << "c.d" << MAXKEY));
        chunkRanges.clear();
    }
}

TEST_F(ChunkManagerTargeterTest, TargetUpdateWithHashedPrefixHashedShardKey) {
    testTargetUpdateWithHashedPrefixHashedShardKey();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest, TargetUpdateWithHashedPrefixHashedShardKey) {
    testTargetUpdateWithHashedPrefixHashedShardKey();
}

void ChunkManagerTargeterTest::testTargetUpdateWithHashedPrefixHashedShardKey() {
    auto findChunk = [&](BSONElement elem) {
        return chunkManager->findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has
    // chunk [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto cmTargeter = prepare(BSON("a.b"
                                   << "hashed"
                                   << "c.d" << 1),
                              splitPoints);

    for (int i = 0; i < 1000; i++) {
        std::set<ChunkRange> chunkRanges;
        auto updateQueryObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));

        // Verify that the given document is being routed based on hashed value of 'i' in
        // 'updateQueryObj'.
        auto request = buildUpdate(kNss, updateQueryObj, fromjson("{$set: {p: 1}}"), false);
        const auto res = cmTargeter.targetUpdate(operationContext(),
                                                 BatchItemRef(&request, 0),
                                                 checkChunkRanges ? &chunkRanges : nullptr);
        ASSERT_EQUALS(res.size(), 1);
        auto chunk = findChunk(updateQueryObj["a"]["b"]);
        ASSERT_EQUALS(res[0].shardName, chunk.getShardId());
        if (checkChunkRanges) {
            ASSERT_EQUALS(chunkRanges.size(), 1);
            ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), chunk.getMin());
            ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), chunk.getMax());
        }
    }

    // Range queries on hashed field cannot be used for targeting. In this case, update will be
    // targeted based on update document.
    std::set<ChunkRange> chunkRanges;
    const auto updateObj = fromjson("{a: {b: -1}}");
    auto requestUpdate = buildUpdate(kNss, fromjson("{'a.b': {$gt : 101}}"), updateObj, false);
    auto res = cmTargeter.targetUpdate(operationContext(),
                                       BatchItemRef(&requestUpdate, 0),
                                       checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    auto chunk = findChunk(updateObj["a"]["b"]);
    ASSERT_EQUALS(res[0].shardName, chunk.getShardId());
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), chunk.getMin());
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), chunk.getMax());
        chunkRanges.clear();
    }
    auto requestErr =
        buildUpdate(kNss, fromjson("{'a.b': {$gt : 101}}"), fromjson("{$set: {p: 1}}"), false);
    ASSERT_THROWS_CODE(cmTargeter.targetUpdate(operationContext(),
                                               BatchItemRef(&requestErr, 0),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

TEST_F(ChunkManagerTargeterTest, TargetDeleteWithExactId) {
    testTargetDeleteWithExactId();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest, TargetDeleteWithExactId) {
    testTargetDeleteWithExactId();
}

void ChunkManagerTargeterTest::testTargetDeleteWithExactId() {
    std::set<ChunkRange> chunkRanges;
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto cmTargeter = prepare(BSON("a.b" << 1), splitPoints);

    auto requestId = buildDelete(kNss, fromjson("{_id: 68755000}"));
    auto res = cmTargeter.targetDelete(
        operationContext(), BatchItemRef(&requestId, 0), checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res[0].shardName, "0");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 5);
        auto itRange = chunkRanges.cbegin();
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << MINKEY));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << BSONNULL));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << BSONNULL));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << -100));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << -100));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << 0));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << 0));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << 100));
        ++itRange;
        ASSERT_BSONOBJ_EQ(itRange->getMin(), BSON("a.b" << 100));
        ASSERT_BSONOBJ_EQ(itRange->getMax(), BSON("a.b" << MAXKEY));
    }
}

TEST_F(ChunkManagerTargeterTest, TargetDeleteWithRangePrefixHashedShardKey) {
    testTargetDeleteWithRangePrefixHashedShardKey();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest, TargetDeleteWithRangePrefixHashedShardKey) {
    testTargetDeleteWithRangePrefixHashedShardKey();
}

void ChunkManagerTargeterTest::testTargetDeleteWithRangePrefixHashedShardKey() {
    std::set<ChunkRange> chunkRanges;
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has
    // chunk [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto cmTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                         << "hashed"),
                              splitPoints);

    // Cannot delete without full shardkey in the query.
    auto requestPartialKey = buildDelete(kNss, fromjson("{'a.b': {$gt : 2}}"));
    ASSERT_THROWS_CODE(cmTargeter.targetDelete(operationContext(),
                                               BatchItemRef(&requestPartialKey, 0),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);

    auto requestPartialKey2 = buildDelete(kNss, fromjson("{'a.b': -101}"));
    ASSERT_THROWS_CODE(cmTargeter.targetDelete(operationContext(),
                                               BatchItemRef(&requestPartialKey2, 0),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);

    // Delete targeted correctly with full shard key in query.
    auto requestFullKey = buildDelete(kNss, fromjson("{'a.b': -101, 'c.d': 5}"));
    auto res = cmTargeter.targetDelete(operationContext(),
                                       BatchItemRef(&requestFullKey, 0),
                                       checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(),
                          BSON("a.b" << BSONNULL << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << -100 << "c.d" << MINKEY));
        chunkRanges.clear();
    }

    // Query with MinKey value should go to chunk '0' because MinKey is smaller than BSONNULL.
    auto requestMinKey =
        buildDelete(kNss, BSONObjBuilder().appendMinKey("a.b").append("c.d", 4).obj());
    res = cmTargeter.targetDelete(operationContext(),
                                  BatchItemRef(&requestMinKey, 0),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "0");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), BSON("a.b" << MINKEY << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(),
                          BSON("a.b" << BSONNULL << "c.d" << MINKEY));
        chunkRanges.clear();
    }

    auto requestMinKey2 = buildDelete(kNss, fromjson("{'a.b':  0, 'c.d': 5}"));
    res = cmTargeter.targetDelete(operationContext(),
                                  BatchItemRef(&requestMinKey2, 0),
                                  checkChunkRanges ? &chunkRanges : nullptr);
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "3");
    if (checkChunkRanges) {
        ASSERT_EQUALS(chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), BSON("a.b" << 0 << "c.d" << MINKEY));
        ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), BSON("a.b" << 100 << "c.d" << MINKEY));
        chunkRanges.clear();
    }
}

TEST_F(ChunkManagerTargeterTest, TargetDeleteWithHashedPrefixHashedShardKey) {
    testTargetDeleteWithHashedPrefixHashedShardKey();
}

TEST_F(ChunkManagerTargeterWithChunkRangesTest, TargetDeleteWithHashedPrefixHashedShardKey) {
    testTargetDeleteWithHashedPrefixHashedShardKey();
}

void ChunkManagerTargeterTest::testTargetDeleteWithHashedPrefixHashedShardKey() {
    std::set<ChunkRange> chunkRanges;
    auto findChunk = [&](BSONElement elem) {
        return chunkManager->findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has
    // chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto cmTargeter = prepare(BSON("a.b"
                                   << "hashed"
                                   << "c.d" << 1),
                              splitPoints);

    for (int i = 0; i < 1000; i++) {
        auto queryObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));
        // Verify that the given document is being routed based on hashed value of 'i' in
        // 'queryObj'.
        auto request = buildDelete(kNss, queryObj);
        const auto res = cmTargeter.targetDelete(operationContext(),
                                                 BatchItemRef(&request, 0),
                                                 checkChunkRanges ? &chunkRanges : nullptr);
        ASSERT_EQUALS(res.size(), 1);
        auto chunk = findChunk(queryObj["a"]["b"]);
        ASSERT_EQUALS(res[0].shardName, chunk.getShardId());
        if (checkChunkRanges) {
            ASSERT_EQUALS(chunkRanges.size(), 1);
            ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMin(), chunk.getMin());
            ASSERT_BSONOBJ_EQ(chunkRanges.cbegin()->getMax(), chunk.getMax());
            chunkRanges.clear();
        }
    }

    // Range queries on hashed field cannot be used for targeting.
    auto request = buildDelete(kNss, fromjson("{'a.b': {$gt : 101}}"));
    ASSERT_THROWS_CODE(cmTargeter.targetDelete(operationContext(),
                                               BatchItemRef(&request, 0),
                                               checkChunkRanges ? &chunkRanges : nullptr),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
}

TEST(ChunkManagerTargeterTest, ExtractBucketsShardKeyFromTimeseriesDocument) {
    const StringData timeField = "tm";
    const StringData metaField = "mm";

    TimeseriesOptions options{std::string(timeField)};
    options.setMetaField(metaField);
    auto dateStatus = dateFromISOString("2021-01-01T00:00:15.555Z");
    ASSERT_OK(dateStatus);
    auto date = dateStatus.getValue();
    auto roundedDate = timeseries::roundTimestampToGranularity(date, options);

    auto checkShardKey = [&](const BSONObj& tsShardKeyPattern,
                             const BSONObj& metaValue = BSONObj()) {
        auto inputDoc = [&]() {
            BSONObjBuilder builder;
            builder << timeField << date;
            if (!metaValue.isEmpty()) {
                builder << metaField << metaValue;
            }
            return builder.obj();
        }();

        auto inputBucket = [&]() {
            BSONObjBuilder builder;
            builder << timeseries::kBucketControlFieldName
                    << BSON(timeseries::kBucketControlMinFieldName
                            << BSON(timeField << roundedDate));
            if (!metaValue.isEmpty()) {
                builder << timeseries::kBucketMetaFieldName << metaValue;
            }
            return builder.obj();
        }();

        auto bucketsShardKeyPattern =
            timeseries::createBucketsShardKeySpecFromTimeseriesShardKeySpec(options,
                                                                            tsShardKeyPattern);
        ASSERT_OK(bucketsShardKeyPattern);
        ShardKeyPattern pattern{bucketsShardKeyPattern.getValue()};

        auto expectedShardKey = pattern.extractShardKeyFromDoc(inputBucket);
        auto actualShardKey = ChunkManagerTargeter::extractBucketsShardKeyFromTimeseriesDoc(
            inputDoc, pattern, options);

        ASSERT_BSONOBJ_EQ(expectedShardKey, actualShardKey);
    };

    checkShardKey(BSON(timeField << 1));
    checkShardKey(BSON(timeField << 1 << metaField << 1));
    checkShardKey(BSON(timeField << 1 << (str::stream() << metaField << ".nested.value") << 1));

    checkShardKey(BSON(timeField << 1), BSON("nested" << 123));
    checkShardKey(BSON(timeField << 1 << metaField << 1), BSON("nested" << 123));
    checkShardKey(BSON(timeField << 1 << (str::stream() << metaField << ".nested.value") << 1),
                  BSON("nested" << BSON("value" << 123)));
}

}  // namespace
}  // namespace mongo
