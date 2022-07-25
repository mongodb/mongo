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
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/unittest.h"

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
    }
    boost::optional<ChunkManager> chunkManager;
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
    auto request =
        buildUpdate(kNss, fromjson("{'a.b': {$gt : 2}}"), fromjson("{a: {b: -1}}"), false);
    auto res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&request, 0));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "2");

    // When update targets using query.
    auto requestAndSet = buildUpdate(kNss,
                                     fromjson("{$and: [{'a.b': {$gte : 0}}, {'a.b': {$lt: 99}}]}}"),
                                     fromjson("{$set: {p : 1}}"),
                                     false);
    res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestAndSet, 0));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "3");

    auto requestLT =
        buildUpdate(kNss, fromjson("{'a.b': {$lt : -101}}"), fromjson("{a: {b: 111}}"), false);
    res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestLT, 0));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");

    // For op-style updates, query on _id gets targeted to all shards.
    auto requestOpUpdate =
        buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{$set: {p: 111}}"), false);
    res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestOpUpdate, 0));
    ASSERT_EQUALS(res.size(), 5);

    // For replacement style updates, query on _id uses replacement doc to target. If the
    // replacement doc doesn't have shard key fields, then update should be routed to the shard
    // holding 'null' shard key documents.
    auto requestReplUpdate = buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{p: 111}}"), false);
    res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestReplUpdate, 0));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");


    // Upsert requires full shard key in query, even if the query can target a single shard.
    auto requestFullKey = buildUpdate(kNss,
                                      fromjson("{'a.b':  100, 'c.d' : {$exists: false}}}"),
                                      fromjson("{a: {b: -111}}"),
                                      true);
    ASSERT_THROWS_CODE(
        cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestFullKey, 0)),
        DBException,
        ErrorCodes::ShardKeyNotFound);

    // Upsert success case.
    auto requestSuccess =
        buildUpdate(kNss, fromjson("{'a.b': 100, 'c.d': 'val'}"), fromjson("{a: {b: -111}}"), true);
    res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestSuccess, 0));
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
        auto request = buildUpdate(kNss, updateQueryObj, fromjson("{$set: {p: 1}}"), false);
        const auto res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&request, 0));
        ASSERT_EQUALS(res.size(), 1);
        ASSERT_EQUALS(res[0].shardName, findChunk(updateQueryObj["a"]["b"]).getShardId());
    }

    // Range queries on hashed field cannot be used for targeting. In this case, update will be
    // targeted based on update document.
    const auto updateObj = fromjson("{a: {b: -1}}");
    auto requestUpdate = buildUpdate(kNss, fromjson("{'a.b': {$gt : 101}}"), updateObj, false);
    auto res = cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestUpdate, 0));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, findChunk(updateObj["a"]["b"]).getShardId());
    auto requestErr =
        buildUpdate(kNss, fromjson("{'a.b': {$gt : 101}}"), fromjson("{$set: {p: 1}}"), false);
    ASSERT_THROWS_CODE(cmTargeter.targetUpdate(operationContext(), BatchItemRef(&requestErr, 0)),
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
    auto requestPartialKey = buildDelete(kNss, fromjson("{'a.b': {$gt : 2}}"));
    ASSERT_THROWS_CODE(
        cmTargeter.targetDelete(operationContext(), BatchItemRef(&requestPartialKey, 0)),
        DBException,
        ErrorCodes::ShardKeyNotFound);

    auto requestPartialKey2 = buildDelete(kNss, fromjson("{'a.b': -101}"));
    ASSERT_THROWS_CODE(
        cmTargeter.targetDelete(operationContext(), BatchItemRef(&requestPartialKey2, 0)),
        DBException,
        ErrorCodes::ShardKeyNotFound);

    // Delete targeted correctly with full shard key in query.
    auto requestFullKey = buildDelete(kNss, fromjson("{'a.b': -101, 'c.d': 5}"));
    auto res = cmTargeter.targetDelete(operationContext(), BatchItemRef(&requestFullKey, 0));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");

    // Query with MinKey value should go to chunk '0' because MinKey is smaller than BSONNULL.
    auto requestMinKey =
        buildDelete(kNss, BSONObjBuilder().appendMinKey("a.b").append("c.d", 4).obj());
    res = cmTargeter.targetDelete(operationContext(), BatchItemRef(&requestMinKey, 0));
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "0");

    auto requestMinKey2 = buildDelete(kNss, fromjson("{'a.b':  0, 'c.d': 5}"));
    res = cmTargeter.targetDelete(operationContext(), BatchItemRef(&requestMinKey2, 0));
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
        auto request = buildDelete(kNss, queryObj);
        const auto res = cmTargeter.targetDelete(operationContext(), BatchItemRef(&request, 0));
        ASSERT_EQUALS(res.size(), 1);
        ASSERT_EQUALS(res[0].shardName, findChunk(queryObj["a"]["b"]).getShardId());
    }

    // Range queries on hashed field cannot be used for targeting.
    auto request = buildDelete(kNss, fromjson("{'a.b': {$gt : 101}}"));
    ASSERT_THROWS_CODE(cmTargeter.targetDelete(operationContext(), BatchItemRef(&request, 0)),
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
