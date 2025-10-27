/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/ftdc/rolling_stats.h"

#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

// Executes assertions that the two RollingStatsResults are equivalent.
void compareResult(RollingStatsResult actual, RollingStatsResult expected) {
    ASSERT_EQ(actual.count, expected.count);
    ASSERT_APPROX_EQUAL(actual.mean, expected.mean, 0.01);
    ASSERT_EQ(actual.max, expected.max);
    ASSERT_EQ(actual.p50, expected.p50);
    ASSERT_EQ(actual.p90, expected.p90);
    ASSERT_EQ(actual.p99, expected.p99);
}

TEST(RollingStatsTest, Empty) {
    RollingStats stats;
    RollingStatsResult result = stats.getStats();
    compareResult(result, RollingStatsResult());
}

TEST(RollingStatsTest, OneValue) {
    RollingStats stats;
    stats.record(10);
    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 1, .mean = 9, .max = 10, .p50 = 0, .p90 = 0, .p99 = 0});
}

TEST(RollingStatsTest, ValuesAboveMaxValue) {
    RollingStats stats;
    stats.record(100'000);
    stats.record(200'000);
    stats.record(300'000);
    RollingStatsResult result = stats.getStats();
    compareResult(
        result,
        {.count = 3, .mean = 10'000, .max = 300'000, .p50 = 10'000, .p90 = 10'000, .p99 = 10'000});
}

TEST(RollingStatsTest, ValuesAboveCustomMaxValue) {
    RollingStats stats({.maxValue = 1'000});
    stats.record(100'000);
    stats.record(200'000);
    stats.record(300'000);
    RollingStatsResult result = stats.getStats();
    compareResult(
        result,
        {.count = 3, .mean = 1'000, .max = 300'000, .p50 = 1'000, .p90 = 1'000, .p99 = 1'000});
}

TEST(RollingStatsTest, ZeroValues) {
    RollingStats stats;
    stats.record(0);
    stats.record(0);
    stats.record(0);
    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 3, .mean = 0, .max = 0, .p50 = 0, .p90 = 0, .p99 = 0});
}

TEST(RollingStatsTest, NegativeValues) {
    RollingStats stats;
    stats.record(-1);
    stats.record(-2);
    stats.record(-3);
    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 3, .mean = 0, .max = 0, .p50 = 0, .p90 = 0, .p99 = 0});
}

TEST(RollingStatsTest, TwoValues) {
    RollingStats stats;
    stats.record(10);
    stats.record(20);
    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 2, .mean = 14, .max = 20, .p50 = 9, .p90 = 9, .p99 = 9});
}

TEST(RollingStatsTest, ThreeValues) {
    RollingStats stats;
    stats.record(10);
    stats.record(20);
    stats.record(60);
    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 3, .mean = 29.333f, .max = 60, .p50 = 9, .p90 = 19, .p99 = 19});
}

TEST(RollingStatsTest, ThreeOldButGoodValues) {
    ClockSourceMock clock;
    RollingStats stats({.clock = &clock});
    stats.record(10);
    stats.record(20);
    stats.record(60);
    clock.advance(Seconds(59));
    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 3, .mean = 29.333f, .max = 60, .p50 = 9, .p90 = 19, .p99 = 19});
}

TEST(RollingStatsTest, AllOldValues) {
    ClockSourceMock clock;
    RollingStats stats({.clock = &clock});
    stats.record(10);
    stats.record(20);
    stats.record(60);
    clock.advance(Seconds(61));
    RollingStatsResult result = stats.getStats();
    compareResult(result, RollingStatsResult());
}

TEST(RollingStatsTest, OneHundredValuesInOrder) {
    RollingStats stats;
    for (int i = 1; i <= 100; ++i) {
        stats.record(i);
    }
    RollingStatsResult result = stats.getStats();
    compareResult(result,
                  {.count = 100, .mean = 45.75f, .max = 100, .p50 = 48, .p90 = 75, .p99 = 94});
}

TEST(RollingStatsTest, OneHundredValuesInReverseOrder) {
    RollingStats stats;
    for (int i = 100; i >= 1; --i) {
        stats.record(i);
    }
    RollingStatsResult result = stats.getStats();
    compareResult(result,
                  {.count = 100, .mean = 45.75f, .max = 100, .p50 = 48, .p90 = 75, .p99 = 94});
}

TEST(RollingStatsTest, OneHundredOneValuesInOrder) {
    RollingStats stats;
    for (int i = 0; i <= 100; ++i) {
        stats.record(i);
    }
    RollingStatsResult result = stats.getStats();
    compareResult(result,
                  {.count = 101, .mean = 45.3f, .max = 100, .p50 = 48, .p90 = 75, .p99 = 94});
}

TEST(RollingStatsTest, OneHundredValuesInOrderHalfOld) {
    ClockSourceMock clock;
    RollingStats stats({.clock = &clock});
    // Increment by 2 minutes/100 so that only the first half are recent enough to keep.
    Milliseconds increment = Milliseconds(Minutes(2)) / 100;
    for (int i = 1; i <= 100; ++i) {
        stats.record(i);
        clock.advance(increment);
    }
    RollingStatsResult result = stats.getStats();
    compareResult(result,
                  {.count = 50, .mean = 68.3f, .max = 100, .p50 = 75, .p90 = 94, .p99 = 94});
}

TEST(RollingStatsTest, OneHundredValuesInReverseOrderHalfOld) {
    ClockSourceMock clock;
    RollingStats stats({.clock = &clock});
    // Increment by 2 minutes/100 so that only the first half are recent enough to keep.
    Milliseconds increment = Milliseconds(Minutes(2)) / 100;
    for (int i = 100; i >= 1; --i) {
        stats.record(i);
        clock.advance(increment);
    }
    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 50, .mean = 23.2f, .max = 50, .p50 = 24, .p90 = 38, .p99 = 48});
}

TEST(RollingStatsTest, ValuesSpreadOverTime) {
    ClockSourceMock clock;
    RollingStats stats({.clock = &clock});
    // The recorded values are the exact values for buckets to make understanding results easier.
    stats.record(1);
    stats.record(2);
    clock.advance(Seconds(10));
    stats.record(4);
    stats.record(5);
    clock.advance(Seconds(10));
    stats.record(1);
    stats.record(3);
    stats.record(4);

    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 7, .mean = 2.857f, .max = 5, .p50 = 2, .p90 = 4, .p99 = 4});
}

TEST(RollingStatsTest, CustomWindowDuration) {
    ClockSourceMock clock;
    RollingStats stats({.windowDuration = Seconds(30), .clock = &clock});
    stats.record(1);
    stats.record(1);
    clock.advance(Seconds(20));
    stats.record(5);
    stats.record(5);
    clock.advance(Seconds(20));

    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 2, .mean = 5, .max = 5, .p50 = 5, .p90 = 5, .p99 = 5});
}

TEST(RollingStatsTest, CustomWindowIncrement) {
    ClockSourceMock clock;
    RollingStats stats({.windowIncrement = Milliseconds(100), .clock = &clock});
    // The first value will not appear in the stats, but the rest will.
    stats.record(1'000);
    clock.advance(Milliseconds(500));
    stats.record(5);
    clock.advance(Milliseconds(100));
    stats.record(5);
    // Advance so 60.1 seconds have elapsed since the first value was recorded.
    clock.advance(Milliseconds(59'500));

    RollingStatsResult result = stats.getStats();
    compareResult(result, {.count = 2, .mean = 5, .max = 5, .p50 = 5, .p90 = 5, .p99 = 5});
}

}  // namespace
}  // namespace mongo
