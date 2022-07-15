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
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(SorterStatsTest, Basic) {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledRanges();
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 1);
}

TEST(SorterStatsTest, MultipleSorters) {
    SorterTracker sorterTracker;
    SorterStats sorterStats1(&sorterTracker);
    SorterStats sorterStats2(&sorterTracker);
    SorterStats sorterStats3(&sorterTracker);

    sorterStats1.incrementSpilledRanges();
    sorterStats2.incrementSpilledRanges();
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 2);

    sorterStats3.setSpilledRanges(10);
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 12);
}

DEATH_TEST(SorterStatsTest, SetNonZeroNumSpilledRanges, "invariant") {
    SorterTracker sorterTracker;
    SorterStats sorterStats(&sorterTracker);

    sorterStats.incrementSpilledRanges();
    ASSERT_EQ(sorterTracker.spilledRanges.load(), 1);

    sorterStats.setSpilledRanges(10);
}
}  // namespace
}  // namespace mongo
