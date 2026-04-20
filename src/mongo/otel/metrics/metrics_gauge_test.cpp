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

#include "mongo/otel/metrics/metrics_gauge.h"

#include "mongo/otel/metrics/metrics_attributes_test_utils.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <vector>

namespace mongo::otel::metrics {

using testing::DoubleEq;
using testing::ElementsAre;
using testing::IsEmpty;

template <typename T>
class GaugeImplTest : public testing::Test {};

using GaugeTypes = testing::Types<int64_t, double>;
TYPED_TEST_SUITE(GaugeImplTest, GaugeTypes);

TYPED_TEST(GaugeImplTest, Sets) {
    GaugeImpl<TypeParam> gauge;
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{0})));
    gauge.set(1);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{1})));
    gauge.set(10);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{10})));
    gauge.set(-1);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{-1})));
}

// Any issues with thread safety should be caught by tsan on this test.
TYPED_TEST(GaugeImplTest, ConcurrentSets) {
    GaugeImpl<TypeParam> gauge;
    constexpr int kNumThreads = 10;
    constexpr int kIterationsPerThread = 1000;

    std::vector<stdx::thread> threads;
    for (int i = 0; i < kNumThreads; ++i) {
        threads.emplace_back([&gauge]() {
            for (int j = 0; j < kIterationsPerThread; ++j) {
                gauge.set(j);
            }
        });
    }

    for (stdx::thread& thread : threads) {
        thread.join();
    }

    EXPECT_THAT(gauge.values(),
                ElementsAre(IsAttributesAndValue(IsEmpty(), TypeParam{kIterationsPerThread - 1})));
}

TEST(DoubleGaugeImplTest, SetsFractionalValues) {
    GaugeImpl<double> gauge;
    gauge.set(1.1);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(1.1))));
    gauge.set(-3.14);
    EXPECT_THAT(gauge.values(), ElementsAre(IsAttributesAndValue(IsEmpty(), DoubleEq(-3.14))));
}

}  // namespace mongo::otel::metrics
