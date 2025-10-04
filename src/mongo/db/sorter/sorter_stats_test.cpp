/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/sorter/sorter_stats.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(SorterStatsTest, Basics) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledRanges();
    ASSERT_EQ(sorterStats.spilledRanges(), 1);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 1);

    sorterStats.incrementSpilledKeyValuePairs(10);
    ASSERT_EQ(sorterStats.spilledKeyValuePairs(), 10);
    ASSERT_EQ(sorterTracker.spilledKeyValuePairs.load(), 10);

    sorterStats.incrementNumSorted();
    ASSERT_EQ(sorterStats.numSorted(), 1);
    ASSERT_EQ(sorterTracker.numSorted.load(), 1);

    sorterStats.incrementBytesSorted(1);
    ASSERT_EQ(sorterStats.bytesSorted(), 1);
    ASSERT_EQ(sorterTracker.bytesSorted.load(), 1);
}

TEST(SorterStatsTest, SingleSorterMemUsage) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementMemUsage(2);
    ASSERT_EQ(sorterStats.memUsage(), 2);
    ASSERT_EQ(sorterTracker.memUsage.load(), 2);

    sorterStats.decrementMemUsage(1);
    ASSERT_EQ(sorterStats.memUsage(), 1);
    ASSERT_EQ(sorterTracker.memUsage.load(), 1);

    // Decrement 'memUsage' more than the total
    sorterStats.decrementMemUsage(10);
    ASSERT_EQ(sorterStats.memUsage(), 0);
    ASSERT_EQ(sorterTracker.memUsage.load(), 0);

    sorterStats.resetMemUsage();
    ASSERT_EQ(sorterStats.memUsage(), 0);
    ASSERT_EQ(sorterTracker.memUsage.load(), 0);

    // Simulate increasing 'memUsage'
    sorterStats.setMemUsage(3);
    ASSERT_EQ(sorterStats.memUsage(), 3);
    ASSERT_EQ(sorterTracker.memUsage.load(), 3);

    // Simulate decreasing 'memUsage'
    sorterStats.setMemUsage(1);
    ASSERT_EQ(sorterStats.memUsage(), 1);
    ASSERT_EQ(sorterTracker.memUsage.load(), 1);
}

TEST(SorterStatsTest, SingleSorterSpilledRanges) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledRanges();
    sorterStats.incrementSpilledRanges();
    ASSERT_EQ(sorterStats.spilledRanges(), 2);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 2);

    // Simulate increasing spilled ragnes.
    sorterStats.setSpilledRanges(3);
    ASSERT_EQ(sorterStats.spilledRanges(), 3);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 3);

    // Simulate decreasing spilled ranges.
    sorterStats.setSpilledRanges(1);
    ASSERT_EQ(sorterStats.spilledRanges(), 1);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 1);

    sorterStats.incrementSpilledRanges();
    ASSERT_EQ(sorterStats.spilledRanges(), 2);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 2);
}

TEST(SorterStatsTest, MultipleSortersSpilledRanges) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);
    SorterStats sorterStats3(&sorterTracker);

    sorterStats1.incrementSpilledRanges();
    sorterStats2.incrementSpilledRanges();
    ASSERT_EQ(sorterStats1.spilledRanges(), 1);
    ASSERT_EQ(sorterStats2.spilledRanges(), 1);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 2);

    sorterStats3.setSpilledRanges(10);
    ASSERT_EQ(sorterStats3.spilledRanges(), 10);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 12);
}

TEST(SorterStatsTest, SingleSorterSpilledKeyValuePairs) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledKeyValuePairs(2);
    sorterStats.incrementSpilledKeyValuePairs(3);
    ASSERT_EQ(sorterStats.spilledKeyValuePairs(), 5);
    ASSERT_EQ(sorterTracker.spilledKeyValuePairs.load(), 5);
}

TEST(SorterStatsTest, MultipleSortersSpilledKeyValuePairs) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);

    sorterStats1.incrementSpilledKeyValuePairs(2);
    sorterStats2.incrementSpilledKeyValuePairs(3);
    ASSERT_EQ(sorterStats1.spilledKeyValuePairs(), 2);
    ASSERT_EQ(sorterStats2.spilledKeyValuePairs(), 3);
    ASSERT_EQ(sorterTracker.spilledKeyValuePairs.load(), 5);
}

TEST(SorterStatsTest, MultipleSortersNumSorted) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);

    sorterStats1.incrementNumSorted();
    sorterStats2.incrementNumSorted(2);
    ASSERT_EQ(sorterStats1.numSorted(), 1);
    ASSERT_EQ(sorterStats2.numSorted(), 2);
    ASSERT_EQ(sorterTracker.numSorted.load(), 3);
}

TEST(SorterStatsTest, MultipleSortersBytesSorted) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);

    sorterStats1.incrementBytesSorted(1);
    sorterStats2.incrementBytesSorted(2);
    ASSERT_EQ(sorterStats1.bytesSorted(), 1);
    ASSERT_EQ(sorterStats2.bytesSorted(), 2);
    ASSERT_EQ(sorterTracker.bytesSorted.load(), 3);
}

TEST(SorterStatsTest, MultipleSortersMemUsage) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);
    SorterStats sorterStats3(&sorterTracker);

    sorterStats1.incrementMemUsage(1);
    ASSERT_EQ(sorterStats1.memUsage(), 1);
    ASSERT_EQ(sorterTracker.memUsage.load(), 1);

    sorterStats2.incrementMemUsage(2);
    ASSERT_EQ(sorterStats2.memUsage(), 2);
    ASSERT_EQ(sorterTracker.memUsage.load(), 3);

    sorterStats1.resetMemUsage();
    ASSERT_EQ(sorterStats1.memUsage(), 0);
    ASSERT_EQ(sorterTracker.memUsage.load(), 2);

    sorterStats2.decrementMemUsage(1);
    ASSERT_EQ(sorterStats2.memUsage(), 1);
    ASSERT_EQ(sorterTracker.memUsage.load(), 1);

    sorterStats3.incrementMemUsage(3);
    ASSERT_EQ(sorterStats3.memUsage(), 3);
    ASSERT_EQ(sorterTracker.memUsage.load(), 4);

    // Simulate increasing memUsage
    sorterStats1.setMemUsage(4);
    ASSERT_EQ(sorterStats1.memUsage(), 4);
    ASSERT_EQ(sorterTracker.memUsage.load(), 8);

    // Simulate decreasing memUsage
    sorterStats2.setMemUsage(0);
    ASSERT_EQ(sorterStats2.memUsage(), 0);
    ASSERT_EQ(sorterTracker.memUsage.load(), 7);

    sorterStats3.setMemUsage(5);
    ASSERT_EQ(sorterStats3.memUsage(), 5);
    ASSERT_EQ(sorterTracker.memUsage.load(), 9);

    // Simulate sorter spilling.
    sorterStats3.resetMemUsage();
    ASSERT_EQ(sorterStats3.memUsage(), 0);
    ASSERT_EQ(sorterTracker.memUsage.load(), 4);
}

}  // namespace
}  // namespace mongo
