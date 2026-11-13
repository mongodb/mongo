// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    CurrentChunkManager makeCmAllowingGaps(
        const std::vector<ChunkType>& chunks,
        boost::optional<Timestamp> historyFloor = boost::none) const {
        auto rt = RoutingTableHistory::makeNewAllowingGaps(kNss,
                                                           _collUUID,
                                                           _shardKeyPattern,
                                                           false, /* unsplittable */
                                                           nullptr,
                                                           false,
                                                           _epoch,
                                                           _collTimestamp,
                                                           /*timeseriesFields=*/boost::none,
                                                           /*reshardingFields=*/boost::none,
                                                           true,
                                                           chunks,
                                                           std::move(historyFloor));
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
    ASSERT_TRUE(updated.keyBelongsToShard(key(7), kThisShard));
    ASSERT_FALSE(updated.keyBelongsToShard(key(2), kThisShard));

    // Result is rewrapped at the delta's version.
    ASSERT_EQ(updated.getVersion(), chunkVersion(2, 0));

    // Source attributes are carried forward, not reset to defaults.
    ASSERT_EQ(updated.isUnsplittable(), cm.isUnsplittable());
    ASSERT_EQ(updated.allowMigrations(), cm.allowMigrations());
}

TEST_F(CurrentChunkManagerUpdateTest, NearestOwnedChunkOwnedKeyReturnsContainsShardKey) {
    auto cm = makeCmAllowingGaps({makeChunk(key(10), key(20), chunkVersion(1, 0))});
    auto result = cm.nearestOwnedChunk(key(15), kThisShard, ChunkMap::Direction::Forward);
    ASSERT_TRUE(result.containsShardKey);
    ASSERT_TRUE(result.nearestOwnedChunk.has_value());
}

TEST_F(CurrentChunkManagerUpdateTest, NearestOwnedChunkForwardFromLeadingGap) {
    auto cm = makeCmAllowingGaps({makeChunk(key(10), key(20), chunkVersion(1, 0))});
    auto result = cm.nearestOwnedChunk(key(5), kThisShard, ChunkMap::Direction::Forward);
    ASSERT_FALSE(result.containsShardKey);
    ASSERT_TRUE(result.nearestOwnedChunk.has_value());
    ASSERT_BSONOBJ_EQ(result.nearestOwnedChunk->getMin(), key(10));
}

TEST_F(CurrentChunkManagerUpdateTest, NearestOwnedChunkBackwardEqualsLeftChunkBorder) {
    auto cm = makeCmAllowingGaps({makeChunk(key(5), key(20), chunkVersion(1, 0))});
    auto result = cm.nearestOwnedChunk(key(5), kThisShard, ChunkMap::Direction::Backward);
    ASSERT_TRUE(result.containsShardKey);
    ASSERT_TRUE(result.nearestOwnedChunk.has_value());
    ASSERT_BSONOBJ_EQ(result.nearestOwnedChunk->getMin(), key(5));
}

TEST_F(CurrentChunkManagerUpdateTest, NearestOwnedChunkBackwardFromLeadingGap) {
    auto cm = makeCmAllowingGaps({makeChunk(key(10), key(20), chunkVersion(1, 0))});
    auto result = cm.nearestOwnedChunk(key(5), kThisShard, ChunkMap::Direction::Backward);
    ASSERT_FALSE(result.containsShardKey);
    ASSERT_FALSE(result.nearestOwnedChunk.has_value());
}

TEST_F(CurrentChunkManagerUpdateTest, NearestOwnedChunkForwardFromTrailingGap) {
    auto cm = makeCmAllowingGaps({makeChunk(key(0), key(10), chunkVersion(1, 0))});
    auto result = cm.nearestOwnedChunk(key(15), kThisShard, ChunkMap::Direction::Forward);
    ASSERT_FALSE(result.containsShardKey);
    ASSERT_FALSE(result.nearestOwnedChunk.has_value());
}

TEST_F(CurrentChunkManagerUpdateTest, NearestOwnedChunkForwardFromGapBetweenChunks) {
    const ShardId kOtherShard{"otherShard"};
    auto cm = makeCmAllowingGaps({makeChunk(key(0), key(10), chunkVersion(1, 0), kOtherShard),
                                  makeChunk(key(20), key(30), chunkVersion(1, 1))});
    auto result = cm.nearestOwnedChunk(key(15), kThisShard, ChunkMap::Direction::Forward);
    ASSERT_FALSE(result.containsShardKey);
    ASSERT_TRUE(result.nearestOwnedChunk.has_value());
    ASSERT_BSONOBJ_EQ(result.nearestOwnedChunk->getMin(), key(20));
}

TEST_F(CurrentChunkManagerUpdateTest, NearestOwnedChunkBackwardFromGapBetweenChunks) {
    const ShardId kOtherShard{"otherShard"};
    auto cm = makeCmAllowingGaps({makeChunk(key(0), key(10), chunkVersion(1, 0)),
                                  makeChunk(key(20), key(30), chunkVersion(1, 1), kOtherShard)});
    auto result = cm.nearestOwnedChunk(key(15), kThisShard, ChunkMap::Direction::Backward);
    ASSERT_FALSE(result.containsShardKey);
    ASSERT_TRUE(result.nearestOwnedChunk.has_value());
    ASSERT_BSONOBJ_EQ(result.nearestOwnedChunk->getMax(), key(10));
}

// A routing table built by the shard catalog with the expired-history filter enabled carries a
// history floor and may be missing chunks whose ownership history is entirely expired. The tests
// below pin what a point-in-time chunk manager over such a table may and may not answer: chunks
// that are present answer for themselves at any cluster time (their own history is complete, and
// ChunkInfo::getShardIdAt throws if it does not reach back far enough), while a key that matches
// no chunk is unknown below the floor rather than "not owned".
class PointInTimeHistoryFloorTest : public CurrentChunkManagerUpdateTest {
public:
    // A chunk owned by 'shard' since 'onCurrentShardSince', with a single history entry, as the
    // shard catalog stores it.
    ChunkType makeChunkOwnedSince(const BSONObj& min,
                                  const BSONObj& max,
                                  const ChunkVersion& version,
                                  const ShardId& shard,
                                  Timestamp onCurrentShardSince) const {
        auto chunk = makeChunk(min, max, version, shard);
        chunk.setOnCurrentShardSince(onCurrentShardSince);
        chunk.setHistory({ChunkHistory(onCurrentShardSince, shard)});
        return chunk;
    }

    static constexpr auto kFloorSecs = 100;
    const Timestamp kFloor{kFloorSecs, 0};
    const Timestamp kBelowFloor{kFloorSecs - 50, 0};
    const Timestamp kAtFloor = kFloor;
};

// The motivating case: this shard has owned [10, 20) since well before the floor, so its ownership
// at a cluster time below the floor is known from the chunk's own history. The floor must not
// refuse it.
TEST_F(PointInTimeHistoryFloorTest, PresentChunkAnswersBelowFloor) {
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kThisShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    ASSERT_TRUE(pit.keyBelongsToShard(key(15), kThisShard));

    // A key inside a present, owned chunk is answered without any scan.
    auto result = pit.nearestOwnedChunk(key(15), kThisShard, ChunkMap::Direction::Forward);
    ASSERT_TRUE(result.containsShardKey);
    ASSERT_TRUE(result.nearestOwnedChunk.has_value());
    ASSERT_BSONOBJ_EQ(result.nearestOwnedChunk->getMin(), key(10));
}

// Same table, but the chunk's own history does not reach the requested cluster time. The chunk,
// not the floor, is what refuses -- and it does so with the same error.
TEST_F(PointInTimeHistoryFloorTest, PresentChunkWithInsufficientHistoryThrowsBelowFloor) {
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kThisShard, Timestamp(80, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, Timestamp(60, 0));

    ASSERT_THROWS_CODE(pit.keyBelongsToShard(key(15), kThisShard),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

// A key in a gap is unknown below the floor: the chunk covering it may have been dropped by the
// expired-history filter while this shard still owned it at that cluster time.
TEST_F(PointInTimeHistoryFloorTest, GapThrowsBelowFloor) {
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kThisShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    ASSERT_THROWS_CODE(pit.keyBelongsToShard(key(50), kThisShard),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

// At or above the floor every dropped chunk provably moved away no later than the floor, so a gap
// means "not owned" and is answered normally.
TEST_F(PointInTimeHistoryFloorTest, GapIsNotOwnedAtFloor) {
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kThisShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kAtFloor);

    ASSERT_FALSE(pit.keyBelongsToShard(key(50), kThisShard));
    // The floor is exclusive on the scan path too.
    ASSERT_FALSE(
        pit.nearestOwnedChunk(key(50), kThisShard, ChunkMap::Direction::Backward).containsShardKey);
}

// Without a floor (nothing was filtered out of the table) gaps are authoritative at any cluster
// time.
TEST_F(PointInTimeHistoryFloorTest, GapIsNotOwnedWithoutFloor) {
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kThisShard, Timestamp(1, 0))});
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    ASSERT_FALSE(pit.keyBelongsToShard(key(50), kThisShard));
    // Likewise for the scan: a gap key is answered, not refused.
    ASSERT_FALSE(
        pit.nearestOwnedChunk(key(50), kThisShard, ChunkMap::Direction::Backward).containsShardKey);
}

// The floor only constrains point-in-time reads; the current chunk manager over the same table is
// unaffected.
TEST_F(PointInTimeHistoryFloorTest, CurrentChunkManagerIgnoresFloor) {
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kThisShard, Timestamp(1, 0))},
        kFloor);

    ASSERT_FALSE(cm.keyBelongsToShard(key(50), kThisShard));
    ASSERT_TRUE(cm.keyBelongsToShard(key(15), kThisShard));
    ASSERT_FALSE(
        cm.nearestOwnedChunk(key(50), kThisShard, ChunkMap::Direction::Backward).containsShardKey);
    ASSERT_TRUE(
        cm.nearestOwnedChunk(key(15), kThisShard, ChunkMap::Direction::Forward).containsShardKey);
}

// nearestOwnedChunk reports the nearest owned chunk by stepping over everything in between, so
// below the floor it must refuse when the key itself is in a gap...
TEST_F(PointInTimeHistoryFloorTest, NearestOwnedChunkThrowsForGapKeyBelowFloor) {
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kThisShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    ASSERT_THROWS_CODE(pit.nearestOwnedChunk(key(50), kThisShard, ChunkMap::Direction::Forward),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

// ...and when the scan steps across a gap on its way to the nearest owned chunk, since the skipped
// range is not known to be unowned at that cluster time.
TEST_F(PointInTimeHistoryFloorTest, NearestOwnedChunkThrowsWhenScanCrossesGapBelowFloor) {
    const ShardId kOtherShard{"otherShard"};
    // Symmetric around the gap, so each direction has an owned chunk waiting on the far side of it.
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(0), key(10), chunkVersion(1, 0), kThisShard, Timestamp(1, 0)),
         makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 1), kOtherShard, Timestamp(1, 0)),
         // Gap at [20, 30): a chunk dropped by the expired-history filter.
         makeChunkOwnedSince(key(30), key(40), chunkVersion(1, 2), kOtherShard, Timestamp(1, 0)),
         makeChunkOwnedSince(key(40), key(50), chunkVersion(1, 3), kThisShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    for (auto direction : {ChunkMap::Direction::Forward, ChunkMap::Direction::Backward}) {
        // Start on the near side of the gap, in a chunk owned by the other shard.
        const auto startKey = direction == ChunkMap::Direction::Forward ? key(15) : key(35);
        ASSERT_THROWS_CODE(pit.nearestOwnedChunk(startKey, kThisShard, direction),
                           AssertionException,
                           ErrorCodes::StaleChunkHistory);
    }
}

// A scan that never crosses a gap is answered below the floor: the chunks it steps over are all
// present and authoritative.
TEST_F(PointInTimeHistoryFloorTest, NearestOwnedChunkAnswersWhenScanCrossesNoGapBelowFloor) {
    const ShardId kOtherShard{"otherShard"};
    // Contiguous, with an owned chunk on either side of the key's chunk. Stepping over a contiguous
    // neighbour must not be mistaken for a gap, whichever way the scan goes.
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(0), key(10), chunkVersion(1, 0), kThisShard, Timestamp(1, 0)),
         makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 1), kOtherShard, Timestamp(1, 0)),
         makeChunkOwnedSince(key(20), key(30), chunkVersion(1, 2), kOtherShard, Timestamp(1, 0)),
         makeChunkOwnedSince(key(30), key(40), chunkVersion(1, 3), kThisShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    for (auto direction : {ChunkMap::Direction::Forward, ChunkMap::Direction::Backward}) {
        auto result = pit.nearestOwnedChunk(key(15), kThisShard, direction);
        ASSERT_FALSE(result.containsShardKey);
        ASSERT_TRUE(result.nearestOwnedChunk.has_value());
        ASSERT_BSONOBJ_EQ(result.nearestOwnedChunk->getMin(),
                          direction == ChunkMap::Direction::Forward ? key(30) : key(0));
    }
}

// The scan can also run off the end of the table. Below the floor, the range between the last chunk
// it visited and the edge of the key space is not known to be unowned either.
TEST_F(PointInTimeHistoryFloorTest, NearestOwnedChunkThrowsWhenForwardScanRunsIntoTrailingGap) {
    const ShardId kOtherShard{"otherShard"};
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(key(0), key(10), chunkVersion(1, 0), kOtherShard, Timestamp(1, 0)),
         // The table ends at 20: [20, MaxKey) was dropped by the expired-history filter.
         makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 1), kOtherShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    ASSERT_THROWS_CODE(pit.nearestOwnedChunk(key(5), kThisShard, ChunkMap::Direction::Forward),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

TEST_F(PointInTimeHistoryFloorTest, NearestOwnedChunkThrowsWhenBackwardScanRunsIntoLeadingGap) {
    const ShardId kOtherShard{"otherShard"};
    auto cm = makeCmAllowingGaps(
        // The table starts at 10: [MinKey, 10) was dropped by the expired-history filter.
        {makeChunkOwnedSince(key(10), key(20), chunkVersion(1, 0), kOtherShard, Timestamp(1, 0)),
         makeChunkOwnedSince(key(20), key(30), chunkVersion(1, 1), kOtherShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    ASSERT_THROWS_CODE(pit.nearestOwnedChunk(key(25), kThisShard, ChunkMap::Direction::Backward),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

// Control: when the table reaches the edge of the key space there is no gap, so running off the end
// is a definite "no owned chunk in that direction".
TEST_F(PointInTimeHistoryFloorTest,
       NearestOwnedChunkAnswersWhenScanRunsOffContiguousEndBelowFloor) {
    const ShardId kOtherShard{"otherShard"};
    const auto min = BSON("a" << MINKEY);
    const auto max = BSON("a" << MAXKEY);
    auto cm = makeCmAllowingGaps(
        {makeChunkOwnedSince(min, key(10), chunkVersion(1, 0), kOtherShard, Timestamp(1, 0)),
         makeChunkOwnedSince(key(10), max, chunkVersion(1, 1), kOtherShard, Timestamp(1, 0))},
        kFloor);
    auto pit = PointInTimeChunkManager::make(cm, kBelowFloor);

    for (auto direction : {ChunkMap::Direction::Forward, ChunkMap::Direction::Backward}) {
        auto result = pit.nearestOwnedChunk(key(5), kThisShard, direction);
        ASSERT_FALSE(result.containsShardKey);
        ASSERT_FALSE(result.nearestOwnedChunk.has_value());
    }
}

class ChunkMapFindClosestTest : public unittest::Test {
public:
    ChunkVersion chunkVersion(uint32_t major, uint32_t minor) const {
        return ChunkVersion{{_epoch, _collTimestamp}, {major, minor}};
    }

    BSONObj key(int value) const {
        return BSON("a" << value);
    }

    ChunkType makeChunk(const BSONObj& min, const BSONObj& max, const ChunkVersion& version) const {
        return ChunkType{_collUUID, ChunkRange{min, max}, version, kThisShard};
    }

    // Creates a gap-allowing ChunkMap with chunkVectorSize=2, so 4 chunks produce two map entries:
    //   Entry A (key=10): [0,5), [5,10)
    //   Entry B (key=35): [20,25), [30,35)   <- gap at 25-30 within Entry B
    ChunkMap makeMapWithMidVectorGapInSecondEntry() const {
        ChunkMap::ChunkVector chunkInfos;
        for (const auto& c : {makeChunk(key(0), key(5), chunkVersion(1, 0)),
                              makeChunk(key(5), key(10), chunkVersion(1, 1)),
                              makeChunk(key(20), key(25), chunkVersion(1, 2)),
                              makeChunk(key(30), key(35), chunkVersion(1, 3))}) {
            chunkInfos.push_back(std::make_shared<ChunkInfo>(c));
        }
        return ChunkMap{_epoch, _collTimestamp, 2 /*chunkVectorSize*/, true /*allowGaps*/}
            .createMerged(std::move(chunkInfos));
    }

private:
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
    const UUID _collUUID{UUID::gen()};
};

TEST_F(ChunkMapFindClosestTest, BackwardFromMidVectorGapInNonFirstEntry) {
    auto chunkMap = makeMapWithMidVectorGapInSecondEntry();
    auto it = chunkMap.findClosestInDirection(key(27), ChunkMap::Direction::Backward);
    ASSERT(it != chunkMap.end());
    ASSERT_BSONOBJ_EQ((*it)->getRange().getMax(), key(25));
}

TEST_F(ChunkMapFindClosestTest, ForwardFromMidVectorGapInNonFirstEntry) {
    auto chunkMap = makeMapWithMidVectorGapInSecondEntry();
    auto it = chunkMap.findClosestInDirection(key(27), ChunkMap::Direction::Forward);
    ASSERT(it != chunkMap.end());
    ASSERT_BSONOBJ_EQ((*it)->getRange().getMin(), key(30));
}

}  // namespace
}  // namespace mongo
