/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include <boost/move/utility_core.hpp>
#include <memory>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/duration.h"

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

class ShardKeyPatternQueryUtilTest : public RouterCatalogCacheTestFixture {
public:
    CollectionRoutingInfo prepare(BSONObj shardKeyPattern,
                                  const std::vector<BSONObj>& splitPoints) {
        return makeCollectionRoutingInfo(
            kNss, ShardKeyPattern(shardKeyPattern), nullptr, false, splitPoints, {});
    }

protected:
    void testGetShardIdsAndChunksUpdateWithRangePrefixHashedShardKey();
    void testGetShardIdsAndChunksUpdateWithHashedPrefixHashedShardKey();
    void testGetShardIdsAndChunksDeleteWithExactId();
    void testGetShardIdsAndChunksDeleteWithHashedPrefixHashedShardKey();
    void testGetShardIdsAndChunksDeleteWithRangePrefixHashedShardKey();

private:
    std::unique_ptr<CanonicalQuery> makeCQ(const BSONObj& query,
                                           const BSONObj& collation,
                                           const ChunkManager& cm) {
        // Parse query.
        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(query);
        auto expCtx =
            ExpressionContextBuilder{}.fromRequest(operationContext(), *findCommand).build();
        expCtx->setUUID(cm.getUUID());

        if (!collation.isEmpty()) {
            findCommand->setCollation(collation.getOwned());
        } else if (cm.getDefaultCollator()) {
            auto defaultCollator = cm.getDefaultCollator();
            expCtx->setCollator(defaultCollator->clone());
        }

        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx, .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    std::unique_ptr<CanonicalQuery> makeCQUpdate(BatchedCommandRequest& request,
                                                 const BSONObj& collation,
                                                 const ChunkManager& cm) {
        auto itemRef = BatchItemRef(&request, 0);
        return makeCQ(itemRef.getUpdateRef().getFilter(), fromjson("{}"), cm);
    }

    std::unique_ptr<CanonicalQuery> makeCQDelete(BatchedCommandRequest& request,
                                                 const BSONObj& collation,
                                                 const ChunkManager& cm) {
        auto itemRef = BatchItemRef(&request, 0);
        return makeCQ(itemRef.getDeleteRef().getFilter(), fromjson("{}"), cm);
    }
};

void ShardKeyPatternQueryUtilTest::testGetShardIdsAndChunksUpdateWithRangePrefixHashedShardKey() {
    std::set<ShardId> shardIds;
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has chunk
    // [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto cri = prepare(BSON("a.b" << 1 << "c.d"
                                  << "hashed"),
                       splitPoints);

    // When update targets using replacement object.
    auto request = buildUpdate(
        kNss, fromjson("{'a.b': {$gt : 2}}"), fromjson("{a: {b: -1}}"), /*upsert=*/false);

    shard_key_pattern_query_util::QueryTargetingInfo info;

    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(request, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);

    ASSERT_EQUALS(shardIds.size(), 2);
    ASSERT_TRUE(shardIds.contains(ShardId("3")));
    ASSERT_TRUE(shardIds.contains(ShardId("4")));

    ASSERT_EQUALS(info.chunkRanges.size(), 2);
    ASSERT_TRUE(info.chunkRanges.contains(
        {BSON("a.b" << 0 << "c.d" << MINKEY), BSON("a.b" << 100 << "c.d" << MINKEY)}));
    ASSERT_TRUE(info.chunkRanges.contains(
        {BSON("a.b" << 100 << "c.d" << MINKEY), BSON("a.b" << MAXKEY << "c.d" << MAXKEY)}));
    info.chunkRanges.clear();
    shardIds.clear();

    // When update targets using query.
    auto requestAndSet = buildUpdate(kNss,
                                     fromjson("{$and: [{'a.b': {$gte : 0}}, {'a.b': {$lt: 99}}]}}"),
                                     fromjson("{$set: {p : 1}}"),
                                     false);

    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(requestAndSet, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);

    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("3")));

    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(), BSON("a.b" << 0 << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(), BSON("a.b" << 100 << "c.d" << MINKEY));
    info.chunkRanges.clear();
    shardIds.clear();

    auto requestLT =
        buildUpdate(kNss, fromjson("{'a.b': {$lt : -101}}"), fromjson("{a: {b: 111}}"), false);
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(requestLT, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("1")));
    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(),
                      BSON("a.b" << BSONNULL << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(), BSON("a.b" << -100 << "c.d" << MINKEY));
    info.chunkRanges.clear();
    shardIds.clear();

    // For op-style updates, query on _id gets targeted to all shards.
    auto requestOpUpdate =
        buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{$set: {p: 111}}"), false);
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(requestOpUpdate, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 5);
    ASSERT_EQUALS(info.chunkRanges.size(), 5);
    auto itRange = info.chunkRanges.cbegin();
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
    info.chunkRanges.clear();
    shardIds.clear();

    // For replacement style updates, query on _id uses replacement doc to target. If the
    // replacement doc doesn't have shard key fields, then update should be routed to the shard
    // holding 'null' shard key documents.
    auto requestReplUpdate = buildUpdate(kNss, fromjson("{_id: 1}"), fromjson("{p: 111}}"), false);
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(requestReplUpdate, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 5);
    ASSERT_EQUALS(info.chunkRanges.size(), 5);
    info.chunkRanges.clear();
    shardIds.clear();

    // Upsert without full shard key.
    auto requestFullKey =
        buildUpdate(kNss, fromjson("{'a.b':  100}"), fromjson("{a: {b: -111}}"), true);
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(requestFullKey, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("4")));
    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(), BSON("a.b" << 100 << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(),
                      BSON("a.b" << MAXKEY << "c.d" << MAXKEY));
    info.chunkRanges.clear();
    shardIds.clear();

    // Upsert with full shard key.
    auto requestSuccess =
        buildUpdate(kNss, fromjson("{'a.b': 100, 'c.d': 'val'}"), fromjson("{a: {b: -111}}"), true);
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(requestSuccess, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("4")));
    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(), BSON("a.b" << 100 << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(),
                      BSON("a.b" << MAXKEY << "c.d" << MAXKEY));
    info.chunkRanges.clear();
    shardIds.clear();
}

void ShardKeyPatternQueryUtilTest::testGetShardIdsAndChunksUpdateWithHashedPrefixHashedShardKey() {
    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has
    // chunk [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};

    auto cri = prepare(BSON("a.b"
                            << "hashed"
                            << "c.d" << 1),
                       splitPoints);

    auto findChunk = [&](BSONElement elem) {
        return cri.cm.findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    for (int i = 0; i < 1000; i++) {
        std::set<ShardId> shardIds;
        shard_key_pattern_query_util::QueryTargetingInfo info;
        auto updateQueryObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));

        // Verify that the given document is being routed based on hashed value of 'i' in
        // 'updateQueryObj'.
        auto request = buildUpdate(kNss, updateQueryObj, fromjson("{$set: {p: 1}}"), false);
        getShardIdsAndChunksForCanonicalQuery(
            *makeCQUpdate(request, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
        ASSERT_EQUALS(shardIds.size(), 1);
        auto chunk = findChunk(updateQueryObj["a"]["b"]);
        ASSERT_TRUE(shardIds.contains(chunk.getShardId()));
        // TODO 22
        ASSERT_EQUALS(info.chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(), chunk.getMin());
        ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(), chunk.getMax());
    }

    // Range queries will be able to target using the two phase write protocol.
    std::set<ShardId> shardIds;
    shard_key_pattern_query_util::QueryTargetingInfo info;
    const auto updateObj = fromjson("{a: {b: -1}}");
    auto requestUpdate = buildUpdate(kNss, fromjson("{'a.b': {$gt : 101}}"), updateObj, false);
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQUpdate(requestUpdate, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 4);
    ASSERT_EQUALS(info.chunkRanges.size(), 4);
}

void ShardKeyPatternQueryUtilTest::testGetShardIdsAndChunksDeleteWithExactId() {
    std::set<ShardId> shardIds;
    shard_key_pattern_query_util::QueryTargetingInfo info;
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100), BSON("a.b" << 0), BSON("a.b" << 100)};
    auto cri = prepare(BSON("a.b" << 1), splitPoints);

    auto requestId = buildDelete(kNss, fromjson("{_id: 68755000}"));
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(requestId, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 5);
    ASSERT_TRUE(shardIds.contains(ShardId("0")));
    ASSERT_EQUALS(info.chunkRanges.size(), 5);
    auto itRange = info.chunkRanges.cbegin();
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

void ShardKeyPatternQueryUtilTest::testGetShardIdsAndChunksDeleteWithRangePrefixHashedShardKey() {
    std::set<ShardId> shardIds;
    shard_key_pattern_query_util::QueryTargetingInfo info;
    // Create 5 chunks and 5 shards such that shardId '0' has chunk [MinKey, null), '1' has
    // chunk [null, -100), '2' has chunk [-100, 0), '3' has chunk ['0', 100) and '4' has chunk
    // [100, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << BSONNULL), BSON("a.b" << -100LL), BSON("a.b" << 0LL), BSON("a.b" << 100LL)};
    auto cri = prepare(BSON("a.b" << 1 << "c.d"
                                  << "hashed"),
                       splitPoints);

    // Can delete wih partial shard key in the query if the query only targets one shard.
    auto requestPartialKey = buildDelete(kNss, fromjson("{'a.b': {$gt : 101}}"));
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(requestPartialKey, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("4")));
    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(), BSON("a.b" << 100 << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(),
                      BSON("a.b" << MAXKEY << "c.d" << MAXKEY));
    shardIds.clear();
    info.chunkRanges.clear();

    // Test delete with range query.
    auto requestPartialKey2 = buildDelete(kNss, fromjson("{'a.b': {$gt: 0}}"));
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(requestPartialKey2, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 2);
    ASSERT_TRUE(shardIds.contains(ShardId("3")));
    ASSERT_TRUE(shardIds.contains(ShardId("4")));
    ASSERT_EQUALS(info.chunkRanges.size(), 2);
    ASSERT_TRUE(info.chunkRanges.contains(
        {BSON("a.b" << 0 << "c.d" << MINKEY), BSON("a.b" << 100 << "c.d" << MINKEY)}));
    ASSERT_TRUE(info.chunkRanges.contains(
        {BSON("a.b" << 100 << "c.d" << MINKEY), BSON("a.b" << MAXKEY << "c.d" << MAXKEY)}));
    shardIds.clear();
    info.chunkRanges.clear();

    // Test delete with no shard key.
    auto requestNoShardKey = buildDelete(kNss, fromjson("{'k': 0}"));
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(requestNoShardKey, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 5);
    ASSERT_EQUALS(info.chunkRanges.size(), 5);
    shardIds.clear();
    info.chunkRanges.clear();

    // Delete targeted correctly with full shard key in query.
    auto requestFullKey = buildDelete(kNss, fromjson("{'a.b': -101, 'c.d': 5}"));
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(requestFullKey, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("1")));
    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(),
                      BSON("a.b" << BSONNULL << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(), BSON("a.b" << -100 << "c.d" << MINKEY));
    shardIds.clear();
    info.chunkRanges.clear();

    // Query with MinKey value should go to chunk '0' because MinKey is smaller than BSONNULL.
    auto requestMinKey =
        buildDelete(kNss, BSONObjBuilder().appendMinKey("a.b").append("c.d", 4).obj());
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(requestMinKey, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("0")));
    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(),
                      BSON("a.b" << MINKEY << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(),
                      BSON("a.b" << BSONNULL << "c.d" << MINKEY));
    shardIds.clear();
    info.chunkRanges.clear();

    auto requestMinKey2 = buildDelete(kNss, fromjson("{'a.b':  0, 'c.d': 5}"));
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(requestMinKey2, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 1);
    ASSERT_TRUE(shardIds.contains(ShardId("3")));
    ASSERT_EQUALS(info.chunkRanges.size(), 1);
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(), BSON("a.b" << 0 << "c.d" << MINKEY));
    ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(), BSON("a.b" << 100 << "c.d" << MINKEY));
}

void ShardKeyPatternQueryUtilTest::testGetShardIdsAndChunksDeleteWithHashedPrefixHashedShardKey() {
    std::set<ShardId> shardIds;
    shard_key_pattern_query_util::QueryTargetingInfo info;

    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has
    // chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto cri = prepare(BSON("a.b"
                            << "hashed"
                            << "c.d" << 1),
                       splitPoints);

    auto findChunk = [&](BSONElement elem) {
        return cri.cm.findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << BSONElementHasher::hash64(elem, BSONElementHasher::DEFAULT_HASH_SEED)));
    };

    for (int i = 0; i < 1000; i++) {
        auto queryObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));
        // Verify that the given document is being routed based on hashed value of 'i' in
        // 'queryObj'.
        auto request = buildDelete(kNss, queryObj);
        getShardIdsAndChunksForCanonicalQuery(
            *makeCQDelete(request, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
        ASSERT_EQUALS(shardIds.size(), 1);
        auto chunk = findChunk(queryObj["a"]["b"]);
        ASSERT_TRUE(shardIds.contains(chunk.getShardId()));

        ASSERT_EQUALS(info.chunkRanges.size(), 1);
        ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMin(), chunk.getMin());
        ASSERT_BSONOBJ_EQ(info.chunkRanges.cbegin()->getMax(), chunk.getMax());
        shardIds.clear();
        info.chunkRanges.clear();
    }

    // Range queries on hashed field can target using the two phase write protocol.
    auto request = buildDelete(kNss, fromjson("{'a.b': {$gt : 101}}"));
    getShardIdsAndChunksForCanonicalQuery(
        *makeCQDelete(request, fromjson("{}"), cri.cm), cri.cm, &shardIds, &info, false);
    ASSERT_EQUALS(shardIds.size(), 4);
    ASSERT_EQUALS(info.chunkRanges.size(), 4);
}

TEST_F(ShardKeyPatternQueryUtilTest, GetShardIdsAndChunksUpdateWithRangePrefixHashedShardKey) {
    testGetShardIdsAndChunksUpdateWithRangePrefixHashedShardKey();
}

TEST_F(ShardKeyPatternQueryUtilTest, GetShardIdsAndChunksUpdateWithHashedPrefixHashedShardKey) {
    testGetShardIdsAndChunksUpdateWithHashedPrefixHashedShardKey();
}

TEST_F(ShardKeyPatternQueryUtilTest, GetShardIdsAndChunksDeleteWithExactId) {
    testGetShardIdsAndChunksDeleteWithExactId();
}

TEST_F(ShardKeyPatternQueryUtilTest, GetShardIdsAndChunksDeleteWithHashedPrefixHashedShardKey) {
    testGetShardIdsAndChunksDeleteWithHashedPrefixHashedShardKey();
}

TEST_F(ShardKeyPatternQueryUtilTest, GetShardIdsAndChunksDeleteWithRangePrefixHashedShardKey) {
    testGetShardIdsAndChunksDeleteWithRangePrefixHashedShardKey();
}

}  // namespace
}  // namespace mongo
