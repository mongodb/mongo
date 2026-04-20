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

#include "mongo/otel/metrics/metrics_updown_counter.h"

#include "mongo/otel/metrics/metrics_attributes_test_utils.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo::otel::metrics {

using testing::DoubleEq;
using testing::ElementsAre;
using testing::IsEmpty;

template <typename T>
class UpDownCounterImplTest : public testing::Test {};

using UpDownCounterTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(UpDownCounterImplTest, UpDownCounterTypes);

TYPED_TEST(UpDownCounterImplTest, Adds) {
    UpDownCounterImpl<TypeParam> counter;
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{0})));
    counter.add(1);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{1})));
    counter.add(10);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{11})));
}

TYPED_TEST(UpDownCounterImplTest, AddsZero) {
    UpDownCounterImpl<TypeParam> counter;
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{0})));
    counter.add(0);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{0})));
    counter.add(10);
    counter.add(0);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{10})));
}

TYPED_TEST(UpDownCounterImplTest, Subtracts) {
    UpDownCounterImpl<TypeParam> counter;
    counter.add(5);
    counter.add(-2);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{3})));
    counter.add(-3);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{0})));
}

// Any issues with thread safety should be caught by tsan on this test.
TYPED_TEST(UpDownCounterImplTest, ConcurrentAdds) {
    UpDownCounterImpl<TypeParam> counter;
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

    EXPECT_THAT(counter.values(),
                ElementsAre(IsAttributesAndValue(IsEmpty(),
                                                 TypeParam{kNumThreads * kIncrementsPerThread})));
}

TYPED_TEST(UpDownCounterImplTest, Serialization) {
    UpDownCounterImpl<TypeParam> counter;
    const std::string key = "a";
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(0);
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 0));
    counter.add(10);
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 10));
    counter.add(-4);
    ASSERT_BSONOBJ_EQ(counter.serializeToBson(key), BSON(key << 6));
}

TEST(DoubleUpDownCounterImplTest, AddsFractionalValues) {
    UpDownCounterImpl<double> counter;
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(0.0))));
    counter.add(1.1);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(1.1))));
    counter.add(10.5);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(11.6))));
    counter.add(-0.6);
    EXPECT_THAT(counter.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(11.0))));
}
}  // namespace mongo::otel::metrics
