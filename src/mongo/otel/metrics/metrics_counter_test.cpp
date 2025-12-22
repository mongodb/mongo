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

#include "mongo/otel/metrics/metrics_counter.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo::otel::metrics {

TEST(Int64CounterImplTest, Adds) {
    CounterImpl<int64_t> counter("name", "description", "unit");
    ASSERT_EQ(counter.value(), 0);
    counter.add(1);
    ASSERT_EQ(counter.value(), 1);
    counter.add(10);
    ASSERT_EQ(counter.value(), 11);
}

TEST(Int64CounterImplTest, AddsZero) {
    CounterImpl<int64_t> counter("name", "description", "unit");
    ASSERT_EQ(counter.value(), 0);
    counter.add(0);
    ASSERT_EQ(counter.value(), 0);
    counter.add(10);
    counter.add(0);
    ASSERT_EQ(counter.value(), 10);
}

TEST(Int64CounterImplTest, ExceptionOnNegativeAdd) {
    CounterImpl<int64_t> counter("name", "description", "unit");
    counter.add(1);
    counter.add(3);
    ASSERT_THROWS_CODE(counter.add(-1), DBException, ErrorCodes::BadValue);
}

// Any issues with thread safety should be caught by tsan on this test.
TEST(Int64CounterImplTest, ConcurrentAdds) {
    CounterImpl<int64_t> counter("name", "description", "unit");
    constexpr int kNumThreads = 10;
    constexpr int kIncrementsPerThread = 1000;

    std::vector<stdx::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&counter]() {
            for (int j = 0; j < kIncrementsPerThread; ++j) {
                counter.add(1);
            }
        });
    }

    for (stdx::thread& thread : threads) {
        thread.join();
    }

    ASSERT_EQ(counter.value(), kNumThreads * kIncrementsPerThread);
}

// TODO(SERVER-115756): Test serialization of counter.
}  // namespace mongo::otel::metrics
