// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
