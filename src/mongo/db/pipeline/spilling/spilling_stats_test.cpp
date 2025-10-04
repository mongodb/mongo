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

#include "mongo/db/pipeline/spilling/spilling_stats.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
TEST(SpillingStatsTest, Spills) {
    SpillingStats stats;

    ASSERT_EQ(stats.getSpills(), 0);
    stats.incrementSpills();
    ASSERT_EQ(stats.getSpills(), 1);
    stats.incrementSpills(5);
    ASSERT_EQ(stats.getSpills(), 6);

    // Increase by a large number.
    auto current = stats.getSpills();
    auto increaseBig = std::numeric_limits<uint64_t>::max() - current - 10;
    stats.incrementSpills(increaseBig);
    ASSERT_EQ(stats.getSpills(), current + increaseBig);

    // Cause an overflow.
    auto overflowValue = std::numeric_limits<uint64_t>::max() - stats.getSpills() + 10;
    stats.incrementSpills(overflowValue);
    ASSERT_EQ(stats.getSpills(), current + increaseBig);
}

TEST(SpillingStatsTest, SpilledBytes) {
    SpillingStats stats;

    ASSERT_EQ(stats.getSpilledBytes(), 0);
    stats.incrementSpilledBytes(6);
    ASSERT_EQ(stats.getSpilledBytes(), 6);

    // Increase by a large number.
    auto current = stats.getSpilledBytes();
    auto increaseBig = std::numeric_limits<uint64_t>::max() - current - 10;
    stats.incrementSpilledBytes(increaseBig);
    ASSERT_EQ(stats.getSpilledBytes(), current + increaseBig);

    // Cause an overflow.
    auto overflowValue = std::numeric_limits<uint64_t>::max() - stats.getSpilledBytes() + 10;
    stats.incrementSpilledBytes(overflowValue);
    ASSERT_EQ(stats.getSpilledBytes(), current + increaseBig);
}

TEST(SpillingStatsTest, SpilledDataStorageSize) {
    SpillingStats stats;

    ASSERT_EQ(stats.getSpilledDataStorageSize(), 0);
    stats.incrementSpilledDataStorageSize(6);
    ASSERT_EQ(stats.getSpilledDataStorageSize(), 6);

    // Increase by a large number.
    auto current = stats.getSpilledDataStorageSize();
    auto increaseBig = std::numeric_limits<uint64_t>::max() - current - 10;
    stats.incrementSpilledDataStorageSize(increaseBig);
    ASSERT_EQ(stats.getSpilledDataStorageSize(), current + increaseBig);

    // Cause an overflow.
    auto overflowValue =
        std::numeric_limits<uint64_t>::max() - stats.getSpilledDataStorageSize() + 10;
    stats.incrementSpilledDataStorageSize(overflowValue);
    ASSERT_EQ(stats.getSpilledDataStorageSize(), current + increaseBig);
}

TEST(SpillingStatsTest, SpilledRecords) {
    SpillingStats stats;

    ASSERT_EQ(stats.getSpilledRecords(), 0);
    stats.incrementSpilledRecords(6);
    ASSERT_EQ(stats.getSpilledRecords(), 6);

    // Increase by a large number.
    auto current = stats.getSpilledRecords();
    auto increaseBig = std::numeric_limits<uint64_t>::max() - current - 10;
    stats.incrementSpilledRecords(increaseBig);
    ASSERT_EQ(stats.getSpilledRecords(), current + increaseBig);

    // Cause an overflow.
    auto overflowValue = std::numeric_limits<uint64_t>::max() - stats.getSpilledRecords() + 10;
    stats.incrementSpilledRecords(overflowValue);
    ASSERT_EQ(stats.getSpilledRecords(), current + increaseBig);
}

TEST(SpillingStatsTest, AllSpillingStats) {
    SpillingStats stats;

    ASSERT_EQ(stats.getSpilledBytes(), 0);
    ASSERT_EQ(stats.getSpilledBytes(), 0);
    ASSERT_EQ(stats.getSpilledDataStorageSize(), 0);
    ASSERT_EQ(stats.getSpilledRecords(), 0);

    uint64_t spills = 3;
    uint64_t spilledBytes = 1024;
    uint64_t spilledRecords = 100;
    uint64_t spilledDataStorageSize = 10 * 1024;

    stats.updateSpillingStats(spills, spilledBytes, spilledRecords, spilledDataStorageSize);
    ASSERT_EQ(stats.getSpills(), spills);
    ASSERT_EQ(stats.getSpilledBytes(), spilledBytes);
    ASSERT_EQ(stats.getSpilledRecords(), spilledRecords);
    ASSERT_EQ(stats.getSpilledDataStorageSize(), spilledDataStorageSize);

    // Make sure that it keeps the largest spilledDataStorageSize.
    stats.updateSpillingStats(0, 0, 0, spilledDataStorageSize - 10);
    ASSERT_EQ(stats.getSpills(), spills);
    ASSERT_EQ(stats.getSpilledBytes(), spilledBytes);
    ASSERT_EQ(stats.getSpilledRecords(), spilledRecords);
    ASSERT_EQ(stats.getSpilledDataStorageSize(), spilledDataStorageSize);

    // Increase spills by a large number.
    auto increaseBig = std::numeric_limits<uint64_t>::max() - spills + 1;
    stats.updateSpillingStats(increaseBig, 1, 1, spilledDataStorageSize + 1);
    ASSERT_EQ(stats.getSpills(), spills);                                      // same
    ASSERT_EQ(stats.getSpilledBytes(), spilledBytes + 1);                      // increased
    ASSERT_EQ(stats.getSpilledRecords(), spilledRecords + 1);                  // increased
    ASSERT_EQ(stats.getSpilledDataStorageSize(), spilledDataStorageSize + 1);  // changed

    // Increase spilledBytes by a large number.
    increaseBig = std::numeric_limits<uint64_t>::max() - spilledBytes + 1;
    stats.updateSpillingStats(1, increaseBig, 1, spilledDataStorageSize + 2);
    ASSERT_EQ(stats.getSpills(), spills + 1);                                  // increased
    ASSERT_EQ(stats.getSpilledBytes(), spilledBytes + 1);                      // same
    ASSERT_EQ(stats.getSpilledRecords(), spilledRecords + 2);                  // increased
    ASSERT_EQ(stats.getSpilledDataStorageSize(), spilledDataStorageSize + 2);  // changed

    // Increase spilledRecords by a large number.
    increaseBig = std::numeric_limits<uint64_t>::max() - spilledRecords + 1;
    stats.updateSpillingStats(1, 1, increaseBig, spilledDataStorageSize + 3);
    ASSERT_EQ(stats.getSpills(), spills + 2);                                  // increased
    ASSERT_EQ(stats.getSpilledBytes(), spilledBytes + 2);                      // increased
    ASSERT_EQ(stats.getSpilledRecords(), spilledRecords + 2);                  // same
    ASSERT_EQ(stats.getSpilledDataStorageSize(), spilledDataStorageSize + 3);  // changed
}
}  // namespace
}  // namespace mongo
