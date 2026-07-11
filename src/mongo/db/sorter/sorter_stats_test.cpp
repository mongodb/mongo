// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sorter/sorter_stats.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(SorterStatsTest, Basics) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledRanges();
    EXPECT_EQ(sorterStats.spilledRanges(), 1);
    EXPECT_EQ(sorterTracker.spilledRanges.load(), 1);

    sorterStats.incrementMergedSpills();
    EXPECT_EQ(sorterStats.mergedSpills(), 1);
    EXPECT_EQ(sorterTracker.mergedSpills.load(), 1);

    sorterStats.incrementSpilledKeyValuePairs(10);
    EXPECT_EQ(sorterStats.spilledKeyValuePairs(), 10);
    EXPECT_EQ(sorterTracker.spilledKeyValuePairs.load(), 10);

    sorterStats.incrementNumSorted();
    EXPECT_EQ(sorterStats.numSorted(), 1);
    EXPECT_EQ(sorterTracker.numSorted.load(), 1);

    sorterStats.incrementBytesSorted(1);
    EXPECT_EQ(sorterStats.bytesSorted(), 1);
    EXPECT_EQ(sorterTracker.bytesSorted.load(), 1);
}

TEST(SorterStatsTest, SingleSorterMemUsage) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementMemUsage(2);
    EXPECT_EQ(sorterStats.memUsage(), 2);
    EXPECT_EQ(sorterTracker.memUsage.load(), 2);

    sorterStats.decrementMemUsage(1);
    EXPECT_EQ(sorterStats.memUsage(), 1);
    EXPECT_EQ(sorterTracker.memUsage.load(), 1);

    // Decrement 'memUsage' more than the total
    sorterStats.decrementMemUsage(10);
    EXPECT_EQ(sorterStats.memUsage(), 0);
    EXPECT_EQ(sorterTracker.memUsage.load(), 0);

    sorterStats.resetMemUsage();
    EXPECT_EQ(sorterStats.memUsage(), 0);
    EXPECT_EQ(sorterTracker.memUsage.load(), 0);

    // Simulate increasing 'memUsage'
    sorterStats.setMemUsage(3);
    EXPECT_EQ(sorterStats.memUsage(), 3);
    EXPECT_EQ(sorterTracker.memUsage.load(), 3);

    // Simulate decreasing 'memUsage'
    sorterStats.setMemUsage(1);
    EXPECT_EQ(sorterStats.memUsage(), 1);
    EXPECT_EQ(sorterTracker.memUsage.load(), 1);
}

TEST(SorterStatsTest, SingleSorterSpilledRanges) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledRanges();
    sorterStats.incrementSpilledRanges();
    EXPECT_EQ(sorterStats.spilledRanges(), 2);
    EXPECT_EQ(sorterTracker.spilledRanges.load(), 2);

    // Simulate increasing spilled ragnes.
    sorterStats.setSpilledRanges(3);
    EXPECT_EQ(sorterStats.spilledRanges(), 3);
    EXPECT_EQ(sorterTracker.spilledRanges.load(), 3);

    // Simulate decreasing spilled ranges.
    sorterStats.setSpilledRanges(1);
    EXPECT_EQ(sorterStats.spilledRanges(), 1);
    EXPECT_EQ(sorterTracker.spilledRanges.load(), 1);

    sorterStats.incrementSpilledRanges();
    EXPECT_EQ(sorterStats.spilledRanges(), 2);
    EXPECT_EQ(sorterTracker.spilledRanges.load(), 2);
}

TEST(SorterStatsTest, MultipleSortersSpilledRanges) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);
    SorterStats sorterStats3(&sorterTracker);

    sorterStats1.incrementSpilledRanges();
    sorterStats2.incrementSpilledRanges();
    EXPECT_EQ(sorterStats1.spilledRanges(), 1);
    EXPECT_EQ(sorterStats2.spilledRanges(), 1);
    EXPECT_EQ(sorterTracker.spilledRanges.load(), 2);

    sorterStats3.setSpilledRanges(10);
    EXPECT_EQ(sorterStats3.spilledRanges(), 10);
    EXPECT_EQ(sorterTracker.spilledRanges.load(), 12);
}

TEST(SorterStatsTest, MultipleSortersMergedSpills) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);

    sorterStats1.incrementMergedSpills();
    sorterStats1.incrementMergedSpills();
    sorterStats2.incrementMergedSpills();
    EXPECT_EQ(sorterStats1.mergedSpills(), 2);
    EXPECT_EQ(sorterStats2.mergedSpills(), 1);
    EXPECT_EQ(sorterTracker.mergedSpills.load(), 3);
}

TEST(SorterStatsTest, SingleSorterSpilledKeyValuePairs) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledKeyValuePairs(2);
    sorterStats.incrementSpilledKeyValuePairs(3);
    EXPECT_EQ(sorterStats.spilledKeyValuePairs(), 5);
    EXPECT_EQ(sorterTracker.spilledKeyValuePairs.load(), 5);
}

TEST(SorterStatsTest, MultipleSortersSpilledKeyValuePairs) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);

    sorterStats1.incrementSpilledKeyValuePairs(2);
    sorterStats2.incrementSpilledKeyValuePairs(3);
    EXPECT_EQ(sorterStats1.spilledKeyValuePairs(), 2);
    EXPECT_EQ(sorterStats2.spilledKeyValuePairs(), 3);
    EXPECT_EQ(sorterTracker.spilledKeyValuePairs.load(), 5);
}

TEST(SorterStatsTest, MultipleSortersNumSorted) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);

    sorterStats1.incrementNumSorted();
    sorterStats2.incrementNumSorted(2);
    EXPECT_EQ(sorterStats1.numSorted(), 1);
    EXPECT_EQ(sorterStats2.numSorted(), 2);
    EXPECT_EQ(sorterTracker.numSorted.load(), 3);
}

TEST(SorterStatsTest, MultipleSortersBytesSorted) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);

    sorterStats1.incrementBytesSorted(1);
    sorterStats2.incrementBytesSorted(2);
    EXPECT_EQ(sorterStats1.bytesSorted(), 1);
    EXPECT_EQ(sorterStats2.bytesSorted(), 2);
    EXPECT_EQ(sorterTracker.bytesSorted.load(), 3);
}

TEST(SorterStatsTest, MultipleSortersMemUsage) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);
    SorterStats sorterStats3(&sorterTracker);

    sorterStats1.incrementMemUsage(1);
    EXPECT_EQ(sorterStats1.memUsage(), 1);
    EXPECT_EQ(sorterTracker.memUsage.load(), 1);

    sorterStats2.incrementMemUsage(2);
    EXPECT_EQ(sorterStats2.memUsage(), 2);
    EXPECT_EQ(sorterTracker.memUsage.load(), 3);

    sorterStats1.resetMemUsage();
    EXPECT_EQ(sorterStats1.memUsage(), 0);
    EXPECT_EQ(sorterTracker.memUsage.load(), 2);

    sorterStats2.decrementMemUsage(1);
    EXPECT_EQ(sorterStats2.memUsage(), 1);
    EXPECT_EQ(sorterTracker.memUsage.load(), 1);

    sorterStats3.incrementMemUsage(3);
    EXPECT_EQ(sorterStats3.memUsage(), 3);
    EXPECT_EQ(sorterTracker.memUsage.load(), 4);

    // Simulate increasing memUsage
    sorterStats1.setMemUsage(4);
    EXPECT_EQ(sorterStats1.memUsage(), 4);
    EXPECT_EQ(sorterTracker.memUsage.load(), 8);

    // Simulate decreasing memUsage
    sorterStats2.setMemUsage(0);
    EXPECT_EQ(sorterStats2.memUsage(), 0);
    EXPECT_EQ(sorterTracker.memUsage.load(), 7);

    sorterStats3.setMemUsage(5);
    EXPECT_EQ(sorterStats3.memUsage(), 5);
    EXPECT_EQ(sorterTracker.memUsage.load(), 9);

    // Simulate sorter spilling.
    sorterStats3.resetMemUsage();
    EXPECT_EQ(sorterStats3.memUsage(), 0);
    EXPECT_EQ(sorterTracker.memUsage.load(), 4);
}

template <typename T>
class SorterStorageStatsTest : public testing::Test {
public:
    SorterTracker sorterTracker;
};

using SorterStorageStatsTypes = ::testing::Types<SorterContainerStats, SorterFileStats>;
TYPED_TEST_SUITE(SorterStorageStatsTest, SorterStorageStatsTypes);

TYPED_TEST(SorterStorageStatsTest, ConstructorWithTracker) {
    TypeParam stats(&this->sorterTracker);

    EXPECT_EQ(stats.bytesSpilledUncompressed(), 0);
    EXPECT_EQ(this->sorterTracker.bytesSpilledUncompressed.loadRelaxed(), 0);
}

TYPED_TEST(SorterStorageStatsTest, ConstructorWithoutTracker) {
    TypeParam stats(nullptr);

    EXPECT_EQ(stats.bytesSpilledUncompressed(), 0);
}

TYPED_TEST(SorterStorageStatsTest, AddSpilledDataSizeUncompressed) {
    TypeParam stats(&this->sorterTracker);

    // Add some data
    stats.addSpilledDataSizeUncompressed(100);

    // Both local and tracker should be updated
    EXPECT_EQ(stats.bytesSpilledUncompressed(), 100);
    EXPECT_EQ(this->sorterTracker.bytesSpilledUncompressed.loadRelaxed(), 100);

    // Add more data
    stats.addSpilledDataSizeUncompressed(50);

    // Should accumulate
    EXPECT_EQ(stats.bytesSpilledUncompressed(), 150);
    EXPECT_EQ(this->sorterTracker.bytesSpilledUncompressed.loadRelaxed(), 150);
}

TYPED_TEST(SorterStorageStatsTest, AddSpilledDataSizeUncompressedWithoutTracker) {
    TypeParam stats(nullptr);

    // Should not crash with null tracker
    stats.addSpilledDataSizeUncompressed(100);
    EXPECT_EQ(stats.bytesSpilledUncompressed(), 100);
}

TYPED_TEST(SorterStorageStatsTest, ZeroValueHandling) {
    TypeParam stats(&this->sorterTracker);

    // Adding zero should not change values
    stats.addSpilledDataSizeUncompressed(0);

    EXPECT_EQ(stats.bytesSpilledUncompressed(), 0);
    EXPECT_EQ(this->sorterTracker.bytesSpilledUncompressed.loadRelaxed(), 0);
}

TYPED_TEST(SorterStorageStatsTest, MultipleInstancesShareTracker) {
    TypeParam stats1(&this->sorterTracker);
    TypeParam stats2(&this->sorterTracker);

    // Both instances should update the same tracker
    stats1.addSpilledDataSizeUncompressed(50);
    stats2.addSpilledDataSizeUncompressed(30);

    EXPECT_EQ(stats1.bytesSpilledUncompressed(), 50);
    EXPECT_EQ(stats2.bytesSpilledUncompressed(), 30);
    EXPECT_EQ(this->sorterTracker.bytesSpilledUncompressed.loadRelaxed(), 80);
}

TEST(SorterContainerStatsTest, AddSpilledDataSize) {
    SorterTracker sorterTracker;
    SorterContainerStats stats(&sorterTracker);

    stats.addSpilledDataSize(100);
    EXPECT_EQ(stats.bytesSpilled(), 100);
    EXPECT_EQ(sorterTracker.bytesSpilled.loadRelaxed(), 100);

    stats.addSpilledDataSize(50);
    EXPECT_EQ(stats.bytesSpilled(), 150);
    EXPECT_EQ(sorterTracker.bytesSpilled.loadRelaxed(), 150);
}

TEST(SorterContainerStatsTest, AddSpilledDataSizeWithoutTracker) {
    SorterContainerStats stats(nullptr);
    stats.addSpilledDataSize(100);
    EXPECT_EQ(stats.bytesSpilled(), 100);
}
}  // namespace
}  // namespace mongo
