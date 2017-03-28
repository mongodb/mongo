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
#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

class NoChunkFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        const OID epoch = OID::gen();

        return stdx::make_unique<CollectionMetadata>(
            BSON("a" << 1),
            ChunkVersion(1, 0, epoch),
            ChunkVersion(0, 0, epoch),
            SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>());
    }
};

TEST_F(NoChunkFixture, BasicBelongsToMe) {
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10)));
}

TEST_F(NoChunkFixture, CompoundKeyBelongsToMe) {
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 1 << "b" << 2)));
}

TEST_F(NoChunkFixture, IsKeyValid) {
    ASSERT_TRUE(makeCollectionMetadata()->isValidKey(BSON("a"
                                                          << "abcde")));
    ASSERT_TRUE(makeCollectionMetadata()->isValidKey(BSON("a" << 3)));
    ASSERT_FALSE(makeCollectionMetadata()->isValidKey(BSON("a"
                                                           << "abcde"
                                                           << "b"
                                                           << 1)));
    ASSERT_FALSE(makeCollectionMetadata()->isValidKey(BSON("c"
                                                           << "abcde")));
}

TEST_F(NoChunkFixture, getNextFromEmpty) {
    ChunkType nextChunk;
    ASSERT(
        !makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMinKey(), &nextChunk));
}

TEST_F(NoChunkFixture, getDifferentFromEmpty) {
    ChunkType differentChunk;
    ASSERT(!makeCollectionMetadata()->getDifferentChunk(makeCollectionMetadata()->getMinKey(),
                                                        &differentChunk));
}

TEST_F(NoChunkFixture, NoPendingChunks) {
    ASSERT(!makeCollectionMetadata()->keyIsPending(BSON("a" << 15)));
    ASSERT(!makeCollectionMetadata()->keyIsPending(BSON("a" << 25)));
}

TEST_F(NoChunkFixture, FirstPendingChunk) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));
    ASSERT(cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 10)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 20)));
}

TEST_F(NoChunkFixture, EmptyMultiPendingChunk) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));

    chunk.setMin(BSON("a" << 40));
    chunk.setMax(BSON("a" << 50));

    cloned = cloned->clonePlusPending(chunk);
    ASSERT(cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 45)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 55)));
}

TEST_F(NoChunkFixture, MinusPendingChunk) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));

    cloned = cloned->cloneMinusPending(chunk);
    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 25)));
}

TEST_F(NoChunkFixture, OverlappingPendingChunk) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 30));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));

    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 40));

    cloned = cloned->clonePlusPending(chunk);
    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 35)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 45)));
}

TEST_F(NoChunkFixture, OverlappingPendingChunks) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 30));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));

    chunk.setMin(BSON("a" << 30));
    chunk.setMax(BSON("a" << 50));

    cloned = cloned->clonePlusPending(chunk);

    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 40));

    cloned = cloned->clonePlusPending(chunk);

    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
    ASSERT(cloned->keyIsPending(BSON("a" << 35)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 45)));
}

TEST_F(NoChunkFixture, OrphanedDataRangeBegin) {
    auto metadata(makeCollectionMetadata());

    KeyRange keyRange;
    BSONObj lookupKey = metadata->getMinKey();
    ASSERT(metadata->getNextOrphanRange(lookupKey, &keyRange));

    ASSERT(keyRange.minKey.woCompare(metadata->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(metadata->getMaxKey()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

TEST_F(NoChunkFixture, OrphanedDataRangeMiddle) {
    auto metadata(makeCollectionMetadata());

    KeyRange keyRange;
    BSONObj lookupKey = BSON("a" << 20);
    ASSERT(metadata->getNextOrphanRange(lookupKey, &keyRange));

    ASSERT(keyRange.minKey.woCompare(metadata->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(metadata->getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(metadata->getKeyPattern()) == 0);

    // Make sure we don't have any more ranges
    ASSERT(!metadata->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

TEST_F(NoChunkFixture, OrphanedDataRangeEnd) {
    auto metadata(makeCollectionMetadata());

    KeyRange keyRange;
    ASSERT(!metadata->getNextOrphanRange(metadata->getMaxKey(), &keyRange));
}

TEST_F(NoChunkFixture, PendingOrphanedDataRanges) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 10));
    chunk.setMax(BSON("a" << 20));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));

    KeyRange keyRange;
    ASSERT(cloned->getNextOrphanRange(cloned->getMinKey(), &keyRange));
    ASSERT(keyRange.minKey.woCompare(cloned->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 20)) == 0);
    ASSERT(keyRange.maxKey.woCompare(cloned->getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(!cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

/**
 * Fixture with single chunk containing:
 * [10->20)
 */
class SingleChunkFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        const OID epoch = OID::gen();

        auto shardChunksMap =
            SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>();
        shardChunksMap.emplace(BSON("a" << 10),
                               CachedChunkInfo(BSON("a" << 20), ChunkVersion(1, 0, epoch)));

        return stdx::make_unique<CollectionMetadata>(BSON("a" << 1),
                                                     ChunkVersion(1, 0, epoch),
                                                     ChunkVersion(1, 0, epoch),
                                                     std::move(shardChunksMap));
    }
};

TEST_F(SingleChunkFixture, BasicBelongsToMe) {
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 15)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 19)));
}

TEST_F(SingleChunkFixture, DoesntBelongsToMe) {
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 0)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 9)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 20)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 1234)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MAXKEY)));
}

TEST_F(SingleChunkFixture, CompoudKeyBelongsToMe) {
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 15 << "a" << 14)));
}

TEST_F(SingleChunkFixture, getNextFromEmpty) {
    ChunkType nextChunk;
    ASSERT(
        makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMinKey(), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 10)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 20)));
}

TEST_F(SingleChunkFixture, GetLastChunkIsFalse) {
    ChunkType nextChunk;
    ASSERT(
        !makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMaxKey(), &nextChunk));
}

TEST_F(SingleChunkFixture, getDifferentFromOneIsFalse) {
    ChunkType differentChunk;
    ASSERT(!makeCollectionMetadata()->getDifferentChunk(BSON("a" << 10), &differentChunk));
}

TEST_F(SingleChunkFixture, PlusPendingChunk) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 20));
    chunk.setMax(BSON("a" << 30));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));

    ASSERT(cloned->keyBelongsToMe(BSON("a" << 15)));
    ASSERT(!cloned->keyBelongsToMe(BSON("a" << 25)));
    ASSERT(!cloned->keyIsPending(BSON("a" << 15)));
    ASSERT(cloned->keyIsPending(BSON("a" << 25)));
}

TEST_F(SingleChunkFixture, ChunkOrphanedDataRanges) {
    KeyRange keyRange;
    ASSERT(makeCollectionMetadata()->getNextOrphanRange(makeCollectionMetadata()->getMinKey(),
                                                        &keyRange));
    ASSERT(keyRange.minKey.woCompare(makeCollectionMetadata()->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(makeCollectionMetadata()->getKeyPattern()) == 0);

    ASSERT(makeCollectionMetadata()->getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 20)) == 0);
    ASSERT(keyRange.maxKey.woCompare(makeCollectionMetadata()->getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(makeCollectionMetadata()->getKeyPattern()) == 0);

    ASSERT(!makeCollectionMetadata()->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

/**
 * Fixture with single chunk containing:
 * [(min, min)->(max, max))
 */
class SingleChunkMinMaxCompoundKeyFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        const OID epoch = OID::gen();

        auto shardChunksMap =
            SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>();
        shardChunksMap.emplace(
            BSON("a" << MINKEY << "b" << MINKEY),
            CachedChunkInfo(BSON("a" << MAXKEY << "b" << MAXKEY), ChunkVersion(1, 0, epoch)));

        return stdx::make_unique<CollectionMetadata>(BSON("a" << 1 << "b" << 1),
                                                     ChunkVersion(1, 0, epoch),
                                                     ChunkVersion(1, 0, epoch),
                                                     std::move(shardChunksMap));
    }
};

// Note: no tests for single key belongsToMe because they are not allowed
// if shard key is compound.

TEST_F(SingleChunkMinMaxCompoundKeyFixture, CompoudKeyBelongsToMe) {
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY << "b" << MINKEY)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MAXKEY << "b" << MAXKEY)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MINKEY << "b" << 10)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10 << "b" << 20)));
}

/**
 * Fixture with chunks:
 * [(10, 0)->(20, 0)), [(30, 0)->(40, 0))
 */
class TwoChunksWithGapCompoundKeyFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        const OID epoch = OID::gen();

        auto shardChunksMap =
            SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>();
        shardChunksMap.emplace(
            BSON("a" << 10 << "b" << 0),
            CachedChunkInfo(BSON("a" << 20 << "b" << 0), ChunkVersion(1, 0, epoch)));
        shardChunksMap.emplace(
            BSON("a" << 30 << "b" << 0),
            CachedChunkInfo(BSON("a" << 40 << "b" << 0), ChunkVersion(1, 1, epoch)));

        return stdx::make_unique<CollectionMetadata>(BSON("a" << 1 << "b" << 1),
                                                     ChunkVersion(1, 1, epoch),
                                                     ChunkVersion(1, 1, epoch),
                                                     std::move(shardChunksMap));
    }
};

TEST_F(TwoChunksWithGapCompoundKeyFixture, ChunkGapOrphanedDataRanges) {
    KeyRange keyRange;
    ASSERT(makeCollectionMetadata()->getNextOrphanRange(makeCollectionMetadata()->getMinKey(),
                                                        &keyRange));
    ASSERT(keyRange.minKey.woCompare(makeCollectionMetadata()->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10 << "b" << 0)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(makeCollectionMetadata()->getKeyPattern()) == 0);

    ASSERT(makeCollectionMetadata()->getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 20 << "b" << 0)) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 30 << "b" << 0)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(makeCollectionMetadata()->getKeyPattern()) == 0);

    ASSERT(makeCollectionMetadata()->getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 40 << "b" << 0)) == 0);
    ASSERT(keyRange.maxKey.woCompare(makeCollectionMetadata()->getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(makeCollectionMetadata()->getKeyPattern()) == 0);

    ASSERT(!makeCollectionMetadata()->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

TEST_F(TwoChunksWithGapCompoundKeyFixture, ChunkGapAndPendingOrphanedDataRanges) {
    ChunkType chunk;
    chunk.setMin(BSON("a" << 20 << "b" << 0));
    chunk.setMax(BSON("a" << 30 << "b" << 0));

    auto cloned(makeCollectionMetadata()->clonePlusPending(chunk));

    KeyRange keyRange;
    ASSERT(cloned->getNextOrphanRange(cloned->getMinKey(), &keyRange));
    ASSERT(keyRange.minKey.woCompare(cloned->getMinKey()) == 0);
    ASSERT(keyRange.maxKey.woCompare(BSON("a" << 10 << "b" << 0)) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
    ASSERT(keyRange.minKey.woCompare(BSON("a" << 40 << "b" << 0)) == 0);
    ASSERT(keyRange.maxKey.woCompare(cloned->getMaxKey()) == 0);
    ASSERT(keyRange.keyPattern.woCompare(cloned->getKeyPattern()) == 0);

    ASSERT(!cloned->getNextOrphanRange(keyRange.maxKey, &keyRange));
}

/**
 * Fixture with chunk containing:
 * [min->10) , [10->20) , <gap> , [30->max)
 */
class ThreeChunkWithRangeGapFixture : public unittest::Test {
protected:
    std::unique_ptr<CollectionMetadata> makeCollectionMetadata() const {
        const OID epoch = OID::gen();

        auto shardChunksMap =
            SimpleBSONObjComparator::kInstance.makeBSONObjIndexedMap<CachedChunkInfo>();
        shardChunksMap.emplace(BSON("a" << MINKEY),
                               CachedChunkInfo(BSON("a" << 10), ChunkVersion(1, 1, epoch)));
        shardChunksMap.emplace(BSON("a" << 10),
                               CachedChunkInfo(BSON("a" << 20), ChunkVersion(1, 3, epoch)));
        shardChunksMap.emplace(BSON("a" << 30),
                               CachedChunkInfo(BSON("a" << MAXKEY), ChunkVersion(1, 2, epoch)));

        return stdx::make_unique<CollectionMetadata>(BSON("a" << 1),
                                                     ChunkVersion(1, 3, epoch),
                                                     ChunkVersion(1, 3, epoch),
                                                     std::move(shardChunksMap));
    }
};

TEST_F(ThreeChunkWithRangeGapFixture, ChunkVersionsMatch) {
    auto metadata(makeCollectionMetadata());

    ChunkType chunk;

    ASSERT(metadata->getNextChunk(BSON("a" << MINKEY), &chunk));
    ASSERT_EQ(ChunkVersion(1, 1, metadata->getCollVersion().epoch()), chunk.getVersion());
    ASSERT_BSONOBJ_EQ(metadata->getMinKey(), chunk.getMin());

    ASSERT(metadata->getNextChunk(BSON("a" << 10), &chunk));
    ASSERT_EQ(ChunkVersion(1, 3, metadata->getCollVersion().epoch()), chunk.getVersion());

    ASSERT(metadata->getNextChunk(BSON("a" << 30), &chunk));
    ASSERT_EQ(ChunkVersion(1, 2, metadata->getCollVersion().epoch()), chunk.getVersion());
    ASSERT_BSONOBJ_EQ(metadata->getMaxKey(), chunk.getMax());
}

TEST_F(ThreeChunkWithRangeGapFixture, ShardOwnsDoc) {
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 5)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 10)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 30)));
    ASSERT(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 40)));
}

TEST_F(ThreeChunkWithRangeGapFixture, ShardDoesntOwnDoc) {
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << 25)));
    ASSERT_FALSE(makeCollectionMetadata()->keyBelongsToMe(BSON("a" << MAXKEY)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextFromEmpty) {
    ChunkType nextChunk;
    ASSERT(
        makeCollectionMetadata()->getNextChunk(makeCollectionMetadata()->getMinKey(), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << MINKEY)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << 10)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextFromMiddle) {
    ChunkType nextChunk;
    ASSERT(makeCollectionMetadata()->getNextChunk(BSON("a" << 20), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 30)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << MAXKEY)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetNextFromLast) {
    ChunkType nextChunk;
    ASSERT(makeCollectionMetadata()->getNextChunk(BSON("a" << 30), &nextChunk));
    ASSERT_EQUALS(0, nextChunk.getMin().woCompare(BSON("a" << 30)));
    ASSERT_EQUALS(0, nextChunk.getMax().woCompare(BSON("a" << MAXKEY)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetDifferentFromBeginning) {
    auto metadata(makeCollectionMetadata());

    ChunkType differentChunk;
    ASSERT(metadata->getDifferentChunk(metadata->getMinKey(), &differentChunk));
    ASSERT_BSONOBJ_EQ(BSON("a" << 10), differentChunk.getMin());
    ASSERT_BSONOBJ_EQ(BSON("a" << 20), differentChunk.getMax());
}

TEST_F(ThreeChunkWithRangeGapFixture, GetDifferentFromMiddle) {
    ChunkType differentChunk;
    ASSERT(makeCollectionMetadata()->getDifferentChunk(BSON("a" << 10), &differentChunk));
    ASSERT_EQUALS(0, differentChunk.getMin().woCompare(BSON("a" << MINKEY)));
    ASSERT_EQUALS(0, differentChunk.getMax().woCompare(BSON("a" << 10)));
}

TEST_F(ThreeChunkWithRangeGapFixture, GetDifferentFromLast) {
    ChunkType differentChunk;
    ASSERT(makeCollectionMetadata()->getDifferentChunk(BSON("a" << 30), &differentChunk));
    ASSERT_EQUALS(0, differentChunk.getMin().woCompare(BSON("a" << MINKEY)));
    ASSERT_EQUALS(0, differentChunk.getMax().woCompare(BSON("a" << 10)));
}

}  // namespace
}  // namespace mongo
