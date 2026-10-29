// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knob_expressions.h"

#include "mongo/unittest/unittest.h"

#include <limits>

namespace mongo {
namespace {

constexpr long long kOneGB = 1LL * 1024 * 1024 * 1024;

// With little available memory, 20% is below 1GB, so the 1GB floor wins.
TEST(QueryKnobExpressionsTest, MaxMemoryPerOperationFloorsAtOneGB) {
    // 1GB of RAM -> 20% is ~205MB.
    ASSERT_EQ(kOneGB, defaultInternalQueryMaxMemoryUsageBytesPerOperation(kOneGB));
    // Even with 0 available memory we never drop below the 1GB floor.
    ASSERT_EQ(kOneGB, defaultInternalQueryMaxMemoryUsageBytesPerOperation(0));
    // Just under the crossover (5GB): 20% is just under 1GB.
    ASSERT_EQ(kOneGB,
              defaultInternalQueryMaxMemoryUsageBytesPerOperation(5 * kOneGB - 1024 * 1024));
}

// With ample available memory, 20% exceeds 1GB and wins.
TEST(QueryKnobExpressionsTest, MaxMemoryPerOperationUsesTwentyPercentWhenLarge) {
    // 100GB of RAM -> 20% is 20GB.
    ASSERT_EQ(20 * kOneGB, defaultInternalQueryMaxMemoryUsageBytesPerOperation(100 * kOneGB));
    // 10GB of RAM -> 20% is 2GB.
    ASSERT_EQ(2 * kOneGB, defaultInternalQueryMaxMemoryUsageBytesPerOperation(10 * kOneGB));
}

// Exactly at the crossover (5GB), 20% equals 1GB and the result is 1GB either way.
TEST(QueryKnobExpressionsTest, MaxMemoryPerOperationCrossoverAtFiveGB) {
    ASSERT_EQ(kOneGB, defaultInternalQueryMaxMemoryUsageBytesPerOperation(5 * kOneGB));
}

// The largest possible input must still yield a positive result: ULLONG_MAX / 5 (~3.7e18) is well
// below LLONG_MAX, so the narrowing cast never wraps negative.
TEST(QueryKnobExpressionsTest, MaxMemoryPerOperationNeverNegativeAtMaxInput) {
    const long long result = defaultInternalQueryMaxMemoryUsageBytesPerOperation(
        std::numeric_limits<unsigned long long>::max());
    ASSERT_GT(result, 0);
    ASSERT_EQ(static_cast<long long>(std::numeric_limits<unsigned long long>::max() / 5), result);
}

}  // namespace
}  // namespace mongo
