// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/server_status/histogram_server_status_metric.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This test case is intended to serve as a cursory check of the HistogramServerStatusMetric
// initialization. It is not meant to be a comprehensive test of the Histogram class. For that,
// refer to histogram_test.cpp.
TEST(HistogramServerStatusMetricTest, TwoBuckets) {
    HistogramServerStatusMetric metric({5U});
    metric.increment(3U);
    metric.increment(7U);
    metric.increment(8U);

    auto&& histogram = metric.hist();
    auto iter = histogram.begin();
    {
        ASSERT(iter != histogram.end());
        auto&& bucket = *iter;
        ASSERT_EQ(bucket.count, 1);
        ASSERT(!bucket.lower);
        ASSERT_EQ(*bucket.upper, 5U);
    }

    iter++;
    {
        ASSERT(iter != histogram.end());
        auto&& bucket = *iter;
        ASSERT_EQ(bucket.count, 2);
        ASSERT_EQ(*bucket.lower, 5U);
        ASSERT(!bucket.upper);
    }

    iter++;
    ASSERT(iter == histogram.end());
}

}  // namespace
}  // namespace mongo
