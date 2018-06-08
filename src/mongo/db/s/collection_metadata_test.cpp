/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

std::unique_ptr<CollectionMetadata> makeCollectionMetadataImpl(
    const KeyPattern& shardKeyPattern,
    const std::vector<std::pair<BSONObj, BSONObj>>& thisShardsChunks,
    bool staleChunkManager) {

    const OID epoch = OID::gen();
    const NamespaceString kNss("test.foo");
    const ShardId kThisShard("thisShard");
    const ShardId kOtherShard("otherShard");

    const Timestamp kRouting(100, 0);
    const Timestamp kChunkManager(staleChunkManager ? 99 : 100, 0);

    std::vector<ChunkType> allChunks;
    auto nextMinKey = shardKeyPattern.globalMin();
    ChunkVersion version{1, 0, epoch};
    for (const auto& myNextChunk : thisShardsChunks) {
        if (SimpleBSONObjComparator::kInstance.evaluate(nextMinKey < myNextChunk.first)) {
            // Need to add a chunk to the other shard from nextMinKey to myNextChunk.first.
            allChunks.emplace_back(
                kNss, ChunkRange{nextMinKey, myNextChunk.first}, version, kOtherShard);
            allChunks.back().setHistory({ChunkHistory(kRouting, kOtherShard)});
            version.incMajor();
        }
        allChunks.emplace_back(
            kNss, ChunkRange{myNextChunk.first, myNextChunk.second}, version, kThisShard);
        allChunks.back().setHistory({ChunkHistory(kRouting, kThisShard)});
        version.incMajor();
        nextMinKey = myNextChunk.second;
    }
    if (SimpleBSONObjComparator::kInstance.evaluate(nextMinKey < shardKeyPattern.globalMax())) {
        allChunks.emplace_back(
            kNss, ChunkRange{nextMinKey, shardKeyPattern.globalMax()}, version, kOtherShard);
        allChunks.back().setHistory({ChunkHistory(kRouting, kOtherShard)});
    }

    UUID uuid(UUID::gen());
    auto rt =
        RoutingTableHistory::makeNew(kNss, uuid, shardKeyPattern, nullptr, false, epoch, allChunks);
    std::shared_ptr<ChunkManager> cm = std::make_shared<ChunkManager>(rt, kChunkManager);
    return stdx::make_unique<CollectionMetadata>(cm, kThisShard);
}

struct ConstructedRangeMap : public RangeMap {
    ConstructedRangeMap()
        : RangeMap(SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<BSONObj>()) {}
};

class NoChunkFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        return makeCollectionMetadataImpl(KeyPattern(BSON("a" << 1)), {}, false);
    }
};

TEST_F(NoChunkFixture, KeyBelongsToMe) {
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY)));

    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSONObj()));
}

TEST_F(NoChunkFixture, IsValidKey) {
    ASSERT(makeCollectionMetadata()->isValidKey(BSON("a"
                                                     << "abcde")));
    ASSERT(makeCollectionMetadata()->isValidKey(BSON("a" << 3)));
    ASSERT(!makeCollectionMetadata()->isValidKey(BSON("a"
                                                      << "abcde"
                                                      << "b"
                                                      << 1)));
    ASSERT(!makeCollectionMetadata()->isValidKey(BSON("c"
                                                      << "abcde")));
}

TEST_F(NoChunkFixture, GetNextChunk) {
    ChunkType nextChunk;
    ASSERT(
        !makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMinKey(), &nextChunk));
}

TEST_F(NoChunkFixture, RangeOverlapsChunk) {
    ASSERT(!makeCollectionMetadata()->rangeOverlapsChunk(
        ChunkRange{BSON("a" << 100), BSON("a" << 200)}));
}

TEST_F(NoChunkFixture, OrphanedDataRangeBegin) {
    auto metadata(makeCollectionMetadata());

    ConstructedRangeMap pending;
    BSONObj lookupKey = metadata->getMinKey();
    auto keyRange = metadata->getNextOrphanRange(pending, lookupKey);
    ASSERT(keyRange);

    ASSERT(keyRange->getMin().woCompare(metadata->getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(metadata->getMaxKey()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata->getNextOrphanRange(pending, keyRange->getMax()));
}

TEST_F(NoChunkFixture, OrphanedDataRangeMiddle) {
    auto metadata(makeCollectionMetadata());

    ConstructedRangeMap pending;
    BSONObj lookupKey = BSON("a" << 20);
    auto keyRange = metadata->getNextOrphanRange(pending, lookupKey);
    ASSERT(keyRange);

    ASSERT(keyRange->getMin().woCompare(metadata->getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(metadata->getMaxKey()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata->getNextOrphanRange(pending, keyRange->getMax()));
}

TEST_F(NoChunkFixture, OrphanedDataRangeEnd) {
    auto metadata(makeCollectionMetadata());

    ConstructedRangeMap pending;
    ASSERT(!metadata->getNextOrphanRange(pending, metadata->getMaxKey()));
}

/**
 * Fixture with single chunk containing:
 * [10->20)
 */
class SingleChunkFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        return makeCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1)), {std::make_pair(BSON("a" << 10), BSON("a" << 20))}, false);
    }
};

TEST_F(SingleChunkFixture, KeyBelongsToMe) {
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 15)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 19)));

    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 0)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 9)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 20)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 1234)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MAXKEY)));

    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSONObj()));
}

TEST_F(SingleChunkFixture, GetNextChunk) {
    ChunkType nextChunk;
    ASSERT(
        makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMinKey(), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 10)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 20)));
}

TEST_F(SingleChunkFixture, GetNextChunkShouldFindNothing) {
    ChunkType nextChunk;
    ASSERT(
        !makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMaxKey(), &nextChunk));
}

TEST_F(SingleChunkFixture, RangeOverlapsChunk) {
    ASSERT(!makeCollectionMetadata()->rangeOverlapsChunk(
        ChunkRange{BSON("a" << 20), BSON("a" << 30)}));
    ASSERT(!makeCollectionMetadata()->rangeOverlapsChunk(
        ChunkRange{BSON("a" << 100), BSON("a" << 200)}));
    ASSERT(
        !makeCollectionMetadata()->rangeOverlapsChunk(ChunkRange{BSON("a" << 0), BSON("a" << 10)}));
    ASSERT(
        makeCollectionMetadata()->rangeOverlapsChunk(ChunkRange{BSON("a" << 11), BSON("a" << 19)}));
    ASSERT(
        makeCollectionMetadata()->rangeOverlapsChunk(ChunkRange{BSON("a" << 19), BSON("a" << 20)}));
}

TEST_F(SingleChunkFixture, ChunkOrphanedDataRanges) {
    ConstructedRangeMap pending;
    auto keyRange = makeCollectionMetadata()->getNextOrphanRange(
        pending, makeCollectionMetadata()->getMinKey());
    ASSERT(keyRange);

    ASSERT(keyRange->getMin().woCompare(makeCollectionMetadata()->getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(BSON("a" << 10)) == 0);

    keyRange = makeCollectionMetadata()->getNextOrphanRange(pending, keyRange->getMax());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(BSON("a" << 20)) == 0);
    ASSERT(keyRange->getMax().woCompare(makeCollectionMetadata()->getMaxKey()) == 0);

    ASSERT(!makeCollectionMetadata()->getNextOrphanRange(pending, keyRange->getMax()));
}

/**
 * Fixture with single chunk containing:
 * [(min, min)->(max, max))
 */
class SingleChunkMinMaxCompoundKeyFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        const KeyPattern shardKeyPattern(BSON("a" << 1 << "b" << 1));
        return makeCollectionMetadataImpl(
            shardKeyPattern,
            {std::make_pair(shardKeyPattern.globalMin(), shardKeyPattern.globalMax())},
            false);
    }
};

TEST_F(SingleChunkMinMaxCompoundKeyFixture, KeyBelongsToMe) {
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY << "b" << MINKEY)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY << "b" << 10)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10 << "b" << 20)));

    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MAXKEY << "b" << MAXKEY)));

    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSONObj()));
}

/**
 * Fixture with chunks:
 * [(10, 0)->(20, 0)), [(30, 0)->(40, 0))
 */
class TwoChunksWithGapCompoundKeyFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        return makeCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1 << "b" << 1)),
            {std::make_pair(BSON("a" << 10 << "b" << 0), BSON("a" << 20 << "b" << 0)),
             std::make_pair(BSON("a" << 30 << "b" << 0), BSON("a" << 40 << "b" << 0))},
            false);
    }
};

TEST_F(TwoChunksWithGapCompoundKeyFixture, ChunkGapOrphanedDataRanges) {
    ConstructedRangeMap pending;

    auto keyRange = makeCollectionMetadata()->getNextOrphanRange(
        pending, makeCollectionMetadata()->getMinKey());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(makeCollectionMetadata()->getMinKey()) == 0);
    ASSERT(keyRange->getMax().woCompare(BSON("a" << 10 << "b" << 0)) == 0);

    keyRange = makeCollectionMetadata()->getNextOrphanRange(pending, keyRange->getMax());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(BSON("a" << 20 << "b" << 0)) == 0);
    ASSERT(keyRange->getMax().woCompare(BSON("a" << 30 << "b" << 0)) == 0);

    keyRange = makeCollectionMetadata()->getNextOrphanRange(pending, keyRange->getMax());
    ASSERT(keyRange);
    ASSERT(keyRange->getMin().woCompare(BSON("a" << 40 << "b" << 0)) == 0);
    ASSERT(keyRange->getMax().woCompare(makeCollectionMetadata()->getMaxKey()) == 0);

    ASSERT(!makeCollectionMetadata()->getNextOrphanRange(pending, keyRange->getMax()));
}

/**
 * Fixture with chunk containing:
 * [min->10) , [10->20) , <gap> , [30->max)
 */
class ThreeChunkWithRangeGapFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {

        return makeCollectionMetadataImpl(KeyPattern(BSON("a" << 1)),
                                          {std::make_pair(BSON("a" << MINKEY), BSON("a" << 10)),
                                           std::make_pair(BSON("a" << 10), BSON("a" << 20)),
                                           std::make_pair(BSON("a" << 30), BSON("a" << MAXKEY))},
                                          false);
    }
};

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkMatch) {
    auto metadata(makeCollectionMetadata());

    ChunkType chunk;

    ASSERT(metadata->getNextChunk(BSON("a" << MINKEY), &chunk));
    ASSERT_BSONOBJ_EQ(metadata->getMinKey(), chunk.getMin());
    ASSERT_BSONOBJ_EQ(BSON("a" << MINKEY), chunk.getMin());
    ASSERT_BSONOBJ_EQ(BSON("a" << 10), chunk.getMax());

    ASSERT(metadata->getNextChunk(BSON("a" << 10), &chunk));
    ASSERT_BSONOBJ_EQ(BSON("a" << 10), chunk.getMin());
    ASSERT_BSONOBJ_EQ(BSON("a" << 20), chunk.getMax());

    ASSERT(metadata->getNextChunk(BSON("a" << 30), &chunk));
    ASSERT_BSONOBJ_EQ(BSON("a" << 30), chunk.getMin());
    ASSERT_BSONOBJ_EQ(metadata->getMaxKey(), chunk.getMax());
    ASSERT_BSONOBJ_EQ(BSON("a" << MAXKEY), chunk.getMax());
}

TEST_F(ThreeChunkWithRangeGapFixture, KeyBelongsToMe) {
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 5)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 30)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 40)));

    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 20)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 25)));
    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MAXKEY)));

    ASSERT(!makeCollectionMetadata()->keyBelongsToMe(BSONObj()));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkFromBeginning) {
    ChunkType nextChunk;
    ASSERT(
        makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMinKey(), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << MINKEY)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 10)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkFromMiddle) {
    ChunkType nextChunk;
    ASSERT(makeCollectionMetadata()->getNextChunk(BSON("a" << 20), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 30)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << MAXKEY)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextChunkFromLast) {
    ChunkType nextChunk;
    ASSERT(makeCollectionMetadata()->getNextChunk(BSON("a" << 30), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 30)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << MAXKEY)));
}

/**
 * Fixture with single chunk containing:
 * [10->20)
 */
class StaleChunkFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        return makeCollectionMetadataImpl(
            KeyPattern(BSON("a" << 1)), {std::make_pair(BSON("a" << 10), BSON("a" << 20))}, true);
    }
};

TEST_F(StaleChunkFixture, KeyBelongsToMe) {
    ASSERT_THROWS_CODE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);

    ASSERT_THROWS_CODE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 0)),
                       AssertionException,
                       ErrorCodes::StaleChunkHistory);
}

}  // namespace
}  // namespace mongo
