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

using testing::DoubleEq;

template <typename T>
class CounterImplTest : public testing::Test {};

using CounterTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(CounterImplTest, CounterTypes);

TYPED_TEST(CounterImplTest, Adds) {
    CounterImpl<TypeParam> counter;
    EXPECT_EQ(counter.value(), 0);
    counter.add(1);
    EXPECT_EQ(counter.value(), 1);
    counter.add(10);
    EXPECT_EQ(counter.value(), 11);
}

TYPED_TEST(CounterImplTest, AddsZero) {
    CounterImpl<TypeParam> counter;
    EXPECT_EQ(counter.value(), 0);
    counter.add(0);
    EXPECT_EQ(counter.value(), 0);
    counter.add(10);
    counter.add(0);
    EXPECT_EQ(counter.value(), 10);
}

TYPED_TEST(CounterImplTest, ExceptionOnNegativeAdd) {
    CounterImpl<TypeParam> counter;
    counter.add(1);
    ASSERT_THROWS_CODE(counter.add(-1), DBException, ErrorCodes::BadValue);
}

// Any issues with thread safety should be caught by tsan on this test.
TYPED_TEST(CounterImplTest, ConcurrentAdds) {
    CounterImpl<TypeParam> counter;
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

    EXPECT_EQ(counter.value(), kNumThreads * kIncrementsPerThread);
}

TYPED_TEST(CounterImplTest, Serialization) {
    CounterImpl<TypeParam> counter;
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(0);
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(10);
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 10));
}

TEST(DoubleCounterImplTest, AddsFractionalValues) {
    CounterImpl<double> counter;
    EXPECT_EQ(counter.value(), 0.0);
    counter.add(1.1);
    EXPECT_THAT(counter.value(), DoubleEq(1.1));
    counter.add(10.5);
    EXPECT_THAT(counter.value(), DoubleEq(11.6));
}
}  // namespace mongo::otel::metrics
