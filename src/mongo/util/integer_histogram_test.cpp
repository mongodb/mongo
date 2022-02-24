/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/integer_histogram.h"

namespace mongo {

namespace {

TEST(IntegerHistogram, EnsureCountsIncrementedAndStored) {
    std::array<int64_t, 4> lowerBounds{0, 5, 8, 12};
    IntegerHistogram<4> hist("testKey", lowerBounds);
    int64_t sum = 0;
    int64_t numInserts = 15;
    for (int64_t i = 0; i < numInserts; i++) {
        hist.increment(i);
        sum += i;
    }

    auto out = [&] {
        BSONObjBuilder builder;
        hist.append(builder, true);
        return builder.obj();
    }();

    auto buckets = out["testKey"];
    ASSERT_EQUALS(buckets["0 - 5"]["count"].Long(), 5);
    ASSERT_EQUALS(buckets["5 - 8"]["count"].Long(), 3);
    ASSERT_EQUALS(buckets["8 - 12"]["count"].Long(), 4);
    ASSERT_EQUALS(buckets["12 - inf"]["count"].Long(), 3);
    ASSERT_EQUALS(buckets["ops"].Long(), numInserts);
    ASSERT_EQUALS(buckets["sum"].Long(), sum);
    ASSERT_EQUALS(buckets["mean"].Double(), static_cast<double>(sum) / numInserts);
}

TEST(IntegerHistogram, EnsureCountsIncrementedInSmallestBucket) {
    std::array<int64_t, 3> lowerBounds{5, 8, 12};
    IntegerHistogram<3> hist("testKey2", lowerBounds);
    int64_t sum = 0;
    int64_t numInserts = 5;
    for (int64_t i = 0; i < numInserts; i++) {
        hist.increment(i);
        sum += i;
    }

    auto out = [&] {
        BSONObjBuilder builder;
        hist.append(builder, true);
        return builder.obj();
    }();

    auto buckets = out["testKey2"];
    ASSERT_EQUALS(buckets["-inf - 5"]["count"].Long(), 5);
    ASSERT_EQUALS(buckets["ops"].Long(), numInserts);
    ASSERT_EQUALS(buckets["sum"].Long(), sum);
    ASSERT_EQUALS(buckets["mean"].Double(), static_cast<double>(sum) / numInserts);
}

TEST(IntegerHistogram, EnsureCountsCorrectlyIncrementedAtBoundary) {
    std::array<int64_t, 3> lowerBounds{5, 8, 12};
    IntegerHistogram<3> hist("testKey3", lowerBounds);
    int64_t sum = 0;
    int64_t numInserts = 3;
    for (auto& boundary : lowerBounds) {
        hist.increment(boundary);
        sum += boundary;
    }

    auto out = [&] {
        BSONObjBuilder builder;
        hist.append(builder, true);
        return builder.obj();
    }();

    auto buckets = out["testKey3"];
    ASSERT_EQUALS(buckets["5 - 8"]["count"].Long(), 1);
    ASSERT_EQUALS(buckets["8 - 12"]["count"].Long(), 1);
    ASSERT_EQUALS(buckets["12 - inf"]["count"].Long(), 1);
    ASSERT_EQUALS(buckets["ops"].Long(), numInserts);
    ASSERT_EQUALS(buckets["sum"].Long(), sum);
    ASSERT_EQUALS(buckets["mean"].Double(), static_cast<double>(sum) / numInserts);
}

TEST(IntegerHistogram, EnsureNegativeCountsIncrementBucketsCorrectly) {
    std::array<int64_t, 3> lowerBounds{-12, -8, 5};
    IntegerHistogram<3> hist("testKey4", lowerBounds);
    int64_t sum = 0;
    int64_t numInserts = 25;
    for (int64_t i = -15; i < 10; i++) {
        hist.increment(i);
        sum += i;
    }

    auto out = [&] {
        BSONObjBuilder builder;
        hist.append(builder, true);
        return builder.obj();
    }();

    auto buckets = out["testKey4"];
    ASSERT_EQUALS(buckets["-inf - -12"]["count"].Long(), 3);
    ASSERT_EQUALS(buckets["-12 - -8"]["count"].Long(), 4);
    ASSERT_EQUALS(buckets["-8 - 5"]["count"].Long(), 13);
    ASSERT_EQUALS(buckets["5 - inf"]["count"].Long(), 5);
    ASSERT_EQUALS(buckets["ops"].Long(), numInserts);
    ASSERT_EQUALS(buckets["sum"].Long(), sum);
    ASSERT_EQUALS(buckets["mean"].Double(), static_cast<double>(sum) / numInserts);
}

TEST(IntegerHistogram, SkipsEmptyBuckets) {
    std::array<int64_t, 2> lowerBounds{0, 5};
    IntegerHistogram<2> hist("testKey6", lowerBounds);
    hist.increment(6);

    auto out = [&] {
        BSONObjBuilder builder;
        hist.append(builder, true);
        return builder.obj();
    }();

    auto buckets = out["testKey6"];
    ASSERT_THROWS(buckets["0 - 5"]["count"].Long(), DBException);
    ASSERT_EQ(buckets["5 - inf"]["count"].Long(), 1);
}

DEATH_TEST(IntegerHistogram,
           FailIfFirstLowerBoundIsMin,
           "Lower bounds must be strictly monotonically increasing") {
    std::array<int64_t, 2> lowerBounds{std::numeric_limits<int64_t>::min(), 5};
    IntegerHistogram<2> hist("testKey5", lowerBounds);
}

DEATH_TEST(IntegerHistogram,
           FailsWhenLowerBoundNotMonotonic,
           "Lower bounds must be strictly monotonically increasing") {
    std::array<int64_t, 2> lowerBounds{5, 0};
    IntegerHistogram<2>("testKey7", lowerBounds);
}
}  // namespace
}  // namespace mongo
