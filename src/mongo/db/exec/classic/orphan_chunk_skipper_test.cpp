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

#include "mongo/db/exec/classic/orphan_chunk_skipper.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>

namespace mongo {
namespace {

/**
 * A mock that implements a ShardFilterer suitable for unittesting orphan chunk skipping.
 */
class MockNearestOwnedChunkFilter final : public ShardFilterer {
public:
    MockNearestOwnedChunkFilter(KeyPattern pattern, ChunkManager::ChunkOwnership chunkOwnership)
        : _pattern(pattern), _chunkOwnership(chunkOwnership) {}

    std::unique_ptr<ShardFilterer> clone() const override {
        return std::make_unique<MockNearestOwnedChunkFilter>(_pattern.toBSON(), _chunkOwnership);
    }

    bool keyBelongsToMe(const BSONObj& key) const override {
        return _chunkOwnership.containsShardKey;
    }

    DocumentBelongsResult documentBelongsToMe(const BSONObj& doc) const override {
        return keyBelongsToMe(doc) ? DocumentBelongsResult::kBelongs
                                   : DocumentBelongsResult::kDoesNotBelong;
    }

    ChunkManager::ChunkOwnership nearestOwnedChunk(const BSONObj& key,
                                                   ChunkMap::Direction direction) const override {
        return _chunkOwnership;
    }

    const KeyPattern& getKeyPattern() const override {
        return _pattern;
    }

    bool isCollectionSharded() const override {
        return true;
    }

    size_t getApproximateSize() const override {
        auto size = sizeof(MockNearestOwnedChunkFilter);
        size += _pattern.getApproximateSize() - sizeof(KeyPattern);
        return size;
    }

private:
    const KeyPattern _pattern;
    const ChunkManager::ChunkOwnership _chunkOwnership;
};

const ShardId kThisShard("testShard");

class OrphanChunkSkipperTest : public unittest::Test {
public:
    static boost::optional<OrphanChunkSkipper> getOrphanChunkSkipper(
        const ShardFilterer& mockFilter,
        const BSONObj& shardKeyBSON,
        const BSONObj& keyPattern,
        int scanDirection) {
        ShardKeyPattern shardKey(shardKeyBSON.copy());
        KeyPattern skp = shardKey.getKeyPattern();
        return OrphanChunkSkipper::tryMakeChunkSkipper(
            mockFilter, shardKey, keyPattern, scanDirection);
    }

    static OrphanChunkSkipper::ShardKeyMask getBitsetFromString(StringData expectedBitsetStr) {
        ShardKeyMaskBitset bits;
        int lastShardKeyPos = 0;
        for (size_t i = 0; i < expectedBitsetStr.size(); i++) {
            if (expectedBitsetStr[i] == '1') {
                bits.set(i);
                lastShardKeyPos = i;
            }
        }
        return {.bits = bits, .lastShardKeyPos = lastShardKeyPos};
    }

    ChunkInfo makeChunkInfo(BSONObj min, BSONObj max) const {
        ChunkVersion version({_epoch, _collTimestamp}, {1, 0});
        return ChunkInfo(ChunkType{_uuid, ChunkRange{min, max}, version, kThisShard});
    }

    struct OrphanChunkSkipperTestParams {
        BSONObj shardKey;
        BSONObj keyPattern;
        int scanDir;
        int expectedScanDir;
        StringData expectedBitset;
    };

private:
    const UUID _uuid = UUID::gen();
    const OID _epoch{OID::gen()};
    const Timestamp _collTimestamp{1, 1};
};

// Note: we use BSONObj here in the "expected" case for ease of test-writing.
void assertKeySuffixesAreEqual(const std::vector<BSONElement>& actualSuffix,
                               const BSONObj& expectedSuffix,
                               size_t skipPrefixElems) {
    auto actualIt = actualSuffix.begin() + skipPrefixElems;
    auto expectedIt = expectedSuffix.begin();
    while (actualIt != actualSuffix.end() && expectedIt != expectedSuffix.end()) {
        ASSERT_BSONELT_EQ(*actualIt, *expectedIt);
        actualIt++;
        expectedIt++;
    }
    ASSERT_EQ(actualIt, actualSuffix.end());
    ASSERT_EQ(expectedIt, expectedSuffix.end());
}


}  // namespace

// Using macros here to preserve line numbers in unit test failure.
#define ASSERT_CHUNK_SKIPPER_MATCHES(                                                          \
    shardKey, keyPattern, scanDir, expectedScanDir, expectedBitset)                            \
    {                                                                                          \
        MockNearestOwnedChunkFilter shardFilter(keyPattern, {});                               \
        auto chunkSkipper = getOrphanChunkSkipper(shardFilter, shardKey, keyPattern, scanDir); \
        ASSERT_CHUNK_SKIPPER(chunkSkipper, expectedScanDir, expectedBitset);                   \
    }

#define ASSERT_CHUNK_SKIPPER(chunkSkipper, expectedScanDir, expectedBitset)                       \
    ASSERT_TRUE(chunkSkipper);                                                                    \
    ASSERT_EQ(expectedScanDir > 0 ? ChunkMap::Direction::Forward : ChunkMap::Direction::Backward, \
              chunkSkipper->getChunkMapScanDir());                                                \
    ASSERT_EQ(getBitsetFromString(expectedBitset), chunkSkipper->getShardKeyMask());

#define ASSERT_NO_CHUNK_SKIPPER(shardKey, keyPattern, scanDir)                           \
    {                                                                                    \
        MockNearestOwnedChunkFilter shardFilter(keyPattern, {});                         \
        ASSERT_FALSE(getOrphanChunkSkipper(shardFilter, shardKey, keyPattern, scanDir)); \
    }

#define ASSERT_NO_NEXT_SEEK_POINT(chunkSkipper, currentKey, expectedInfo)                    \
    {                                                                                        \
        IndexSeekPoint seekPoint;                                                            \
        ASSERT_EQ(chunkSkipper->makeSeekPointIfOrphan(currentKey, seekPoint), expectedInfo); \
    }
#define ASSERT_EXPECTED_SEEK_POINT(chunkSkipper,                                                \
                                   currentKey,                                                  \
                                   expectedKeyPrefix,                                           \
                                   expectedPrefixLen,                                           \
                                   expectedKeySuffix,                                           \
                                   expectedFirstExclusive)                                      \
    {                                                                                           \
        IndexSeekPoint seekPoint;                                                               \
        ASSERT_EQ(chunkSkipper->makeSeekPointIfOrphan(currentKey, seekPoint),                   \
                  OrphanChunkSkipper::Info::CanSkipOrphans);                                    \
        ASSERT_BSONOBJ_EQ(seekPoint.keyPrefix, expectedKeyPrefix);                              \
        ASSERT_EQ(seekPoint.prefixLen, expectedPrefixLen);                                      \
        assertKeySuffixesAreEqual(seekPoint.keySuffix, expectedKeySuffix, seekPoint.prefixLen); \
        ASSERT_EQ(seekPoint.firstExclusive, expectedFirstExclusive);                            \
    }
#define ASSERT_EXPECTED_SEEK_POINT_EXCLUSIVE(                       \
    chunkSkipper, currentKey, expectedPrefixLen, expectedKeySuffix) \
    ASSERT_EXPECTED_SEEK_POINT(                                     \
        chunkSkipper, currentKey, currentKey, expectedPrefixLen, expectedKeySuffix, -1)
#define ASSERT_EXPECTED_SEEK_POINT_INCLUSIVE(                       \
    chunkSkipper, currentKey, expectedPrefixLen, expectedKeySuffix) \
    ASSERT_EXPECTED_SEEK_POINT(chunkSkipper,                        \
                               currentKey,                          \
                               currentKey,                          \
                               expectedPrefixLen,                   \
                               expectedKeySuffix,                   \
                               chunkSkipper->getShardKeyMask().lastShardKeyPos)

TEST_F(OrphanChunkSkipperTest, TestFailingToMakeChunkSkipper) {
    // Validate that we can't generate a chunk skipper when shard keys:
    //  - don't appear in the index key pattern,
    //  - are in a different order in the index key pattern, or
    //  - are sorted in an incompatible way in the index.
    ASSERT_NO_CHUNK_SKIPPER(
        BSON("b" << 1) /* shardKey */, BSON("a" << 1) /* keyPattern */, 1 /* scanDir */);
    ASSERT_NO_CHUNK_SKIPPER(BSON("b" << 1 << "a" << 1) /* shardKey */,
                            BSON("a" << 1 << "b" << 1) /* keyPattern */,
                            1 /* scanDir */);
    ASSERT_NO_CHUNK_SKIPPER(BSON("b" << 1 << "a" << 1), BSON("a" << 1 << "b" << 1 << "c" << 1), 1);
    ASSERT_NO_CHUNK_SKIPPER(BSON("a" << 1 << "b" << 1), BSON("a" << 1 << "b" << -1 << "c" << 1), 1);
    ASSERT_NO_CHUNK_SKIPPER(
        BSON("a" << 1 << "b" << 1 << "c" << 1), BSON("a" << -1 << "b" << -1 << "c" << 1), -1);
    // Ensure we can't generate a chunk skipper in the non-contiguous shard key case.
    ASSERT_NO_CHUNK_SKIPPER(BSON("a" << 1 << "c" << 1), BSON("a" << 1 << "b" << 1 << "c" << 1), -1);
    ASSERT_NO_CHUNK_SKIPPER(
        BSON("a" << 1 << "d" << 1), BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1), 1);
    ASSERT_NO_CHUNK_SKIPPER(
        BSON("b" << 1 << "d" << 1), BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1), 1);
}

TEST_F(OrphanChunkSkipperTest, SingleShardKeyChunkSkipperConstruction) {
    // Validate the non-compound shard-key case.
    auto shardKey = BSON("a" << 1);
    {
        auto expectedBitSet = "1"_sd;
        ASSERT_CHUNK_SKIPPER_MATCHES(
            shardKey, shardKey, 1 /* scanDir */, 1 /* expectedScanDir*/, expectedBitSet);
        ASSERT_CHUNK_SKIPPER_MATCHES(
            shardKey, shardKey, -1 /* scanDir */, -1 /* expectedScanDir*/, expectedBitSet);
        ASSERT_CHUNK_SKIPPER_MATCHES(
            shardKey, BSON("a" << -1), -1 /* scanDir */, 1 /* expectedScanDir*/, expectedBitSet);
        ASSERT_CHUNK_SKIPPER_MATCHES(
            shardKey, BSON("a" << -1), 1 /* scanDir */, -1 /* expectedScanDir*/, expectedBitSet);
    }

    ASSERT_CHUNK_SKIPPER_MATCHES(shardKey,
                                 BSON("a" << -1 << "b" << 1 << "c" << -1) /* indexKeyPattern */,
                                 -1 /* scanDir */,
                                 1 /* expectedScanDir*/,
                                 "100"_sd /* expectedBitSet */);
    ASSERT_CHUNK_SKIPPER_MATCHES(shardKey,
                                 BSON("a" << -1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                 1 /* scanDir */,
                                 -1 /* expectedScanDir*/,
                                 "100"_sd /* expectedBitSet */);
    ASSERT_CHUNK_SKIPPER_MATCHES(shardKey,
                                 BSON("b" << -1 << "a" << 1 << "c" << 1) /* indexKeyPattern */,
                                 1 /* scanDir */,
                                 1 /* expectedScanDir*/,
                                 "010"_sd /* expectedBitSet */);
    ASSERT_CHUNK_SKIPPER_MATCHES(shardKey,
                                 BSON("b" << -1 << "c" << 1 << "a" << 1) /* indexKeyPattern */,
                                 -1 /* scanDir */,
                                 -1 /* expectedScanDir*/,
                                 "001"_sd /* expectedBitSet */);
}

TEST_F(OrphanChunkSkipperTest, CompoundShardKeyChunkSkipperConstruction) {
    // Validate the compound shard-key case.
    auto shardKey = BSON("a" << 1 << "b" << 1);
    ASSERT_CHUNK_SKIPPER_MATCHES(shardKey,
                                 BSON("a" << 1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                 -1 /* scanDir */,
                                 -1 /* expectedScanDir*/,
                                 "110"_sd /* expectedBitSet */);
    ASSERT_CHUNK_SKIPPER_MATCHES(shardKey,
                                 BSON("c" << 1 << "a" << -1 << "b" << -1) /* indexKeyPattern */,
                                 -1 /* scanDir */,
                                 1 /* expectedScanDir*/,
                                 "011"_sd /* expectedBitSet */);
    ASSERT_CHUNK_SKIPPER_MATCHES(BSON("a" << 1 << "b" << 1 << "c" << 1) /* shardKey */,
                                 BSON("a" << -1 << "b" << -1 << "c" << -1) /* indexKeyPattern */,
                                 1 /* scanDir */,
                                 -1 /* expectedScanDir*/,
                                 "111"_sd /* expectedBitSet */);
}

TEST_F(OrphanChunkSkipperTest, MakeSeekPointSingleKey) {
    auto shardKey = BSON("a" << 1);
    // Ensure we correcly flag an owned value.
    {
        // Mock a chunk such that the input value is not an orphan.
        auto nearestInfo = makeChunkInfo(BSON("a" << 10), BSON("a" << 20));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = true, .nearestOwnedChunk = nearest});

        auto cs = getOrphanChunkSkipper(
            shardFilter, shardKey, BSON("a" << -1) /* indexKeyPattern */, -1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, 1 /* expectedScanDirection */, "1"_sd /* expectedBitSet */);
        ASSERT_NO_NEXT_SEEK_POINT(
            cs, BSON("a" << 0) /* currentShardKeyValue */, OrphanChunkSkipper::NotOrphan);
    }

    // Ensure that we generate the expected seek point for the specified chunk ownership.
    {
        // Mock a nearest chunk.
        auto nearestInfo = makeChunkInfo(BSON("a" << 10), BSON("a" << 20));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs = getOrphanChunkSkipper(
            shardFilter, shardKey, BSON("a" << 1) /* indexKeyPattern */, 1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, 1 /* expectedScanDirection */, "1"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_EXCLUSIVE(cs,
                                             BSON("a" << 0) /* currentShardKeyValue */,
                                             0 /* expectedPrefixLen */,
                                             BSON("a" << 10) /* expectedKeySuffix */);
    }
    {
        // Mock a nearest chunk.
        auto nearestInfo = makeChunkInfo(BSON("a" << -20), BSON("a" << -10));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs = getOrphanChunkSkipper(
            shardFilter, shardKey, BSON("a" << -1) /* indexKeyPattern */, 1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, -1 /* expectedScanDirection */, "1"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_INCLUSIVE(cs,
                                             BSON("a" << 0) /* currentShardKeyValue */,
                                             0 /* expectedPrefixLen */,
                                             BSON("a" << -10) /* expectedKeySuffix */);
    }

    // Ensure that no seek point is generated if nearestOwnedChunk() says we're out of chunks.
    // Since the shard key is the index, we should return 'NoMoreOwned' (i.e. we could end the scan
    // here).
    {
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = boost::none});

        auto cs = getOrphanChunkSkipper(
            shardFilter, shardKey, BSON("a" << -1) /* indexKeyPattern */, 1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, -1 /* expectedScanDirection */, "1"_sd /* expectedBitSet */);
        ASSERT_NO_NEXT_SEEK_POINT(
            cs, BSON("a" << 0) /* currentShardKeyValue */, OrphanChunkSkipper::NoMoreOwned);
    }
    {
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = boost::none});

        auto cs = getOrphanChunkSkipper(
            shardFilter, shardKey, BSON("a" << -1) /* indexKeyPattern */, -1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, 1 /* expectedScanDirection */, "1"_sd /* expectedBitSet */);
        ASSERT_NO_NEXT_SEEK_POINT(
            cs, BSON("a" << 0) /* currentShardKeyValue */, OrphanChunkSkipper::NoMoreOwned);
    }
}

TEST_F(OrphanChunkSkipperTest, MakeSeekPointSingleShardKeyCompoundIndexForwardScan) {
    // Ensure we correcly flag an owned value.
    {
        // Mock a chunk such that the input value is not an orphan.
        auto shardKey = BSON("b" << 1);
        auto nearestInfo = makeChunkInfo(BSON("b" << "zap"), BSON("b" << "zoo"));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = true, .nearestOwnedChunk = nearest});

        auto cs = getOrphanChunkSkipper(
            shardFilter, shardKey, BSON("b" << 1) /* indexKeyPattern */, 1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, 1 /* expectedScanDirection */, "1"_sd /* expectedBitSet */);
        ASSERT_NO_NEXT_SEEK_POINT(
            cs, BSON("b" << "zed") /* currentShardKeyValue */, OrphanChunkSkipper::NotOrphan);
    }

    // Forward seek to next chunk boundary.
    {
        // Mock a nearest chunk.
        auto shardKey = BSON("a" << 1);
        auto nearestInfo = makeChunkInfo(BSON("a" << 10), BSON("a" << 20));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs =
            getOrphanChunkSkipper(shardFilter,
                                  shardKey,
                                  BSON("a" << 1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                  1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, 1 /* expectedScanDirection */, "100"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_EXCLUSIVE(cs,
                                             BSON("a" << 0 << "b"
                                                      << "foo"
                                                      << "c"
                                                      << "bar") /* currentShardKeyValue */,
                                             0 /* expectedPrefixLen */,
                                             BSON("a" << 10) /* expectedKeySuffix */);
    }
    {
        // Mock a nearest chunk.
        auto shardKey = BSON("b" << 1);
        auto nearestInfo = makeChunkInfo(BSON("b" << "zap"), BSON("b" << "zoo"));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs =
            getOrphanChunkSkipper(shardFilter,
                                  shardKey,
                                  BSON("a" << 1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                  1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, 1 /* expectedScanDirection */, "010"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_EXCLUSIVE(cs,
                                             BSON("a" << 0 << "b"
                                                      << "foo"
                                                      << "c"
                                                      << "bar") /* currentShardKeyValue */,
                                             1 /* expectedPrefixLen */,
                                             BSON("b" << "zap") /* expectedKeySuffix */);
    }
    {
        // Mock a nearest chunk.
        auto shardKey = BSON("c" << 1);
        auto nearestInfo = makeChunkInfo(BSON("c" << "baz"), BSON("c" << "boo"));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs =
            getOrphanChunkSkipper(shardFilter,
                                  shardKey,
                                  BSON("a" << 1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                  1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, 1 /* expectedScanDirection */, "001"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_EXCLUSIVE(cs,
                                             BSON("a" << 0 << "b"
                                                      << "foo"
                                                      << "c"
                                                      << "bar") /* currentShardKeyValue */,
                                             2 /* expectedPrefixLen */,
                                             BSON("c" << "baz") /* expectedKeySuffix */);
    }
    // Ensure that no seek point is generated if nearestOwnedChunk() says we're out of chunks.
    // Since this is not a prefix of the index key, we should return 'NoMoreOwnedForThisPrefix',
    // i.e. we may still have non-orphan values for a different {a, b} prefix.
    {
        auto shardKey = BSON("c" << 1);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = boost::none});

        auto cs =
            getOrphanChunkSkipper(shardFilter,
                                  shardKey,
                                  BSON("a" << 1 << "b" << 1 << "c" << -1) /* indexKeyPattern */,
                                  1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, -1 /* expectedScanDirection */, "001"_sd /* expectedBitSet */);
        ASSERT_NO_NEXT_SEEK_POINT(cs,
                                  BSON("a" << 0) /* currentShardKeyValue */,
                                  OrphanChunkSkipper::NoMoreOwnedForThisPrefix);
    }
}

TEST_F(OrphanChunkSkipperTest, MakeSeekPointSingleShardKeyCompoundIndexReverseScan) {
    {
        // Mock a chunk such that the input value is not an orphan.
        auto shardKey = BSON("b" << 1);
        auto nearestInfo = makeChunkInfo(BSON("b" << "zap"), BSON("b" << "zoo"));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = true, .nearestOwnedChunk = nearest});

        auto cs = getOrphanChunkSkipper(
            shardFilter, shardKey, BSON("b" << -1) /* indexKeyPattern */, 1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, -1 /* expectedScanDirection */, "1"_sd /* expectedBitSet */);
        ASSERT_NO_NEXT_SEEK_POINT(
            cs, BSON("b" << "zed") /* currentShardKeyValue */, OrphanChunkSkipper::NotOrphan);
    }

    // Reverse seek to next chunk boundary.
    {
        // Mock a nearest chunk.
        auto shardKey = BSON("a" << 1);
        auto nearestInfo = makeChunkInfo(BSON("a" << -20), BSON("a" << -10));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs =
            getOrphanChunkSkipper(shardFilter,
                                  shardKey,
                                  BSON("a" << 1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                  -1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, -1 /* expectedScanDirection */, "100"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_INCLUSIVE(cs,
                                             BSON("a" << 0 << "b"
                                                      << "foo"
                                                      << "c"
                                                      << "bar") /* currentShardKeyValue */,
                                             0 /* expectedPrefixLen */,
                                             BSON("a" << -10) /* expectedKeySuffix */);
    }
    {
        // Mock a nearest chunk.
        auto shardKey = BSON("b" << 1);
        auto nearestInfo = makeChunkInfo(BSON("b" << "zap"), BSON("b" << "zoo"));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs =
            getOrphanChunkSkipper(shardFilter,
                                  shardKey,
                                  BSON("a" << 1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                  -1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, -1 /* expectedScanDirection */, "010"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_INCLUSIVE(cs,
                                             BSON("a" << 0 << "b"
                                                      << "zooo"
                                                      << "c"
                                                      << "bar") /* currentShardKeyValue */,
                                             1 /* expectedPrefixLen */,
                                             BSON("b" << "zoo") /* expectedKeySuffix */);
    }
    {
        // Mock a nearest chunk.
        auto shardKey = BSON("c" << 1);
        auto nearestInfo = makeChunkInfo(BSON("c" << "baz"), BSON("c" << "boo"));
        Chunk nearest(nearestInfo, boost::none);
        MockNearestOwnedChunkFilter shardFilter(
            shardKey, {.containsShardKey = false, .nearestOwnedChunk = nearest});

        auto cs =
            getOrphanChunkSkipper(shardFilter,
                                  shardKey,
                                  BSON("a" << 1 << "b" << 1 << "c" << 1) /* indexKeyPattern */,
                                  -1 /* scanDir */);
        ASSERT_CHUNK_SKIPPER(cs, -1 /* expectedScanDirection */, "001"_sd /* expectedBitSet */);
        ASSERT_EXPECTED_SEEK_POINT_INCLUSIVE(cs,
                                             BSON("a" << 0 << "b"
                                                      << "foo"
                                                      << "c"
                                                      << "boom") /* currentShardKeyValue */,
                                             2 /* expectedPrefixLen */,
                                             BSON("c" << "boo") /* expectedKeySuffix */);
    }
}

}  // namespace mongo
