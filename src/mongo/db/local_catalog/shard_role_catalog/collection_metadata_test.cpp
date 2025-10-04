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

#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/s/resharding/resharding_util.h"
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

    return CollectionMetadata(
        ChunkManager(ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(
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
                                                      allChunks)),
                     kChunkManager),
        kThisShard);
}


struct ConstructedRangeMap : public RangeMap {
    ConstructedRangeMap()
        : RangeMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()) {}
};

class NoChunkFixture : public unittest::Test {
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

TEST_F(NoChunkFixture, GetNextChunk) {
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
class SingleChunkFixture : public unittest::Test {
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

TEST_F(SingleChunkFixture, GetNextChunk) {
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

/**
 * Fixture with single chunk containing:
 * [(min, min)->(max, max))
 */
class SingleChunkMinMaxCompoundKeyFixture : public unittest::Test {
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
class TwoChunksWithGapCompoundKeyFixture : public unittest::Test {
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
class ThreeChunkWithRangeGapFixture : public unittest::Test {
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
class StaleChunkFixture : public unittest::Test {
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
