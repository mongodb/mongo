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
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/chunk_manager_targeter.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");

write_ops::UpdateOpEntry buildUpdate(BSONObj query, BSONObj update, bool upsert) {
    write_ops::UpdateOpEntry entry;
    entry.setQ(query);
    entry.setU(update);
    entry.setUpsert(upsert);
    return entry;
}

write_ops::DeleteOpEntry buildDelete(BSONObj query) {
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(false);
    return entry;
}

class ChunkManagerTargeterTest : public CatalogCacheTestFixture {
public:
    ChunkManagerTargeter prepare(BSONObj shardKeyPattern, const std::vector<BSONObj>& splitPoints) {
        chunkManager =
            makeChunkManager(kNss, ShardKeyPattern(shardKeyPattern), nullptr, false, splitPoints);
        return ChunkManagerTargeter(operationContext(), kNss);
    }
    std::shared_ptr<ChunkManager> chunkManager;
};

TEST_F(ChunkManagerTargeterTest, TargetInsertWithRangePrefixHashedShardKey) {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto cmTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                         << "hashed"),
                              splitPoints);

    auto res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: -111}, c: {d: '1'}}"));
    ASSERT_EQUALS(res.shardName, "1");

    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: -10}}"));
    ASSERT_EQUALS(res.shardName, "2");

    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: 4}}"));
    ASSERT_EQUALS(res.shardName, "3");

    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 1000}, c: null, d: {}}"));
    ASSERT_EQUALS(res.shardName, "4");

    // Missing field will be treated as null and will be targeted to the chunk which holds null,
    // which is shard '1'.
    res = cmTargeter.targetInsert(operationContext(), BSONObj());
    ASSERT_EQUALS(res.shardName, "1");

    res = cmTargeter.targetInsert(operationContext(), BSON("a" << 10));
    ASSERT_EQUALS(res.shardName, "1");

    // Arrays along shard key path are not allowed.
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(operationContext(), fromjson("{a: [1,2]}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(operationContext(), fromjson("{c: [1,2]}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(operationContext(), fromjson("{c: {d: [1,2]}}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
}

TEST_F(ChunkManagerTargeterTest, TargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix) {
    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto cmTargeter = prepare(BSON("a.b"
                                   << "hashed"
                                   << "c.d" << 1),
                              splitPoints);

    for (int i = 0; i < 1000; i++) {
        auto insertObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));
        auto res = cmTargeter.targetInsert(operationContext(), insertObj);

        // Verify that the given document is being routed based on hashed value of 'i'.
        auto chunk = chunkManager->findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(insertObj["a"]["b"],
                                                    BSONElementHasher::DEFAULT_HASH_SEED)));
        ASSERT_EQUALS(res.shardName, chunk.getShardId());
    }

    // Arrays along shard key path are not allowed.
    ASSERT_THROWS_CODE(cmTargeter.targetInsert(operationContext(), fromjson("{a: [1,2]}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
}

TEST_F(ChunkManagerTargeterTest, TargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix) {
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

    auto res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: -111}}"));
    ASSERT_EQUALS(res.shardName, "1");

    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: -11}}"));
    ASSERT_EQUALS(res.shardName, "2");

    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: 0}}"));
    ASSERT_EQUALS(res.shardName, "3");

    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: 111}}"));
    ASSERT_EQUALS(res.shardName, "4");

    // Missing field will be treated as null and will be targeted to the chunk which holds null,
    // which is shard '1'.
    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}}"));
    ASSERT_EQUALS(res.shardName, "1");

    res = cmTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}}, c: 5}"));
    ASSERT_EQUALS(res.shardName, "1");
}

TEST_F(ChunkManagerTargeterTest, TargetUpdateWithRangePrefixHashedShardKey) {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto cmTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                         << "hashed"),
                              splitPoints);

    // When update targets using replacement object.
    auto res = cmTargeter.targetUpdate(
        operationContext(),
        buildUpdate(fromjson("{'a.b': {$gt : 2}}"), fromjson("{a: {b: -1}}"), false));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "2");

    // When update targets using query.
    res = cmTargeter.targetUpdate(
        operationContext(),
        buildUpdate(fromjson("{$and: [{'a.b': {$gte : 0}}, {'a.b': {$lt: 99}}]}}"),
                    fromjson("{$set: {p : 1}}"),
                    false));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "3");

    res = cmTargeter.targetUpdate(
        operationContext(),
        buildUpdate(fromjson("{'a.b': {$lt : -101}}"), fromjson("{a: {b: 111}}"), false));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");

    // For op-style updates, query on _id gets targeted to all shards.
    res = cmTargeter.targetUpdate(
        operationContext(), buildUpdate(fromjson("{_id: 1}"), fromjson("{$set: {p: 111}}"), false));
    ASSERT_EQUALS(res.size(), 5);

    // For replacement style updates, query on _id uses replacement doc to target. If the
    // replacement doc doesn't have shard key fields, then update should be routed to the shard
    // holding 'null' shard key documents.
    res = cmTargeter.targetUpdate(operationContext(),
                                  buildUpdate(fromjson("{_id: 1}"), fromjson("{p: 111}}"), false));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");


    // Upsert requires full shard key in query, even if the query can target a single shard.
    ASSERT_THROWS_CODE(
        cmTargeter.targetUpdate(operationContext(),
                                buildUpdate(fromjson("{'a.b':  100, 'c.d' : {$exists: false}}}"),
                                            fromjson("{a: {b: -111}}"),
                                            true)),
        DBException,
        ErrorCodes::ShardKeyNotFound);

    // Upsert success case.
    res = cmTargeter.targetUpdate(
        operationContext(),
        buildUpdate(fromjson("{'a.b': 100, 'c.d': 'val'}"), fromjson("{a: {b: -111}}"), true));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "4");
}

TEST_F(ChunkManagerTargeterTest, TargetUpdateWithHashedPrefixHashedShardKey) {
    auto findChunk = [&](BSONElement elem) {
        return chunkManager->findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto cmTargeter = prepare(BSON("a.b"
                                   << "hashed"
                                   << "c.d" << 1),
                              splitPoints);

    for (int i = 0; i < 1000; i++) {
        auto updateQueryObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));

        // Verify that the given document is being routed based on hashed value of 'i' in
        // 'updateQueryObj'.
        const auto res = cmTargeter.targetUpdate(
            operationContext(), buildUpdate(updateQueryObj, fromjson("{$set: {p: 1}}"), false));
        ASSERT_EQUALS(res.size(), 1);
        ASSERT_EQUALS(res[0].shardName, findChunk(updateQueryObj["a"]["b"]).getShardId());
    }

    // Range queries on hashed field cannot be used for targeting. In this case, update will be
    // targeted based on update document.
    const auto updateObj = fromjson("{a: {b: -1}}");
    auto res = cmTargeter.targetUpdate(
        operationContext(), buildUpdate(fromjson("{'a.b': {$gt : 101}}"), updateObj, false));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, findChunk(updateObj["a"]["b"]).getShardId());
    ASSERT_THROWS_CODE(
        cmTargeter.targetUpdate(
            operationContext(),
            buildUpdate(fromjson("{'a.b': {$gt : 101}}"), fromjson("{$set: {p: 1}}"), false)),
        DBException,
        ErrorCodes::InvalidOptions);
}

TEST_F(ChunkManagerTargeterTest, TargetDeleteWithRangePrefixHashedShardKey) {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto cmTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                         << "hashed"),
                              splitPoints);

    // Cannot delete without full shardkey in the query.
    ASSERT_THROWS_CODE(
        cmTargeter.targetDelete(operationContext(), buildDelete(fromjson("{'a.b': {$gt : 2}}"))),
        DBException,
        ErrorCodes::ShardKeyNotFound);
    ASSERT_THROWS_CODE(
        cmTargeter.targetDelete(operationContext(), buildDelete(fromjson("{'a.b':  -101}"))),
        DBException,
        ErrorCodes::ShardKeyNotFound);

    // Delete targeted correctly with full shard key in query.
    auto res = cmTargeter.targetDelete(operationContext(),
                                       buildDelete(fromjson("{'a.b':  -101, 'c.d': 5}")));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");

    // Query with MinKey value should go to chunk '0' because MinKey is smaller than BSONNULL.
    res = cmTargeter.targetDelete(
        operationContext(),
        buildDelete(BSONObjBuilder().appendMinKey("a.b").append("c.d", 4).obj()));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "0");

    res =
        cmTargeter.targetDelete(operationContext(), buildDelete(fromjson("{'a.b':  0, 'c.d': 5}")));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "3");
}

TEST_F(ChunkManagerTargeterTest, TargetDeleteWithHashedPrefixHashedShardKey) {
    auto findChunk = [&](BSONElement elem) {
        return chunkManager->findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has chunk
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
        const auto res = cmTargeter.targetDelete(operationContext(), buildDelete(queryObj));
        ASSERT_EQUALS(res.size(), 1);
        ASSERT_EQUALS(res[0].shardName, findChunk(queryObj["a"]["b"]).getShardId());
    }

    // Range queries on hashed field cannot be used for targeting.
    ASSERT_THROWS_CODE(
        cmTargeter.targetDelete(operationContext(), buildDelete(fromjson("{'a.b': {$gt : 101}}"))),
        DBException,
        ErrorCodes::ShardKeyNotFound);
}

}  // namespace
}  // namespace mongo
