/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
