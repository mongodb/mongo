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

#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
const ShardId kThisShard("thisShard");
const ShardId kOtherShard("otherShard");

CollectionMetadata makeTrackedCollectionMetadataImpl(
    const KeyPattern& shardKeyPattern,
    const std::vector<std::pair<BSONObj, BSONObj>>& thisShardsChunks,
    bool staleChunkManager,
    UUID uuid = UUID::gen(),
    Timestamp timestamp = Timestamp(1, 1),
    boost::optional<TypeCollectionReshardingFields> reshardingFields = boost::none) {

    const OID epoch = OID::gen();

    const Timestamp kOnCurrentShardSince(100, 0);
    const boost::optional<Timestamp> kChunkManager(staleChunkManager, Timestamp{99, 0});

    std::vector<ChunkType> allChunks;
    auto nextMinKey = shardKeyPattern.globalMin();
    ChunkVersion version({epoch, timestamp}, {1, 0});
    for (const auto& myNextChunk : thisShardsChunks) {
        if (SimpleBSONObjComparator::kInstance.evaluate(nextMinKey < myNextChunk.first)) {
            // Need to add a chunk to the other shard from nextMinKey to myNextChunk.first.
            allChunks.emplace_back(
                uuid, ChunkRange{nextMinKey, myNextChunk.first}, version, kOtherShard);
            auto& chunk = allChunks.back();
            chunk.setOnCurrentShardSince(kOnCurrentShardSince);
            chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), chunk.getShard())});

            version.incMajor();
        }
        allChunks.emplace_back(
            uuid, ChunkRange{myNextChunk.first, myNextChunk.second}, version, kThisShard);
        auto& chunk = allChunks.back();
        chunk.setOnCurrentShardSince(kOnCurrentShardSince);
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), chunk.getShard())});

        version.incMajor();
        nextMinKey = myNextChunk.second;
    }

    if (SimpleBSONObjComparator::kInstance.evaluate(nextMinKey < shardKeyPattern.globalMax())) {
        allChunks.emplace_back(
            uuid, ChunkRange{nextMinKey, shardKeyPattern.globalMax()}, version, kOtherShard);
        auto& chunk = allChunks.back();
        chunk.setOnCurrentShardSince(kOnCurrentShardSince);
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), chunk.getShard())});
    }

    auto routingTableHistory = ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(
        RoutingTableHistory::makeNew(kNss,
                                     uuid,
                                     shardKeyPattern,
                                     false, /* unsplittable */
                                     nullptr,
                                     false,
                                     epoch,
                                     timestamp,
                                     boost::none /* timeseriesFields */,
                                     std::move(reshardingFields),
                                     true,
                                     allChunks));

    if (kChunkManager) {
        return CollectionMetadata(
            PointInTimeChunkManager(std::move(routingTableHistory), kChunkManager.get()),
            kThisShard);
    } else {
        return CollectionMetadata(CurrentChunkManager(std::move(routingTableHistory)), kThisShard);
    }
}

ChunkType makeChangedChunkWithVersion(const CollectionMetadata& metadata,
                                      ChunkRange range,
                                      const ShardId& shardId,
                                      ChunkVersion version) {
    ChunkType chunk(metadata.getUUID(), std::move(range), version, shardId);
    chunk.setOnCurrentShardSince(Timestamp(200, 0));
    chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), chunk.getShard())});
    return chunk;
}

ChunkType makeChangedChunk(const CollectionMetadata& metadata,
                           ChunkRange range,
                           const ShardId& shardId) {
    auto version = metadata.getCollPlacementVersion();
    version.incMajor();

    return makeChangedChunkWithVersion(metadata, std::move(range), shardId, version);
}

// Builds metadata whose routing table allows gaps and contains only the given chunks owned by
// kThisShard, mirroring how the shard catalog builds filtering metadata from the chunks a shard
// actually owns (RoutingTableHistory::makeNewAllowingGaps). Unlike
// makeTrackedCollectionMetadataImpl, the unowned ranges are left as real gaps rather than
// back-filled with kOtherShard chunks.
CollectionMetadata makeGappedCollectionMetadata(
    const KeyPattern& shardKeyPattern,
    const std::vector<std::pair<BSONObj, BSONObj>>& ownedChunks) {
    const OID epoch = OID::gen();
    const Timestamp timestamp(1, 1);
    const UUID uuid = UUID::gen();
    const Timestamp kOnCurrentShardSince(100, 0);

    std::vector<ChunkType> chunks;
    ChunkVersion version({epoch, timestamp}, {1, 0});
    for (const auto& range : ownedChunks) {
        chunks.emplace_back(uuid, ChunkRange{range.first, range.second}, version, kThisShard);
        auto& chunk = chunks.back();
        chunk.setOnCurrentShardSince(kOnCurrentShardSince);
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), chunk.getShard())});
        version.incMajor();
    }

    auto routingTableHistory = ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(
        RoutingTableHistory::makeNewAllowingGaps(kNss,
                                                 uuid,
                                                 shardKeyPattern,
                                                 false, /* unsplittable */
                                                 nullptr,
                                                 false,
                                                 epoch,
                                                 timestamp,
                                                 boost::none /* timeseriesFields */,
                                                 boost::none /* reshardingFields */,
                                                 true,
                                                 chunks));
    return CollectionMetadata(CurrentChunkManager(std::move(routingTableHistory)), kThisShard);
}

struct ConstructedRangeMap : public RangeMap {
    ConstructedRangeMap()
        : RangeMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()) {}
};

class CollectionMetadataTestFixture : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = makeOperationContext();
    }

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

class NoChunkFixture : public CollectionMetadataTestFixture {
protected:
    CollectionMetadata makeTrackedCollectionMetadata(
        UUID existingUuid = UUID::gen(),
        UUID reshardingUuid = UUID::gen(),
        CoordinatorStateEnum state = CoordinatorStateEnum::kInitializing) const {

        TypeCollectionReshardingFields reshardingFields{reshardingUuid};
        reshardingFields.setState(state);

        if (state == CoordinatorStateEnum::kCommitting) {
            TypeCollectionRecipientFields recipientFields{
                {kThisShard, kOtherShard}, existingUuid, kNss, 5000};
            reshardingFields.setRecipientFields(std::move(recipientFields));
        } else if (state == CoordinatorStateEnum::kBlockingWrites) {
            TypeCollectionDonorFields donorFields{
                resharding::constructTemporaryReshardingNss(kNss, existingUuid),
                KeyPattern{BSON("newKey" << 1)},
                {kThisShard, kOtherShard}};
            reshardingFields.setDonorFields(std::move(donorFields));
        }

        auto metadataUuid =
            (state >= CoordinatorStateEnum::kCommitting) ? reshardingUuid : existingUuid;

        return makeTrackedCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1)), {}, false, metadataUuid, Timestamp(1, 1), reshardingFields);
    }
};

TEST_F(NoChunkFixture, KeyBelongsToMe) {
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 10)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MINKEY)));

    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSONObj()));
}

TEST_F(NoChunkFixture, IsValidKey) {
    ASSERT(makeTrackedCollectionMetadata().isValidKey(BSON("a" << "abcde")));
    ASSERT(makeTrackedCollectionMetadata().isValidKey(BSON("a" << 3)));
    ASSERT(!makeTrackedCollectionMetadata().isValidKey(BSON("a" << "abcde"
                                                                << "b" << 1)));
    ASSERT(!makeTrackedCollectionMetadata().isValidKey(BSON("c" << "abcde")));
}

TEST_F(NoChunkFixture, getNextChunk) {
    ChunkType nextChunk;
    ASSERT(!makeTrackedCollectionMetadata().getNextChunk(
        makeTrackedCollectionMetadata().getMinKey(), &nextChunk));
}

TEST_F(NoChunkFixture, RangeOverlapsChunk) {
    ASSERT(!makeTrackedCollectionMetadata().rangeOverlapsChunk(
        ChunkRange{BSON("a" << 100), BSON("a" << 200)}));
}

TEST_F(NoChunkFixture, OrphanedDataRangeBegin) {
    auto metadata(makeTrackedCollectionMetadata());

    ConstructedRangeMap pending;
    BSONObj lookupKey = metadata.getMinKey();
    auto keyRange = metadata.getNextOrphanRange(pending, lookupKey);
    ASSERT(keyRange);

    ASSERT(keyRange->getMin().woCompare(metadata.getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(metadata.getMaxKey()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata.getNextOrphanRange(pending, keyRange->getMax()));
}

TEST_F(NoChunkFixture, OrphanedDataRangeMiddle) {
    auto metadata(makeTrackedCollectionMetadata());

    ConstructedRangeMap pending;
    BSONObj lookupKey = BSON("a" << 20);
    auto keyRange = metadata.getNextOrphanRange(pending, lookupKey);
    ASSERT(keyRange);

    ASSERT(keyRange->getMin().woCompare(metadata.getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(metadata.getMaxKey()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata.getNextOrphanRange(pending, keyRange->getMax()));
}

TEST_F(NoChunkFixture, OrphanedDataRangeEnd) {
    auto metadata(makeTrackedCollectionMetadata());

    ConstructedRangeMap pending;
    ASSERT(!metadata.getNextOrphanRange(pending, metadata.getMaxKey()));
}

/**
 * Fixture with single chunk containing:
 * [10->20)
 */
class SingleChunkFixture : public CollectionMetadataTestFixture {
protected:
    CollectionMetadata makeTrackedCollectionMetadata() const {
        return makeTrackedCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1)), {std::make_pair(BSON("a" << 10), BSON("a" << 20))}, false);
    }
};

TEST_F(SingleChunkFixture, KeyBelongsToMe) {
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 10)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 15)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 19)));

    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 0)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 9)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 20)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 1234)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MAXKEY)));

    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSONObj()));
}

TEST_F(SingleChunkFixture, getNextChunk) {
    ChunkType nextChunk;
    ASSERT(makeTrackedCollectionMetadata().getNextChunk(makeTrackedCollectionMetadata().getMinKey(),
                                                        &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 10)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 20)));
}

TEST_F(SingleChunkFixture, GetNextChunkShouldFindNothing) {
    ChunkType nextChunk;
    ASSERT(!makeTrackedCollectionMetadata().getNextChunk(
        makeTrackedCollectionMetadata().getMaxKey(), &nextChunk));
}

TEST_F(SingleChunkFixture, RangeOverlapsChunk) {
    ASSERT(!makeTrackedCollectionMetadata().rangeOverlapsChunk(
        ChunkRange{BSON("a" << 20), BSON("a" << 30)}));
    ASSERT(!makeTrackedCollectionMetadata().rangeOverlapsChunk(
        ChunkRange{BSON("a" << 100), BSON("a" << 200)}));
    ASSERT(!makeTrackedCollectionMetadata().rangeOverlapsChunk(
        ChunkRange{BSON("a" << 0), BSON("a" << 10)}));
    ASSERT(makeTrackedCollectionMetadata().rangeOverlapsChunk(
        ChunkRange{BSON("a" << 11), BSON("a" << 19)}));
    ASSERT(makeTrackedCollectionMetadata().rangeOverlapsChunk(
        ChunkRange{BSON("a" << 19), BSON("a" << 20)}));
}

TEST_F(SingleChunkFixture, ChunkOrphanedDataRanges) {
    ConstructedRangeMap pending;
    auto keyRange = makeTrackedCollectionMetadata().getNextOrphanRange(
        pending, makeTrackedCollectionMetadata().getMinKey());
    ASSERT(keyRange);

    ASSERT(keyRange->getMin().woCompare(makeTrackedCollectionMetadata().getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(BSON("a" << 10)) == 0);

    keyRange = makeTrackedCollectionMetadata().getNextOrphanRange(pending, keyRange->getMax());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(BSON("a" << 20)) == 0);
    ASSERT(keyRange->getMax().woCompare(makeTrackedCollectionMetadata().getMaxKey()) == 0);

    ASSERT(!makeTrackedCollectionMetadata().getNextOrphanRange(pending, keyRange->getMax()));
}

TEST_F(SingleChunkFixture, CurrentChunkManagerMakeUpdatedAppliesChangedChunks) {
    auto metadata = makeTrackedCollectionMetadata();
    auto changedChunk =
        makeChangedChunk(metadata, ChunkRange{BSON("a" << 10), BSON("a" << 20)}, kOtherShard);

    auto updatedChunkManager = metadata.getCurrentChunkManager()->makeUpdated({changedChunk});

    ASSERT_EQ(changedChunk.getVersion(), updatedChunkManager.getVersion());
    ASSERT(!updatedChunkManager.keyBelongsToShard(opCtx(), BSON("a" << 15), kThisShard));
    ASSERT(updatedChunkManager.keyBelongsToShard(opCtx(), BSON("a" << 15), kOtherShard));
}

TEST_F(SingleChunkFixture, CollectionMetadataMakeUpdatedAppliesChangedChunks) {
    auto metadata = makeTrackedCollectionMetadata();
    auto changedChunk =
        makeChangedChunk(metadata, ChunkRange{BSON("a" << 10), BSON("a" << 20)}, kOtherShard);

    auto updatedMetadata = metadata.makeUpdated({changedChunk});

    ASSERT(metadata.keyBelongsToMe(BSON("a" << 15)));
    ASSERT(!updatedMetadata.keyBelongsToMe(BSON("a" << 15)));
    ASSERT_EQ(changedChunk.getVersion(), updatedMetadata.getCollPlacementVersion());
}

TEST_F(SingleChunkFixture, CollectionMetadataMakeUpdatedDonorOfLastChunkReportsZeroVersion) {
    // This shard owns a single chunk. Migrating it away leaves the shard owning no chunks, so
    // its placement version must collapse to {generation, 0, 0} and its max-valid-after must
    // reset, matching the value a shard with no chunks reports.
    auto metadata = makeTrackedCollectionMetadata();
    ASSERT(metadata.keyBelongsToMe(BSON("a" << 15)));

    auto changedChunk =
        makeChangedChunk(metadata, ChunkRange{BSON("a" << 10), BSON("a" << 20)}, kOtherShard);
    auto updatedMetadata = metadata.makeUpdated({changedChunk});

    // The donor no longer owns the migrated key.
    ASSERT(!updatedMetadata.keyBelongsToMe(BSON("a" << 15)));

    // The collection version advances to the migrated chunk's version, while the donor's shard
    // version drops to {0, 0} under the same generation.
    ASSERT_EQ(changedChunk.getVersion(), updatedMetadata.getCollPlacementVersion());
    const auto expectedDonorVersion = ChunkVersion(
        static_cast<CollectionGeneration>(updatedMetadata.getCollPlacementVersion()), {0, 0});
    ASSERT_EQ(expectedDonorVersion, updatedMetadata.getShardPlacementVersion());
    ASSERT_EQ(Timestamp(0, 0), updatedMetadata.getShardMaxValidAfter());
}

TEST_F(SingleChunkFixture, CollectionMetadataMakeUpdatedIsIdempotentForSplitDelta) {
    auto metadata = makeTrackedCollectionMetadata();

    auto version = metadata.getCollPlacementVersion();
    version.incMajor();
    auto splitFirst = makeChangedChunkWithVersion(
        metadata, ChunkRange{BSON("a" << 10), BSON("a" << 15)}, kOtherShard, version);
    version.incMinor();
    auto splitSecond = makeChangedChunkWithVersion(
        metadata, ChunkRange{BSON("a" << 15), BSON("a" << 20)}, kOtherShard, version);

    auto updatedMetadata = metadata.makeUpdated({splitFirst, splitSecond});
    auto reappliedMetadata = updatedMetadata.makeUpdated({splitFirst, splitSecond});

    ASSERT_EQ(splitSecond.getVersion(), reappliedMetadata.getCollPlacementVersion());
    ASSERT_EQ(updatedMetadata.getChunkManager()->numChunks(),
              reappliedMetadata.getChunkManager()->numChunks());
    ASSERT(!reappliedMetadata.keyBelongsToMe(BSON("a" << 12)));
    ASSERT(!reappliedMetadata.keyBelongsToMe(BSON("a" << 17)));
}

TEST_F(SingleChunkFixture, CollectionMetadataMakeUpdatedIgnoresOlderDelta) {
    auto metadata = makeTrackedCollectionMetadata();
    auto firstChangedChunk =
        makeChangedChunk(metadata, ChunkRange{BSON("a" << 10), BSON("a" << 20)}, kOtherShard);
    auto updatedMetadata = metadata.makeUpdated({firstChangedChunk});

    auto newerChangedChunk =
        makeChangedChunk(updatedMetadata, ChunkRange{BSON("a" << 10), BSON("a" << 20)}, kThisShard);
    auto newerMetadata = updatedMetadata.makeUpdated({newerChangedChunk});
    auto reappliedOlderMetadata = newerMetadata.makeUpdated({firstChangedChunk});

    ASSERT_EQ(newerChangedChunk.getVersion(), reappliedOlderMetadata.getCollPlacementVersion());
    ASSERT(reappliedOlderMetadata.keyBelongsToMe(BSON("a" << 15)));
}

TEST_F(SingleChunkFixture, CollectionMetadataMakeUpdatedWithEmptyDeltaIsNoOp) {
    auto metadata = makeTrackedCollectionMetadata();

    auto updatedMetadata = metadata.makeUpdated({});

    // An empty delta carries no version, so the metadata is left untouched.
    ASSERT_EQ(metadata.getCollPlacementVersion(), updatedMetadata.getCollPlacementVersion());
    ASSERT_EQ(metadata.getChunkManager()->numChunks(),
              updatedMetadata.getChunkManager()->numChunks());
    ASSERT(updatedMetadata.keyBelongsToMe(BSON("a" << 15)));
}

TEST_F(SingleChunkFixture, CollectionMetadataMakeUpdatedSelectsMaxVersionRegardlessOfOrder) {
    auto metadata = makeTrackedCollectionMetadata();

    // Build a split delta whose highest-version chunk is listed FIRST. The version guard must
    // compare against the maximum version in the delta, not the last element, so the delta is
    // still recognized as newer and applied.
    auto version = metadata.getCollPlacementVersion();
    version.incMajor();
    auto lowerVersionChunk = makeChangedChunkWithVersion(
        metadata, ChunkRange{BSON("a" << 10), BSON("a" << 15)}, kOtherShard, version);
    version.incMinor();
    auto higherVersionChunk = makeChangedChunkWithVersion(
        metadata, ChunkRange{BSON("a" << 15), BSON("a" << 20)}, kOtherShard, version);

    // Highest-version chunk first, lowest last.
    auto updatedMetadata = metadata.makeUpdated({higherVersionChunk, lowerVersionChunk});

    ASSERT_EQ(higherVersionChunk.getVersion(), updatedMetadata.getCollPlacementVersion());
    ASSERT(!updatedMetadata.keyBelongsToMe(BSON("a" << 12)));
    ASSERT(!updatedMetadata.keyBelongsToMe(BSON("a" << 17)));

    // Re-applying the same delta (again with the max version first) is recognized as already
    // applied and changes nothing.
    auto reappliedMetadata = updatedMetadata.makeUpdated({higherVersionChunk, lowerVersionChunk});
    ASSERT_EQ(higherVersionChunk.getVersion(), reappliedMetadata.getCollPlacementVersion());
    ASSERT_EQ(updatedMetadata.getChunkManager()->numChunks(),
              reappliedMetadata.getChunkManager()->numChunks());
}

class GappedChunksFixture : public CollectionMetadataTestFixture {
protected:
    CollectionMetadata makeGappedMetadata(
        const std::vector<std::pair<BSONObj, BSONObj>>& ownedChunks) const {
        return makeGappedCollectionMetadata(KeyPattern(BSON("a" << 1)), ownedChunks);
    }
};

TEST_F(GappedChunksFixture, MakeUpdatedPreservesGapsWithNonAdjacentChunk) {
    // This shard owns [0, 10) and [30, 40); the ranges [10, 30) and the outer ranges are real gaps
    // in the routing table, as they are on a shard that owns only part of the key space.
    auto metadata =
        makeGappedMetadata({{BSON("a" << 0), BSON("a" << 10)}, {BSON("a" << 30), BSON("a" << 40)}});

    ASSERT_EQ(metadata.getChunkManager()->numChunks(), 2);
    ASSERT(metadata.keyBelongsToMe(BSON("a" << 5)));
    ASSERT(metadata.keyBelongsToMe(BSON("a" << 35)));

    // Receiving a new chunk [50, 60) that is not adjacent to any owned chunk must not be rejected
    // as a gap: makeUpdated tolerates gaps because the routing table was built allowing them.
    auto changedChunk =
        makeChangedChunk(metadata, ChunkRange{BSON("a" << 50), BSON("a" << 60)}, kThisShard);
    auto updatedMetadata = metadata.makeUpdated({changedChunk});

    // The new chunk is applied and the pre-existing gaps are preserved (not back-filled with
    // placeholder chunks).
    ASSERT_EQ(updatedMetadata.getChunkManager()->numChunks(), 3);
    ASSERT(updatedMetadata.keyBelongsToMe(BSON("a" << 5)));
    ASSERT(updatedMetadata.keyBelongsToMe(BSON("a" << 35)));
    ASSERT(updatedMetadata.keyBelongsToMe(BSON("a" << 55)));
    ASSERT_EQ(changedChunk.getVersion(), updatedMetadata.getCollPlacementVersion());
}

/**
 * Fixture with single chunk containing:
 * [(min, min)->(max, max))
 */
class SingleChunkMinMaxCompoundKeyFixture : public CollectionMetadataTestFixture {
protected:
    CollectionMetadata makeTrackedCollectionMetadata() const {
        const KeyPattern shardKeyPattern(BSON("a" << 1 << "b" << 1));
        return makeTrackedCollectionMetadataImpl(
            shardKeyPattern,
            {std::make_pair(shardKeyPattern.globalMin(), shardKeyPattern.globalMax())},
            false);
    }
};

TEST_F(SingleChunkMinMaxCompoundKeyFixture, KeyBelongsToMe) {
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MINKEY << "b" << MINKEY)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MINKEY << "b" << 10)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 10 << "b" << 20)));

    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MAXKEY << "b" << MAXKEY)));

    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSONObj()));
}

/**
 * Fixture with chunks:
 * [(10, 0)->(20, 0)), [(30, 0)->(40, 0))
 */
class TwoChunksWithGapCompoundKeyFixture : public CollectionMetadataTestFixture {
protected:
    CollectionMetadata makeTrackedCollectionMetadata() const {
        return makeTrackedCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1 << "b" << 1)),
            {std::make_pair(BSON("a" << 10 << "b" << 0), BSON("a" << 20 << "b" << 0)),
             std::make_pair(BSON("a" << 30 << "b" << 0), BSON("a" << 40 << "b" << 0))},
            false);
    }
};

TEST_F(TwoChunksWithGapCompoundKeyFixture, ChunkGapOrphanedDataRanges) {
    ConstructedRangeMap pending;

    auto keyRange = makeTrackedCollectionMetadata().getNextOrphanRange(
        pending, makeTrackedCollectionMetadata().getMinKey());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(makeTrackedCollectionMetadata().getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(BSON("a" << 10 << "b" << 0)) == 0);

    keyRange = makeTrackedCollectionMetadata().getNextOrphanRange(pending, keyRange->getMax());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(BSON("a" << 20 << "b" << 0)) == 0);
    ASSERT(keyRange->getMax().woCompare(BSON("a" << 30 << "b" << 0)) == 0);

    keyRange = makeTrackedCollectionMetadata().getNextOrphanRange(pending, keyRange->getMax());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(BSON("a" << 40 << "b" << 0)) == 0);
    ASSERT(keyRange->getMax().woCompare(makeTrackedCollectionMetadata().getMaxKey()) == 0);

    ASSERT(!makeTrackedCollectionMetadata().getNextOrphanRange(pending, keyRange->getMax()));
}

/**
 * Fixture with chunk containing:
 * [min->10) , [10->20) , <gap> , [30->max)
 */
class ThreeChunkWithRangeGapFixture : public CollectionMetadataTestFixture {
protected:
    CollectionMetadata makeTrackedCollectionMetadata() const {
        return makeTrackedCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1)),
            {std::make_pair(BSON("a" << MINKEY), BSON("a" << 10)),
             std::make_pair(BSON("a" << 10), BSON("a" << 20)),
             std::make_pair(BSON("a" << 30), BSON("a" << MAXKEY))},
            false);
    }
};

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkMatch) {
    auto metadata(makeTrackedCollectionMetadata());

    ChunkType chunk;

    ASSERT(metadata.getNextChunk(BSON("a" << MINKEY), &chunk));
    ASSERT_BSONOBJ_EQ(metadata.getMinKey(), chunk.getMin());
    ASSERT_BSONOBJ_EQ(BSON("a" << MINKEY), chunk.getMin());
    ASSERT_BSONOBJ_EQ(BSON("a" << 10), chunk.getMax());

    ASSERT(metadata.getNextChunk(BSON("a" << 10), &chunk));
    ASSERT_BSONOBJ_EQ(BSON("a" << 10), chunk.getMin());
    ASSERT_BSONOBJ_EQ(BSON("a" << 20), chunk.getMax());

    ASSERT(metadata.getNextChunk(BSON("a" << 30), &chunk));
    ASSERT_BSONOBJ_EQ(BSON("a" << 30), chunk.getMin());
    ASSERT_BSONOBJ_EQ(metadata.getMaxKey(), chunk.getMax());
    ASSERT_BSONOBJ_EQ(BSON("a" << MAXKEY), chunk.getMax());
}

TEST_F(ThreeChunkWithRangeGapFixture, KeyBelongsToMe) {
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 5)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 10)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 30)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 40)));
    ASSERT(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << MAXKEY)));

    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 20)));
    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 25)));

    ASSERT(!makeTrackedCollectionMetadata().keyBelongsToMe(BSONObj()));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkFromBeginning) {
    ChunkType nextChunk;
    ASSERT(makeTrackedCollectionMetadata().getNextChunk(makeTrackedCollectionMetadata().getMinKey(),
                                                        &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << MINKEY)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 10)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkFromMiddle) {
    ChunkType nextChunk;
    ASSERT(makeTrackedCollectionMetadata().getNextChunk(BSON("a" << 20), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 30)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << MAXKEY)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkFromLast) {
    ChunkType nextChunk;
    ASSERT(makeTrackedCollectionMetadata().getNextChunk(BSON("a" << 30), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 30)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << MAXKEY)));
}

/**
 * Fixture with single chunk containing:
 * [10->20)
 */
class StaleChunkFixture : public CollectionMetadataTestFixture {
protected:
    CollectionMetadata makeTrackedCollectionMetadata() const {
        return makeTrackedCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1)), {std::make_pair(BSON("a" << 10), BSON("a" << 20))}, true);
    }
};

TEST_F(StaleChunkFixture, KeyBelongsToMe) {
    ASSERT_THROWS_CODE(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 10)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);

    ASSERT_THROWS_CODE(makeTrackedCollectionMetadata().keyBelongsToMe(BSON("a" << 0)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

}  // namespace
}  // namespace mongo
