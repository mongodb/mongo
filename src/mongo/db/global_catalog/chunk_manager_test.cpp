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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/hasher.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const ShardId kThisShard = ShardId{"thisShard"};

class ChunkManagerTest : public RouterCatalogCacheTestFixture {
public:
    CollectionRoutingInfo prepare(BSONObj shardKeyPattern,
                                  const std::vector<BSONObj>& splitPoints) {
        return makeCollectionRoutingInfo(
            kNss, ShardKeyPattern(shardKeyPattern), nullptr, false, splitPoints, {});
    }

protected:
    ChunkRange getChunkRangeForDoc(const std::string& doc, const ChunkManager& cm) {
        auto insertObj = fromjson(doc);
        const auto& shardKeyPattern = cm.getShardKeyPattern();
        BSONObj shardKey = shardKeyPattern.extractShardKeyFromDoc(insertObj);
        auto intersectingChunk = cm.findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);
        return intersectingChunk.getRange();
    }

    void checkChunkRangeForDoc(const std::string& doc,
                               const ChunkManager& cm,
                               const BSONObj& min,
                               const BSONObj& max) {
        auto chunkRange = getChunkRangeForDoc(doc, cm);
        ASSERT_BSONOBJ_EQ(chunkRange.getMin(), min);
        ASSERT_BSONOBJ_EQ(chunkRange.getMax(), max);
    }
};

TEST_F(ChunkManagerTest, FindIntersectingWithVaryingHashedPrefixAndConstantRangedSuffix) {
    // Create 4 chunks and 4 shards such that shardId '0' has chunk [MinKey, -2^62), '1' has chunk
    // [-2^62, 0), '2' has chunk ['0', 2^62) and '3' has chunk [2^62, MaxKey).
    std::vector<BSONObj> splitPoints = {
        BSON("a.b" << -(1LL << 62)), BSON("a.b" << 0LL), BSON("a.b" << (1LL << 62))};
    auto cri = prepare(BSON("a.b" << "hashed"
                                  << "c.d" << 1),
                       splitPoints);

    const auto& shardKeyPattern = cri.getChunkManager().getShardKeyPattern();
    for (int i = 0; i < 1000; i++) {
        auto insertObj = BSON("a" << BSON("b" << i) << "c" << BSON("d" << 10));

        BSONObj shardKey = shardKeyPattern.extractShardKeyFromDoc(insertObj);

        auto hashValue =
            BSONElementHasher::hash64(insertObj["a"]["b"], BSONElementHasher::DEFAULT_HASH_SEED);

        auto intersectingChunk =
            cri.getChunkManager().findIntersectingChunk(shardKey, CollationSpec::kSimpleSpec);

        // Verify that the given document is being routed based on hashed value of 'i'.
        auto expectedChunk = cri.getChunkManager().findIntersectingChunkWithSimpleCollation(
            BSON("a.b" << hashValue));
        auto chunkRange = intersectingChunk.getRange();
        ASSERT_BSONOBJ_EQ(expectedChunk.getMin(), chunkRange.getMin());
        ASSERT_BSONOBJ_EQ(expectedChunk.getMax(), chunkRange.getMax());
        ASSERT_BSONOBJ_LTE(expectedChunk.getMin(), BSON("a.b" << hashValue));
        ASSERT_BSONOBJ_LT(BSON("a.b" << hashValue), expectedChunk.getMax());
    }
}

TEST_F(ChunkManagerTest, FindIntersectingWithConstantHashedPrefixAndVaryingRangedSuffix) {
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
    auto cri = prepare(BSON("a.b" << "hashed"
                                  << "c.d" << 1),
                       splitPoints);

    checkChunkRangeForDoc("{a: {b: 0}, c: {d: -111}}",
                          cri.getChunkManager(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << BSONNULL),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100));

    checkChunkRangeForDoc("{a: {b: 0}, c: {d: -11}}",
                          cri.getChunkManager(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 0));

    checkChunkRangeForDoc("{a: {b: 0}, c: {d: 0}}",
                          cri.getChunkManager(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 0),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 100));

    checkChunkRangeForDoc("{a: {b: 0}, c: {d: 111}}",
                          cri.getChunkManager(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << 100),
                          BSON("a.b" << MAXKEY << "c.d" << MAXKEY));

    // Missing field will be treated as null and will be targeted to the chunk which holds null,
    // which is shard '1'.
    checkChunkRangeForDoc("{a: {b: 0}}",
                          cri.getChunkManager(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << BSONNULL),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100));


    checkChunkRangeForDoc("{a: {b: 0}, c: 5}",
                          cri.getChunkManager(),
                          BSON("a.b" << hashedValueOfZero << "c.d" << BSONNULL),
                          BSON("a.b" << hashedValueOfZero << "c.d" << -100));
}

// CurrentChunkManager::makeUpdated is a thin wrapper: it forwards a delta of changed chunks (plus
// the collection's timeseries/resharding/allowMigrations/unsplittable attributes) to
// RoutingTableHistory::makeUpdated and rewraps the result. The delta-application semantics
// themselves (dropping overlapped chunks, opening/filling gaps, version handling) live in the
// routing-table layer and are exhaustively covered by routing_table_history_test.cpp. The single
// test below only pins what this wrapper layer can independently break: forwarding the delta,
// rewrapping the resulting version, and carrying the source's own attributes forward rather than
// substituting defaults.
class CurrentChunkManagerUpdateTest : public unittest::Test {
public:
    ChunkVersion chunkVersion(uint32_t major, uint32_t minor) const {
        return ChunkVersion{{_epoch, _collTimestamp}, {major, minor}};
    }

    ChunkType makeChunk(const BSONObj& min,
                        const BSONObj& max,
                        const ChunkVersion& version,
                        const ShardId& shard = kThisShard) const {
        return ChunkType{_collUUID, ChunkRange{min, max}, version, shard};
    }

    BSONObj key(int value) const {
        return BSON("a" << value);
    }

    // Builds a CurrentChunkManager over a gap-allowing routing table, wrapping it the same way
    // CurrentChunkManager::makeUpdated does internally.
    CurrentChunkManager makeCmAllowingGaps(const std::vector<ChunkType>& chunks) const {
        auto rt = RoutingTableHistory::makeNewAllowingGaps(kNss,
                                                           _collUUID,
                                                           _shardKeyPattern,
                                                           false, /* unsplittable */
                                                           nullptr,
                                                           false,
                                                           _epoch,
                                                           _collTimestamp,
                                                           boost::none /* timeseriesFields */,
                                                           boost::none /* reshardingFields */,
                                                           true,
                                                           chunks);
        auto version = rt.getVersion();
        auto handle = RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
        return CurrentChunkManager(std::move(handle));
    }

protected:
    KeyPattern _shardKeyPattern{BSON("a" << 1)};
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
    const UUID _collUUID{UUID::gen()};
};

// The wrapper forwards the delta to the routing-table layer and rewraps the result: the new chunk
// is applied (overlapped old chunks dropped, as proven at the routing-table layer), the resulting
// ChunkManager reports the delta's version, and the source collection's own attributes are carried
// forward rather than replaced by defaults.
TEST_F(CurrentChunkManagerUpdateTest, MakeUpdatedForwardsDeltaAndCarriesAttributes) {
    auto cm = makeCmAllowingGaps({makeChunk(key(0), key(10), chunkVersion(1, 0)),
                                  makeChunk(key(10), key(20), chunkVersion(1, 1)),
                                  makeChunk(key(20), key(30), chunkVersion(1, 2))});

    // [5, 15) overlaps [0, 10) and [10, 20); applying it must reach the routing-table layer.
    auto updated = cm.makeUpdated({makeChunk(key(5), key(15), chunkVersion(2, 0))});

    // Delta was forwarded and applied.
    ASSERT_EQ(updated.numChunks(), 2);  // [5, 15) and the surviving [20, 30)
    // TODO (SERVER-128349): Replace with operation context once keyBelongsToShard performs shard
    // handle resolution.
    ASSERT_TRUE(updated.keyBelongsToShard(nullptr /* opCtx */, key(7), kThisShard));
    ASSERT_FALSE(updated.keyBelongsToShard(nullptr /* opCtx */, key(2), kThisShard));

    // Result is rewrapped at the delta's version.
    ASSERT_EQ(updated.getVersion(), chunkVersion(2, 0));

    // Source attributes are carried forward, not reset to defaults.
    ASSERT_EQ(updated.isUnsplittable(), cm.isUnsplittable());
    ASSERT_EQ(updated.allowMigrations(), cm.allowMigrations());
}

}  // namespace
}  // namespace mongo
