// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(CollectionReplicatedMetadataTest, HashIsAbsentByDefault) {
    EXPECT_FALSE(CollectionReplicatedMetadata{}.hash);
}

TEST(CollectionReplicatedMetadataTest, AddCombinesHashesWithXor) {
    const CollectionReplicatedMetadata lhs{.sizeCount = {.size = 10, .count = 1}, .hash = 0b0110};
    const CollectionReplicatedMetadata rhs{.sizeCount = {.size = 20, .count = 1}, .hash = 0b1010};

    const auto result = lhs + rhs;
    EXPECT_EQ(30, result.sizeCount.size);
    EXPECT_EQ(2, result.sizeCount.count);
    EXPECT_EQ(int64_t{0b1100}, result.hash);
}

TEST(CollectionReplicatedMetadataTest, SubtractCombinesHashesWithXor) {
    const CollectionReplicatedMetadata lhs{.sizeCount = {.size = 30, .count = 2}, .hash = 0b0110};
    const CollectionReplicatedMetadata rhs{.sizeCount = {.size = 20, .count = 1}, .hash = 0b1010};

    const auto result = lhs - rhs;
    EXPECT_EQ(10, result.sizeCount.size);
    EXPECT_EQ(1, result.sizeCount.count);
    EXPECT_EQ(int64_t{0b1100}, result.hash);
}

TEST(CollectionReplicatedMetadataTest, SubtractUndoesAdd) {
    const CollectionReplicatedMetadata accumulated{.sizeCount = {.size = 30, .count = 2},
                                                   .hash = 0x1234};
    const CollectionReplicatedMetadata delta{.sizeCount = {.size = 20, .count = 1}, .hash = 0x5678};

    EXPECT_EQ(accumulated, (accumulated + delta) - delta);
}

// A document inserted and later deleted contributes its hash twice, which cancels out.
TEST(CollectionReplicatedMetadataTest, AddingTheSameHashTwiceCancels) {
    const CollectionReplicatedMetadata empty{.sizeCount = {.size = 0, .count = 0}, .hash = 0};
    const CollectionReplicatedMetadata insert{.sizeCount = {.size = 100, .count = 1},
                                              .hash = 0xabcd};
    const CollectionReplicatedMetadata remove{.sizeCount = {.size = -100, .count = -1},
                                              .hash = 0xabcd};

    EXPECT_EQ(empty, (empty + insert) + remove);
}

TEST(CollectionReplicatedMetadataTest, AbsentHashPoisonsOnBothSides) {
    const CollectionReplicatedMetadata tracked{.sizeCount = {.size = 10, .count = 1}, .hash = 42};
    const CollectionReplicatedMetadata untracked{.sizeCount = {.size = 10, .count = 1},
                                                 .hash = boost::none};

    EXPECT_FALSE((tracked + untracked).hash);
    EXPECT_FALSE((untracked + tracked).hash);
    EXPECT_FALSE((tracked - untracked).hash);
    EXPECT_FALSE((untracked - tracked).hash);
    EXPECT_FALSE((untracked + untracked).hash);
}

// Absence is sticky: once poisoned, folding in tracked deltas cannot resurrect the hash.
TEST(CollectionReplicatedMetadataTest, AbsentHashIsNotResurrectedByLaterDeltas) {
    CollectionReplicatedMetadata accumulated{.sizeCount = {.size = 0, .count = 0}, .hash = 0};
    accumulated = accumulated +
        CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}, .hash = boost::none};
    accumulated = accumulated +
        CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}, .hash = 42};

    EXPECT_EQ(20, accumulated.sizeCount.size);
    EXPECT_EQ(2, accumulated.sizeCount.count);
    EXPECT_FALSE(accumulated.hash);
}

// Zero is a legitimate hash - the hash of an empty collection - and must not be conflated with an
// absent one.
TEST(CollectionReplicatedMetadataTest, ZeroHashIsDistinctFromAbsentHash) {
    const CollectionReplicatedMetadata zeroHash{.sizeCount = {.size = 0, .count = 0}, .hash = 0};
    const CollectionReplicatedMetadata noHash{.sizeCount = {.size = 0, .count = 0},
                                              .hash = boost::none};

    EXPECT_NE(zeroHash, noHash);
    EXPECT_TRUE((zeroHash + zeroHash).hash);
    EXPECT_EQ(int64_t{0}, (zeroHash + zeroHash).hash);
}

TEST(CollectionReplicatedMetadataTest, EqualityComparesHash) {
    EXPECT_EQ((CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}, .hash = 42}),
              (CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}, .hash = 42}));
    EXPECT_NE((CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}, .hash = 42}),
              (CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}, .hash = 43}));
    EXPECT_EQ((CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}}),
              (CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}}));
}

TEST(CollectionReplicatedMetadataTest, ToStringOmitsAbsentHash) {
    EXPECT_EQ("size: 10, count: 1",
              (CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}}.toString()));
    EXPECT_EQ("size: 10, count: 1, hash: 42",
              (CollectionReplicatedMetadata{.sizeCount = {.size = 10, .count = 1}, .hash = 42}
                   .toString()));
}

}  // namespace
}  // namespace mongo
