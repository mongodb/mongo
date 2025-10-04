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

#include "mongo/db/global_catalog/router_role_api/collection_routing_info_targeter.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_mock.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/hasher.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {


const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

auto buildUpdate(
    const NamespaceString& nss, BSONObj query, BSONObj update, bool upsert, bool multi = false) {
    write_ops::UpdateCommandRequest updateOp(nss);
    write_ops::UpdateOpEntry entry;
    entry.setQ(query);
    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
    entry.setUpsert(upsert);
    entry.setMulti(multi);
    updateOp.setUpdates(std::vector{entry});
    return BatchedCommandRequest{std::move(updateOp)};
}

auto buildDelete(const NamespaceString& nss, BSONObj query, bool multi = false) {
    write_ops::DeleteCommandRequest deleteOp(nss);
    write_ops::DeleteOpEntry entry;
    entry.setQ(query);
    entry.setMulti(multi);
    deleteOp.setDeletes(std::vector{entry});
    return BatchedCommandRequest{std::move(deleteOp)};
}

/**
 * Fixture that populates the CatalogCache with 'kNss' as a sharded collection.
 */
class CollectionRoutingInfoTargeterTest : public RouterCatalogCacheTestFixture {
public:
    CollectionRoutingInfoTargeter prepare(BSONObj shardKeyPattern,
                                          const std::vector<BSONObj>& splitPoints) {
        collectionRoutingInfo.emplace(makeCollectionRoutingInfo(
            kNss, ShardKeyPattern(shardKeyPattern), nullptr, false, splitPoints, {}));
        return CollectionRoutingInfoTargeter(operationContext(), kNss);
    };
    boost::optional<CollectionRoutingInfo> collectionRoutingInfo;

protected:
    void testTargetInsertWithRangePrefixHashedShardKeyCommon(
        OperationContext* opCtx, const CollectionRoutingInfoTargeter& criTargeter);
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

/**
 * This is the common part of test TargetInsertWithRangePrefixHashedShardKey and
 * TargetInsertWithRangePrefixHashedShardKeyCustomChunkManager
 * Tests that the destination shard is the correct one as defined from the split points
 * when the ChunkManager was constructed.
 */
void CollectionRoutingInfoTargeterTest::testTargetInsertWithRangePrefixHashedShardKeyCommon(
    OperationContext* opCtx, const CollectionRoutingInfoTargeter& criTargeter) {

    // Caller has created 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1'
    // has chunk [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    auto res = criTargeter.targetInsert(opCtx, fromjson("{a: {b: -111}, c: {d: '1'}}"));
    ASSERT_EQUALS(res.shardName, "1");

    res = criTargeter.targetInsert(opCtx, fromjson("{a: {b: -10}}"));
    ASSERT_EQUALS(res.shardName, "2");

    res = criTargeter.targetInsert(opCtx, fromjson("{a: {b: 0}, c: {d: 4}}"));
    ASSERT_EQUALS(res.shardName, "3");

    res = criTargeter.targetInsert(opCtx, fromjson("{a: {b: 1000}, c: null, d: {}}"));
    ASSERT_EQUALS(res.shardName, "4");

    // Missing field will be treated as null and will be targeted to the chunk which holds null,
    // which is shard '1'.
    res = criTargeter.targetInsert(opCtx, BSONObj());
    ASSERT_EQUALS(res.shardName, "1");

    res = criTargeter.targetInsert(opCtx, BSON("a" << 10));
    ASSERT_EQUALS(res.shardName, "1");

    // Arrays along shard key path are not allowed.
    ASSERT_THROWS_CODE(criTargeter.targetInsert(opCtx, fromjson("{a: [1,2]}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
    ASSERT_THROWS_CODE(criTargeter.targetInsert(opCtx, fromjson("{c: [1,2]}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
    ASSERT_THROWS_CODE(criTargeter.targetInsert(opCtx, fromjson("{c: {d: [1,2]}}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
}

TEST_F(CollectionRoutingInfoTargeterTest, TargetInsertWithRangePrefixHashedShardKey) {
    testTargetInsertWithRangePrefixHashedShardKey();
}

void CollectionRoutingInfoTargeterTest::testTargetInsertWithRangePrefixHashedShardKey() {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto criTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                          << "hashed"),
                               splitPoints);
    testTargetInsertWithRangePrefixHashedShardKeyCommon(operationContext(), criTargeter);
}

/**
 * Build and return a custom ChunkManager with the given shard pattern and split points.
 * This is similar to CoreCatalogCacheTestFixture::makeChunkManager() which prepare() calls
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
    const OID epoch = OID::gen();
    ChunkVersion version({epoch, timestamp}, {1, 0});
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
                                                            false,  /* unsplittable */
                                                            {},     // collator
                                                            false,  // unique
                                                            epoch,
                                                            timestamp,
                                                            boost::none,  // time series fields
                                                            boost::none,  // resharding fields
                                                            true,         // allowMigration
                                                            chunks);

    return ChunkManager(RoutingTableHistoryValueHandle(
                            std::make_shared<RoutingTableHistory>(std::move(routingTableHistory))),
                        boost::none);
}


TEST_F(CollectionRoutingInfoTargeterTest,
       TargetInsertWithRangePrefixHashedShardKeyCustomChunkManager) {
    testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager();
}

void CollectionRoutingInfoTargeterTest::
    testTargetInsertWithRangePrefixHashedShardKeyCustomChunkManager() {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto shardKeyPattern = ShardKeyPattern(BSON("a.b" << 1 << "c.d"
                                                      << "hashed"));

    auto cm = makeCustomChunkManager(shardKeyPattern, splitPoints);
    auto criTargeter = CollectionRoutingInfoTargeter(
        kNss,
        CollectionRoutingInfo{
            std::move(cm),
            DatabaseTypeValueHandle(DatabaseType{kNss.dbName(),
                                                 ShardId("dummyShardPrimary"),
                                                 DatabaseVersion(UUID::gen(), Timestamp(1, 0))})});
    ASSERT_EQ(criTargeter.getRoutingInfo().getChunkManager().numChunks(), 5);

    // Cause the global chunk manager to have some other configuration.
    std::vector<BSONObj> differentPoints = {BSON("c" << BSONNULL), BSON("c" << 0)};
    auto cri2 = makeCollectionRoutingInfo(kNss,
                                          ShardKeyPattern(BSON("c" << 1 << "d"
                                                                   << "hashed")),
                                          nullptr,
                                          false,
                                          differentPoints,
                                          {});
    ASSERT_EQ(cri2.getChunkManager().numChunks(), 3);

    // Run common test on the custom ChunkManager of CollectionRoutingInfoTargeter.
    testTargetInsertWithRangePrefixHashedShardKeyCommon(operationContext(), criTargeter);
}

TEST_F(CollectionRoutingInfoTargeterTest,
       TargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix) {
    testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix();
}

void CollectionRoutingInfoTargeterTest::
    testTargetInsertsWithVaryingHashedPrefixAndConstantRangedSuffix() {
    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto criTargeter = prepare(BSON("a.b" << "hashed"
                                          << "c.d" << 1),
                               splitPoints);
    for (int i = 0; i < 1000; i++) {
        auto insertObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));
        auto res = criTargeter.targetInsert(operationContext(), insertObj);

        // Verify that the given document is being routed based on hashed value of 'i'.
        auto hashValue =
            BSONElementHasher::hash64(insertObj["a"]["b"], BSONElementHasher::DEFAULT_HASH_SEED);
        auto chunk =
            collectionRoutingInfo->getChunkManager().findIntersectingChunkWithSimpleCollation(
                BSON("a.b" << hashValue));
        ASSERT_EQUALS(res.shardName, chunk.getShardId());
    }

    // Arrays along shard key path are not allowed.
    ASSERT_THROWS_CODE(criTargeter.targetInsert(operationContext(), fromjson("{a: [1,2]}")),
                       DBException,
                       ErrorCodes::ShardKeyNotFound);
}  // namespace

TEST_F(CollectionRoutingInfoTargeterTest,
       TargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix) {
    testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix();
}

void CollectionRoutingInfoTargeterTest::
    testTargetInsertsWithConstantHashedPrefixAndVaryingRangedSuffix() {
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
    auto criTargeter = prepare(BSON("a.b" << "hashed"
                                          << "c.d" << 1),
                               splitPoints);

    auto res = criTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: -111}}"));
    ASSERT_EQUALS(res.shardName, "1");

    res = criTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: -11}}"));
    ASSERT_EQUALS(res.shardName, "2");

    res = criTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: 0}}"));
    ASSERT_EQUALS(res.shardName, "3");

    res = criTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: {d: 111}}"));
    ASSERT_EQUALS(res.shardName, "4");

    // Missing field will be treated as null and will be targeted to the chunk which holds null,
    // which is shard '1'.
    res = criTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}}"));
    ASSERT_EQUALS(res.shardName, "1");

    res = criTargeter.targetInsert(operationContext(), fromjson("{a: {b: 0}, c: 5}"));
    ASSERT_EQUALS(res.shardName, "1");
}

TEST_F(CollectionRoutingInfoTargeterTest, TargetUpdateWithRangePrefixHashedShardKey) {
    testTargetUpdateWithRangePrefixHashedShardKey();
}

void CollectionRoutingInfoTargeterTest::testTargetUpdateWithRangePrefixHashedShardKey() {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto criTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                          << "hashed"),
                               splitPoints);

    // When update targets using replacement object.
    auto request = buildUpdate(
        kNss, fromjson("{'a.b': {$gt : 2}}"), fromjson("{a: {b: -1}}"), /*upsert=*/false);
    auto res = criTargeter.targetUpdate(operationContext(), BatchItemRef(&request, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 2);
    ASSERT_EQUALS(res[0].shardName, "3");
    ASSERT_EQUALS(res[1].shardName, "4");

    // When update targets using query.
    auto requestAndSet = buildUpdate(kNss,
                                     fromjson("{$and: [{'a.b': {$gte : 0}}, {'a.b': {$lt: 99}}]}"),
                                     fromjson("{$set: {p : 1}}"),
                                     false);
    res = criTargeter.targetUpdate(operationContext(), BatchItemRef(&requestAndSet, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "3");

    auto requestLT =
        buildUpdate(kNss, fromjson("{'a.b': {$lt : -101}}"), fromjson("{a: {b: 111}}"), false);
    res = criTargeter.targetUpdate(operationContext(), BatchItemRef(&requestLT, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");

    // For op-style updates and replacement style updates, query on _id gets targeted to all shards.
    auto requestOpUpdate =
        buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{$set: {p: 111}}"), false);
    res = criTargeter.targetUpdate(operationContext(), BatchItemRef(&requestOpUpdate, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 5);

    auto requestReplUpdate = buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{p: 111}"), false);
    res =
        criTargeter.targetUpdate(operationContext(), BatchItemRef(&requestReplUpdate, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 5);

    // Upsert without full shard key.
    auto requestFullKey =
        buildUpdate(kNss, fromjson("{'a.b':  100}"), fromjson("{a: {b: -111}}"), true);
    auto resUpsert =
        criTargeter.targetUpdate(operationContext(), BatchItemRef(&requestFullKey, 0)).endpoints;
    ASSERT_EQUALS(resUpsert.size(), 1);
    ASSERT_EQUALS(resUpsert[0].shardName, "4");

    // Upsert with full shard key.
    auto requestSuccess =
        buildUpdate(kNss, fromjson("{'a.b': 100, 'c.d': 'val'}"), fromjson("{a: {b: -111}}"), true);
    res = criTargeter.targetUpdate(operationContext(), BatchItemRef(&requestSuccess, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "4");
}

TEST_F(CollectionRoutingInfoTargeterTest, TargetUpdateWithHashedPrefixHashedShardKey) {
    testTargetUpdateWithHashedPrefixHashedShardKey();
}

void CollectionRoutingInfoTargeterTest::testTargetUpdateWithHashedPrefixHashedShardKey() {
    auto findChunk = [&](BSONElement elem) {
        return collectionRoutingInfo->getChunkManager().findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has
    // chunk [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto criTargeter = prepare(BSON("a.b" << "hashed"
                                          << "c.d" << 1),
                               splitPoints);

    for (int i = 0; i < 1000; i++) {
        auto updateQueryObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));

        // Verify that the given document is being routed based on hashed value of 'i' in
        // 'updateQueryObj'.
        auto request = buildUpdate(kNss, updateQueryObj, fromjson("{$set: {p: 1}}"), false);
        const auto res =
            criTargeter.targetUpdate(operationContext(), BatchItemRef(&request, 0)).endpoints;
        ASSERT_EQUALS(res.size(), 1);
        auto chunk = findChunk(updateQueryObj["a"]["b"]);
        ASSERT_EQUALS(res[0].shardName, chunk.getShardId());
    }

    // Range queries will be able to target using the two phase write protocol.
    const auto updateObj = fromjson("{a: {b: -1}}");
    auto requestUpdate = buildUpdate(kNss, fromjson("{'a.b': {$gt : 101}}"), updateObj, false);
    auto res =
        criTargeter.targetUpdate(operationContext(), BatchItemRef(&requestUpdate, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 4);
}

TEST_F(CollectionRoutingInfoTargeterTest, TargetDeleteWithExactId) {
    testTargetDeleteWithExactId();
}

void CollectionRoutingInfoTargeterTest::testTargetDeleteWithExactId() {
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto criTargeter = prepare(BSON("a.b" << 1), splitPoints);

    auto requestId = buildDelete(kNss, fromjson("{_id: 68755000}"));
    auto res = criTargeter.targetDelete(operationContext(), BatchItemRef(&requestId, 0)).endpoints;
    ASSERT_EQUALS(res[0].shardName, "0");
}

TEST_F(CollectionRoutingInfoTargeterTest, TargetDeleteWithRangePrefixHashedShardKey) {
    testTargetDeleteWithRangePrefixHashedShardKey();
}

void CollectionRoutingInfoTargeterTest::testTargetDeleteWithRangePrefixHashedShardKey() {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has
    // chunk [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto criTargeter = prepare(BSON("a.b" << 1 << "c.d"
                                          << "hashed"),
                               splitPoints);

    // Can delete wih partial shard key in the query if the query only targets one shard.
    auto requestPartialKey = buildDelete(kNss, fromjson("{'a.b': {$gt : 101}}"));
    auto res =
        criTargeter.targetDelete(operationContext(), BatchItemRef(&requestPartialKey, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "4");

    // Test delete with range query.
    auto requestPartialKey2 = buildDelete(kNss, fromjson("{'a.b': {$gt: 0}}"));
    auto resMultipleShards =
        criTargeter.targetDelete(operationContext(), BatchItemRef(&requestPartialKey2, 0))
            .endpoints;
    ASSERT_EQUALS(resMultipleShards.size(), 2);
    ASSERT_EQUALS(resMultipleShards[0].shardName, "3");
    ASSERT_EQUALS(resMultipleShards[1].shardName, "4");

    // Test delete with no shard key.
    auto requestNoShardKey = buildDelete(kNss, fromjson("{'k': 0}"));
    auto resNoShardKey =
        criTargeter.targetDelete(operationContext(), BatchItemRef(&requestNoShardKey, 0)).endpoints;
    ASSERT_EQUALS(resNoShardKey.size(), 5);

    // Delete targeted correctly with full shard key in query.
    auto requestFullKey = buildDelete(kNss, fromjson("{'a.b': -101, 'c.d': 5}"));
    res = criTargeter.targetDelete(operationContext(), BatchItemRef(&requestFullKey, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "1");

    // Query with MinKey value should go to chunk '0' because MinKey is smaller than BSONNULL.
    auto requestMinKey =
        buildDelete(kNss, BSONObjBuilder().appendMinKey("a.b").append("c.d", 4).obj());
    res = criTargeter.targetDelete(operationContext(), BatchItemRef(&requestMinKey, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "0");

    auto requestMinKey2 = buildDelete(kNss, fromjson("{'a.b':  0, 'c.d': 5}"));
    res = criTargeter.targetDelete(operationContext(), BatchItemRef(&requestMinKey2, 0)).endpoints;
    ASSERT_EQUALS(res.size(), 1);
    ASSERT_EQUALS(res[0].shardName, "3");
}

TEST_F(CollectionRoutingInfoTargeterTest, TargetDeleteWithHashedPrefixHashedShardKey) {
    testTargetDeleteWithHashedPrefixHashedShardKey();
}

void CollectionRoutingInfoTargeterTest::testTargetDeleteWithHashedPrefixHashedShardKey() {
    auto findChunk = [&](BSONElement elem) {
        return collectionRoutingInfo->getChunkManager().findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has
    // chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto criTargeter = prepare(BSON("a.b" << "hashed"
                                          << "c.d" << 1),
                               splitPoints);

    for (int i = 0; i < 1000; i++) {
        auto queryObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));
        // Verify that the given document is being routed based on hashed value of 'i' in
        // 'queryObj'.
        auto request = buildDelete(kNss, queryObj);
        const auto res =
            criTargeter.targetDelete(operationContext(), BatchItemRef(&request, 0)).endpoints;
        ASSERT_EQUALS(res.size(), 1);
        auto chunk = findChunk(queryObj["a"]["b"]);
        ASSERT_EQUALS(res[0].shardName, chunk.getShardId());
    }

    // Range queries on hashed field can target using the two phase write protocol.
    auto request = buildDelete(kNss, fromjson("{'a.b': {$gt : 101}}"));
    auto resRangeQuery =
        criTargeter.targetDelete(operationContext(), BatchItemRef(&request, 0)).endpoints;
    ASSERT_EQUALS(resRangeQuery.size(), 4);
}

TEST(SimpleCollectionRoutingInfoTargeterTest, ExtractBucketsShardKeyFromTimeseriesDocument) {
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
        auto actualShardKey =
            CollectionRoutingInfoTargeter::extractBucketsShardKeyFromTimeseriesDoc(
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

TEST_F(CollectionRoutingInfoTargeterTest, TestRoutingWithout_id) {
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    const auto targeter = prepare(BSON("a.b" << 1 << "_id" << 1), splitPoints);
    // Tests that routing writes when the shard key includes the _id field will throw an error if
    // the document does not contain _id.
    ASSERT_THROWS_CODE(targeter.targetInsert(operationContext(), BSON("a.b" << 10)),
                       DBException,
                       ErrorCodes::InvalidIdField);
}

TEST_F(CollectionRoutingInfoTargeterTest, ThrowOnCollectionlessAggregateNss) {
    const auto kCollectionlessAggNss =
        NamespaceString::createNamespaceString_forTest("db", "$cmd.aggregate");
    ASSERT_THROWS_CODE(
        [&] {
            CollectionRoutingInfoTargeter cri(operationContext(), kCollectionlessAggNss);
        }(),
        DBException,
        ErrorCodes::InvalidNamespace);
}

/**
 * Fixture that populates the CatalogCache with 'kNss' as an unsharded collection not tracked on the
 * configsvr, or a non-existent collection.
 */
class CollectionRoutingInfoTargeterUntrackedTest : public RouterCatalogCacheTestFixture {
public:
    CollectionRoutingInfoTargeter prepare() {
        const auto cri = makeUntrackedCollectionRoutingInfo(kNss);
        primaryShard = cri.getDbPrimaryShardId();
        dbVersion = cri.getDbVersion();
        return CollectionRoutingInfoTargeter(kNss, cri);
    };

    ShardId primaryShard;
    DatabaseVersion dbVersion;
};

TEST_F(CollectionRoutingInfoTargeterUntrackedTest, InsertIsTargetedToDbPrimaryShard) {
    const auto targeter = prepare();
    const auto shardEndpoint = targeter.targetInsert(operationContext(), BSON("x" << 10));
    ASSERT_EQ(primaryShard, shardEndpoint.shardName);
    ASSERT_EQ(dbVersion, shardEndpoint.databaseVersion);
    ASSERT_EQ(ChunkVersion::UNSHARDED(), shardEndpoint.shardVersion->placementVersion());
}

TEST_F(CollectionRoutingInfoTargeterUntrackedTest, UpdateIsTargetedToDbPrimaryShard) {
    const auto targeter = prepare();
    const auto update = buildUpdate(kNss, fromjson("{}"), fromjson("{$set: {p : 1}}"), false);
    const auto shardEndpoints =
        targeter.targetUpdate(operationContext(), BatchItemRef(&update, 0)).endpoints;

    ASSERT_EQ(1, shardEndpoints.size());
    const auto& shardEndpoint = shardEndpoints.front();
    ASSERT_EQ(primaryShard, shardEndpoint.shardName);
    ASSERT_EQ(dbVersion, shardEndpoint.databaseVersion);
    ASSERT_EQ(ChunkVersion::UNSHARDED(), shardEndpoint.shardVersion->placementVersion());
}

TEST_F(CollectionRoutingInfoTargeterUntrackedTest, DeleteIsTargetedToDbPrimaryShard) {
    const auto targeter = prepare();
    const auto deleteOp = buildDelete(kNss, fromjson("{}"));
    const auto shardEndpoints =
        targeter.targetDelete(operationContext(), BatchItemRef(&deleteOp, 0)).endpoints;

    ASSERT_EQ(1, shardEndpoints.size());
    const auto& shardEndpoint = shardEndpoints.front();
    ASSERT_EQ(primaryShard, shardEndpoint.shardName);
    ASSERT_EQ(dbVersion, shardEndpoint.databaseVersion);
    ASSERT_EQ(ChunkVersion::UNSHARDED(), shardEndpoint.shardVersion->placementVersion());
}

/**
 * Fixture that populates the CatalogCache with 'kNss' as an unsharded collection tracked on the
 * configsvr.
 */
class CollectionRoutingInfoTargeterUnshardedTest : public RouterCatalogCacheTestFixture {
public:
    CollectionRoutingInfoTargeter prepare() {
        const auto cri = makeUnshardedCollectionRoutingInfo(kNss);

        std::set<ShardId> shards;
        cri.getChunkManager().getAllShardIds(&shards);
        ASSERT_EQ(1, shards.size());
        owningShard = *shards.begin();

        shardVersion = cri.getCollectionVersion();

        return CollectionRoutingInfoTargeter(kNss, cri);
    };

    ShardId owningShard;
    ShardVersion shardVersion;
};

TEST_F(CollectionRoutingInfoTargeterUnshardedTest, InsertIsTargetedToOwningShard) {
    const auto targeter = prepare();
    const auto shardEndpoint = targeter.targetInsert(operationContext(), BSON("x" << 10));
    ASSERT_EQ(owningShard, shardEndpoint.shardName);
    ASSERT_EQ(boost::none, shardEndpoint.databaseVersion);
    ASSERT_EQ(shardVersion, shardEndpoint.shardVersion);
}

TEST_F(CollectionRoutingInfoTargeterUnshardedTest, UpdateIsTargetedToOwningShard) {
    const auto targeter = prepare();
    const auto update = buildUpdate(kNss, fromjson("{}"), fromjson("{$set: {p : 1}}"), false);
    const auto shardEndpoints =
        targeter.targetUpdate(operationContext(), BatchItemRef(&update, 0)).endpoints;

    ASSERT_EQ(1, shardEndpoints.size());
    const auto& shardEndpoint = shardEndpoints.front();
    ASSERT_EQ(owningShard, shardEndpoint.shardName);
    ASSERT_EQ(boost::none, shardEndpoint.databaseVersion);
    ASSERT_EQ(shardVersion, shardEndpoint.shardVersion);
}

TEST_F(CollectionRoutingInfoTargeterUnshardedTest, DeleteIsTargetedToOwningShard) {
    const auto targeter = prepare();
    const auto deleteOp = buildDelete(kNss, fromjson("{}"));
    const auto shardEndpoints =
        targeter.targetDelete(operationContext(), BatchItemRef(&deleteOp, 0)).endpoints;

    ASSERT_EQ(1, shardEndpoints.size());
    const auto& shardEndpoint = shardEndpoints.front();
    ASSERT_EQ(owningShard, shardEndpoint.shardName);
    ASSERT_EQ(boost::none, shardEndpoint.databaseVersion);
    ASSERT_EQ(shardVersion, shardEndpoint.shardVersion);
}

/**
 * Fixture that sets up a mock CatalogCache with several timeseries collections.
 */
class CollectionRoutingInfoTargeterTimeseriesTest : public ShardingTestFixtureWithMockCatalogCache {
public:
    CollectionRoutingInfoTargeterTimeseriesTest()
        : _dbName(DatabaseName::createDatabaseName_forTest(boost::none, "test")),
          _untrackedTimeseriesNss(
              NamespaceString::createNamespaceString_forTest(_dbName, "untrackedTS")),
          _unsplittableTimeseriesNss(
              NamespaceString::createNamespaceString_forTest(_dbName, "unsplittableTS")),
          _shardedTimeseriesNss(
              NamespaceString::createNamespaceString_forTest(_dbName, "shardedTS")) {
        _timeseriesOptions.setTimeField(_timeField);
        _timeseriesOptions.setMetaField(StringData(_metaField));
    }

    void setUp() override {
        ShardingTestFixtureWithMockCatalogCache::setUp();

        getCatalogCacheMock()->setDatabaseReturnValue(
            _dbName, CatalogCacheMock::makeDatabaseInfo(_dbName, _dbPrimaryShard, _dbVersion));

        auto extraOptions =
            CatalogCacheMock::ExtraCollectionOptions{.timeseriesOptions{_timeseriesOptions}};

        // _untrackedTimeseriesNss on dbPrimary shard (_shard0).
        getCatalogCacheMock()->setCollectionReturnValue(
            _untrackedTimeseriesNss,
            CatalogCacheMock::makeCollectionRoutingInfoUntracked(
                _untrackedTimeseriesNss, _dbPrimaryShard, _dbVersion));
        getCatalogCacheMock()->setCollectionReturnValue(
            _untrackedTimeseriesNss.makeTimeseriesBucketsNamespace(),
            CatalogCacheMock::makeCollectionRoutingInfoUntracked(
                _untrackedTimeseriesNss.makeTimeseriesBucketsNamespace(),
                _dbPrimaryShard,
                _dbVersion));

        // _unsplittableTimeseriesNss on _shard1.
        getCatalogCacheMock()->setCollectionReturnValue(
            _unsplittableTimeseriesNss,
            CatalogCacheMock::makeCollectionRoutingInfoUntracked(
                _unsplittableTimeseriesNss, _dbPrimaryShard, _dbVersion));
        getCatalogCacheMock()->setCollectionReturnValue(
            _unsplittableTimeseriesNss.makeTimeseriesBucketsNamespace(),
            CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
                _unsplittableTimeseriesNss.makeTimeseriesBucketsNamespace(),
                _dbPrimaryShard,
                _dbVersion,
                _shard1,
                extraOptions));

        // _shardedTimeseriesNss:
        // - {meta: MINKEY} to {meta: 0} on _shard0.
        // - {meta: 0} to {meta: MAXKEY} on _shard1.
        getCatalogCacheMock()->setCollectionReturnValue(
            _shardedTimeseriesNss,
            CatalogCacheMock::makeCollectionRoutingInfoUntracked(
                _shardedTimeseriesNss, _dbPrimaryShard, _dbVersion));
        getCatalogCacheMock()->setCollectionReturnValue(
            _shardedTimeseriesNss.makeTimeseriesBucketsNamespace(),
            CatalogCacheMock::makeCollectionRoutingInfoSharded(
                _shardedTimeseriesNss.makeTimeseriesBucketsNamespace(),
                _dbPrimaryShard,
                _dbVersion,
                KeyPattern(BSON(timeseries::kBucketMetaFieldName << 1)),
                {{ChunkRange(BSON(timeseries::kBucketMetaFieldName << MINKEY),
                             BSON(timeseries::kBucketMetaFieldName << 0)),
                  _shard0},
                 {ChunkRange(BSON(timeseries::kBucketMetaFieldName << 0),
                             BSON(timeseries::kBucketMetaFieldName << MAXKEY)),
                  _shard1}},
                extraOptions));
    }

protected:
    const DatabaseName _dbName;
    const NamespaceString _untrackedTimeseriesNss;
    const NamespaceString _unsplittableTimeseriesNss;
    const NamespaceString _shardedTimeseriesNss;

    const ShardId _shard0{"shard0"};
    const ShardId _shard1{"shard1"};

    const ShardId _dbPrimaryShard{_shard0};
    const DatabaseVersion _dbVersion{UUID::gen(), Timestamp(12, 34)};

    const std::string _timeField{"timeField"};
    const std::string _metaField{"metaField"};
    TimeseriesOptions _timeseriesOptions;
};

TEST_F(CollectionRoutingInfoTargeterTimeseriesTest, TargetWritesToUntrackedTimeseries) {
    // Expect all operations to be targeted to the db-primary shard, with 'databaseVersion' attached
    // and 'shardVersion=UNSHARDED.
    auto checkEndpoint = [&](const ShardEndpoint& shardEndpoint) {
        ASSERT_EQ(_dbPrimaryShard, shardEndpoint.shardName);
        ASSERT_EQ(_dbVersion, shardEndpoint.databaseVersion);
        ASSERT_EQ(ChunkVersion::UNSHARDED(), shardEndpoint.shardVersion->placementVersion());
    };

    CollectionRoutingInfoTargeter cri(operationContext(), _untrackedTimeseriesNss);

    // Insert
    const auto endpointInsert = cri.targetInsert(operationContext(), BSON("x" << 1));
    checkEndpoint(endpointInsert);

    // Update
    const auto update = buildUpdate(kNss, fromjson("{}"), fromjson("{$set: {p : 1}}"), false);
    const auto endpointUpdate =
        cri.targetUpdate(operationContext(), BatchItemRef(&update, 0)).endpoints;
    ASSERT_EQ(1, endpointUpdate.size());
    checkEndpoint(endpointUpdate.front());

    // Delete
    const auto deleteOp = buildDelete(kNss, fromjson("{}"));
    const auto endpointDelete =
        cri.targetDelete(operationContext(), BatchItemRef(&deleteOp, 0)).endpoints;
    ASSERT_EQ(1, endpointDelete.size());
    checkEndpoint(endpointDelete.front());
}


TEST_F(CollectionRoutingInfoTargeterTimeseriesTest, TargetWritesToUnsplittableTimeseries) {
    const auto nss = _unsplittableTimeseriesNss;
    const auto bucketsNss = nss.makeTimeseriesBucketsNamespace();

    // Expect all operations to be targeted to '_shard1', with a shard1's ShardVersion attached and
    // no 'databaseVersion'.
    auto checkEndpoint = [&](const ShardEndpoint& shardEndpoint) {
        const auto expectedShardVersion =
            uassertStatusOK(getCatalogCacheMock()->getCollectionRoutingInfo(
                                operationContext(), bucketsNss, false))
                .getShardVersion(_shard1);

        ASSERT_EQ(_shard1, shardEndpoint.shardName);
        ASSERT_EQ(boost::none, shardEndpoint.databaseVersion);
        ASSERT_EQ(expectedShardVersion, shardEndpoint.shardVersion);
    };

    CollectionRoutingInfoTargeter cri(operationContext(), nss);
    ASSERT_EQ(1, cri.getAproxNShardsOwningChunks());

    // Insert
    const auto endpointInsert = cri.targetInsert(operationContext(), BSON("x" << 1));
    checkEndpoint(endpointInsert);

    // Update
    const auto update = buildUpdate(kNss, fromjson("{}"), fromjson("{$set: {p : 1}}"), false);
    const auto endpointUpdate =
        cri.targetUpdate(operationContext(), BatchItemRef(&update, 0)).endpoints;
    ASSERT_EQ(1, endpointUpdate.size());
    checkEndpoint(endpointUpdate.front());

    // Delete
    const auto deleteOp = buildDelete(kNss, fromjson("{}"));
    const auto endpointDelete =
        cri.targetDelete(operationContext(), BatchItemRef(&deleteOp, 0)).endpoints;
    ASSERT_EQ(1, endpointDelete.size());
    checkEndpoint(endpointDelete.front());
}

TEST_F(CollectionRoutingInfoTargeterTimeseriesTest, TargetWritesToShardedTimeseries) {
    const auto nss = _shardedTimeseriesNss;
    const auto bucketsNss = nss.makeTimeseriesBucketsNamespace();

    auto checkEndpoints = [&](const std::vector<ShardEndpoint>& endpoints,
                              std::set<ShardId> expectedTargetedShards) {
        ASSERT_EQ(expectedTargetedShards.size(), endpoints.size());
        const auto collectionRoutingInfo = uassertStatusOK(
            getCatalogCacheMock()->getCollectionRoutingInfo(operationContext(), bucketsNss, false));
        for (const auto& endpoint : endpoints) {
            const auto it = expectedTargetedShards.find(endpoint.shardName);
            ASSERT_TRUE(it != expectedTargetedShards.end());
            expectedTargetedShards.erase(it);

            const auto expectedShardVersion =
                collectionRoutingInfo.getShardVersion(endpoint.shardName);
            ASSERT_EQ(boost::none, endpoint.databaseVersion);
            ASSERT_EQ(expectedShardVersion, endpoint.shardVersion);
        }
    };

    CollectionRoutingInfoTargeter cri(operationContext(), nss);
    ASSERT_EQ(2, cri.getAproxNShardsOwningChunks());

    // Insert
    {
        const auto endpoint = cri.targetInsert(
            operationContext(),
            BSON(_metaField << -1 << _timeField
                            << uassertStatusOK(dateFromISOString("2023-01-01T00:00:00Z"))));
        checkEndpoints({endpoint}, {_shard0});
    }

    {
        const auto endpoint = cri.targetInsert(
            operationContext(),
            BSON(_metaField << 1 << _timeField
                            << uassertStatusOK(dateFromISOString("2023-01-01T00:00:00Z"))));
        checkEndpoints({endpoint}, {_shard1});
    }

    // Update
    {
        const auto update = buildUpdate(
            kNss, BSON(_metaField << -1), fromjson("{$set: {p : 1}}"), false, true /*multi*/);
        const auto endpoints =
            cri.targetUpdate(operationContext(), BatchItemRef(&update, 0)).endpoints;
        checkEndpoints(endpoints, {_shard0});
    }

    {
        const auto update = buildUpdate(kNss,
                                        BSON(_metaField << BSON("$gte" << -10 << "$lt" << 10)),
                                        fromjson("{$set: {p : 1}}"),
                                        false,
                                        true /*multi*/);
        const auto endpoints =
            cri.targetUpdate(operationContext(), BatchItemRef(&update, 0)).endpoints;
        checkEndpoints(endpoints, {_shard0, _shard1});
    }

    // Delete
    {
        const auto deleteOp = buildDelete(kNss, BSON(_metaField << -1), true /*multi*/);
        const auto endpoints =
            cri.targetDelete(operationContext(), BatchItemRef(&deleteOp, 0)).endpoints;
        checkEndpoints(endpoints, {_shard0});
    }

    {
        const auto deleteOp = buildDelete(
            kNss, BSON(_metaField << BSON("$gte" << -10 << "$lt" << 10)), true /*multi*/);
        const auto endpoints =
            cri.targetDelete(operationContext(), BatchItemRef(&deleteOp, 0)).endpoints;
        checkEndpoints(endpoints, {_shard0, _shard1});
    }
}

TEST_F(CollectionRoutingInfoTargeterTimeseriesTest, UntrackedAreNotTranslatedToBucketsNs) {
    const auto nss = _untrackedTimeseriesNss;

    CollectionRoutingInfoTargeter cri(operationContext(), nss);
    ASSERT_EQ(nss, cri.getNS());
    ASSERT_EQ(false, cri.isTrackedTimeSeriesBucketsNamespace());
    ASSERT_FALSE(cri.timeseriesNamespaceNeedsRewrite(nss));
    ASSERT_EQ(0, cri.getAproxNShardsOwningChunks());
}

TEST_F(CollectionRoutingInfoTargeterTimeseriesTest, TrackedAreTranslatedToBucketsNs) {
    auto testFn = [&](const NamespaceString& nss) {
        const auto bucketsNss = nss.makeTimeseriesBucketsNamespace();

        CollectionRoutingInfoTargeter cri(operationContext(), nss);
        ASSERT_EQ(bucketsNss, cri.getNS());
        ASSERT_EQ(true, cri.isTrackedTimeSeriesBucketsNamespace());
        ASSERT_TRUE(cri.timeseriesNamespaceNeedsRewrite(nss));
        ASSERT_FALSE(cri.timeseriesNamespaceNeedsRewrite(bucketsNss));
        ASSERT_EQ(bucketsNss, cri.getRoutingInfo().getChunkManager().getNss());
    };

    testFn(_unsplittableTimeseriesNss);
    testFn(_shardedTimeseriesNss);
}

TEST_F(CollectionRoutingInfoTargeterTimeseriesTest, RefreshOnStaleResponse) {
    const auto nss = _untrackedTimeseriesNss;
    const auto bucketsNss = nss.makeTimeseriesBucketsNamespace();

    CollectionRoutingInfoTargeter cri(operationContext(), nss);
    ASSERT_EQ(nss, cri.getNS());
    ASSERT_EQ(false, cri.isTrackedTimeSeriesBucketsNamespace());
    ASSERT_FALSE(cri.timeseriesNamespaceNeedsRewrite(nss));

    auto sci = StaleConfigInfo(bucketsNss, ShardVersion::UNSHARDED(), boost::none, _shard0);

    // No need to refresh when no stale info or targeting error is present.
    ASSERT_FALSE(cri.refreshIfNeeded(operationContext()));

    // Setup new metadata on the CatalogCache representing nss is now a sharded time series.
    getCatalogCacheMock()->setCollectionReturnValue(
        bucketsNss,
        CatalogCacheMock::makeCollectionRoutingInfoSharded(
            bucketsNss,
            _dbPrimaryShard,
            _dbVersion,
            KeyPattern(BSON(timeseries::kBucketMetaFieldName << 1)),
            {{ChunkRange(BSON(timeseries::kBucketMetaFieldName << MINKEY),
                         BSON(timeseries::kBucketMetaFieldName << 0)),
              _shard0},
             {ChunkRange(BSON(timeseries::kBucketMetaFieldName << 0),
                         BSON(timeseries::kBucketMetaFieldName << MAXKEY)),
              _shard1}},
            CatalogCacheMock::ExtraCollectionOptions{.timeseriesOptions{_timeseriesOptions}}));

    cri.noteStaleCollVersionResponse(operationContext(), sci);
    ASSERT_TRUE(cri.refreshIfNeeded(operationContext()));

    // CollectionRoutingInfoTargeter became aware that nss is now a sharded time series.
    ASSERT_EQ(bucketsNss, cri.getNS());
    ASSERT_EQ(true, cri.isTrackedTimeSeriesBucketsNamespace());
    ASSERT_TRUE(cri.timeseriesNamespaceNeedsRewrite(nss));

    // Setup new metadata on the CatalogCache representing nss is now an untracked time series.
    getCatalogCacheMock()->setCollectionReturnValue(
        bucketsNss,
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(
            bucketsNss, _dbPrimaryShard, _dbVersion));

    cri.noteStaleCollVersionResponse(operationContext(), sci);
    ASSERT_TRUE(cri.refreshIfNeeded(operationContext()));

    // CollectionRoutingInfoTargeter became aware that nss is now an untracked time series.
    ASSERT_EQ(nss, cri.getNS());
    ASSERT_EQ(false, cri.isTrackedTimeSeriesBucketsNamespace());
    ASSERT_FALSE(cri.timeseriesNamespaceNeedsRewrite(nss));
}

/*
 * Verifies that refreshIfNeeded() compares StaleConfigInfo metadata against refreshed routing
 * information as expected.
 */
TEST_F(ShardingTestFixtureWithMockCatalogCache, TestRefreshIfNeededAgainstUntrackedCollection) {
    const auto dbName = DatabaseName::createDatabaseName_forTest(boost::none, "testDB");
    const auto nss = NamespaceString::createNamespaceString_forTest(dbName, "testColl");
    const ShardId shard0{"shard0"};
    const ShardId shard1{"shard1"};
    const ShardId primaryShard{shard0};

    DatabaseVersion dbVersion{UUID::gen(), Timestamp(0, 1)};
    getCatalogCacheMock()->setDatabaseReturnValue(
        dbName, CatalogCacheMock::makeDatabaseInfo(dbName, primaryShard, dbVersion));

    const StaleConfigInfo dummyStaleConfigInfo(nss, ShardVersion::UNSHARDED(), boost::none, shard0);

    // Install metadata for an untracked collection - then verify against:
    const auto initialCollVersion =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nss, primaryShard, dbVersion);
    getCatalogCacheMock()->setCollectionReturnValue(nss, initialCollVersion);
    getCatalogCacheMock()->setCollectionReturnValue(nss.makeTimeseriesBucketsNamespace(),
                                                    initialCollVersion);
    CollectionRoutingInfoTargeter cri(operationContext(), nss);

    // 1) No error noted.
    ASSERT_FALSE(cri.refreshIfNeeded(operationContext()));

    // 2) An error notification, but with no changes in the metadata returned by the catalog cache.
    cri.noteStaleCollVersionResponse(operationContext(), dummyStaleConfigInfo);
    ASSERT_FALSE(cri.refreshIfNeeded(operationContext()));

    // 3) An error notification, plus an updated unsharded version returned by the catalog cache.
    dbVersion = dbVersion.makeUpdated();
    const auto collVersionWithBumpedDbVersion =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nss, primaryShard, dbVersion);
    getCatalogCacheMock()->setCollectionReturnValue(nss, collVersionWithBumpedDbVersion);
    getCatalogCacheMock()->setCollectionReturnValue(nss.makeTimeseriesBucketsNamespace(),
                                                    collVersionWithBumpedDbVersion);

    cri.noteStaleCollVersionResponse(operationContext(), dummyStaleConfigInfo);
    ASSERT_TRUE(cri.refreshIfNeeded(operationContext()));

    // 4) An error notification, plus an updated tracked version returned by the catalog cache.
    const ShardId dataShard{shard1};
    const auto versionAfterSimulatedMoveCollection =
        CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
            nss, primaryShard, dbVersion, dataShard);
    getCatalogCacheMock()->setCollectionReturnValue(nss, versionAfterSimulatedMoveCollection);

    cri.noteStaleCollVersionResponse(operationContext(), dummyStaleConfigInfo);
    ASSERT_TRUE(cri.refreshIfNeeded(operationContext()));
}

TEST_F(ShardingTestFixtureWithMockCatalogCache, TestRefreshIfNeededAgainstTrackedCollection) {
    const auto dbName = DatabaseName::createDatabaseName_forTest(boost::none, "testDB");
    const auto nss = NamespaceString::createNamespaceString_forTest(dbName, "testColl");
    const ShardId shard0{"shard0"};
    const ShardId shard1{"shard1"};
    const ShardId primaryShard{"shard0"};
    const ShardId dataShard{"shard1"};

    DatabaseVersion dbVersion{UUID::gen(), Timestamp(0, 1)};
    getCatalogCacheMock()->setDatabaseReturnValue(
        dbName, CatalogCacheMock::makeDatabaseInfo(dbName, primaryShard, dbVersion));

    const StaleConfigInfo dummyStaleConfigInfo(nss, ShardVersion::UNSHARDED(), boost::none, shard0);

    // Install metadata for a tracked collection - then verify against:
    const auto initialCollVersion = CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
        nss, primaryShard, dbVersion, dataShard);
    getCatalogCacheMock()->setCollectionReturnValue(nss, initialCollVersion);
    CollectionRoutingInfoTargeter cri(operationContext(), nss);

    // 1) No error noted.
    ASSERT_FALSE(cri.refreshIfNeeded(operationContext()));

    // 2) An error notification, but with no changes in the metadata returned by the catalog cache.
    cri.noteStaleCollVersionResponse(operationContext(), dummyStaleConfigInfo);
    ASSERT_FALSE(cri.refreshIfNeeded(operationContext()));

    // 3) An error notification, plus an updated tracked version returned by the catalog cache.
    const auto collVersionWhenSharded = CatalogCacheMock::makeCollectionRoutingInfoSharded(
        nss,
        primaryShard,
        dbVersion,
        KeyPattern(BSON("_id" << 1)),
        {{ChunkRange(BSON("_id" << MINKEY), BSON("_id" << 0)), shard0},
         {ChunkRange(BSON("_id" << 0), BSON("_id" << MAXKEY)), shard1}});
    getCatalogCacheMock()->setCollectionReturnValue(nss, collVersionWhenSharded);

    cri.noteStaleCollVersionResponse(operationContext(), dummyStaleConfigInfo);
    ASSERT_TRUE(cri.refreshIfNeeded(operationContext()));

    // 4) An error notification, plus an updated unsharded version returned by the catalog cache.
    const auto versionAfterUntrackCollection =
        CatalogCacheMock::makeCollectionRoutingInfoUntracked(nss, primaryShard, dbVersion);
    getCatalogCacheMock()->setCollectionReturnValue(nss, versionAfterUntrackCollection);
    getCatalogCacheMock()->setCollectionReturnValue(nss.makeTimeseriesBucketsNamespace(),
                                                    versionAfterUntrackCollection);

    cri.noteStaleCollVersionResponse(operationContext(), dummyStaleConfigInfo);
    ASSERT_TRUE(cri.refreshIfNeeded(operationContext()));
}

}  // namespace
}  // namespace mongo
