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

#include "mongo/otel/metrics/instrumentation/index_build_metrics.h"

#include "mongo/db/index/index_bulk_builder_metrics.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

IndexBulkBuilderMetricsSnapshot makeSnapshot(long long numSorted,
                                             long long bytesSorted,
                                             long long memUsage,
                                             long long spilledRanges) {
    IndexBulkBuilderMetricsSnapshot s{};
    s.numSorted = numSorted;
    s.bytesSorted = bytesSorted;
    s.memUsage = memUsage;
    s.spilledRanges = spilledRanges;
    return s;
}

class IndexBuildOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    // Must be constructed before any metric is recorded so it resets the process-global
    // instruments and captures subsequent writes.
    OtelMetricsCapturer _capturer;
};

// Regression test for the value-initialized _prevSnapshot: the very first update() must measure
// deltas against a zeroed previous snapshot, so the instruments reflect the full snapshot. An
// uninitialized _prevSnapshot would emit garbage deltas here.
TEST_F(IndexBuildOtelMetricsTest, FirstUpdateMeasuresFromZero) {
    IndexBuildOtelMetrics metrics;
    metrics.update(makeSnapshot(/*numSorted=*/10,
                                /*bytesSorted=*/100,
                                /*memUsage=*/500,
                                /*spilledRanges=*/3));

    ASSERT_EQ(10, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderNumSorted));
    ASSERT_EQ(100, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted));
    ASSERT_EQ(500, _capturer.readInt64Gauge(MetricNames::kIndexBulkBuilderMemUsage));
    ASSERT_EQ(3, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderSpilledRanges));
}

// update() retains the previous snapshot between ticks, so monotonic counters accumulate only the
// per-tick delta (running total == latest absolute value) and the gauge reports the latest value.
TEST_F(IndexBuildOtelMetricsTest, SubsequentUpdatesCarryPreviousSnapshot) {
    IndexBuildOtelMetrics metrics;
    metrics.update(makeSnapshot(10, 100, 500, 3));
    metrics.update(makeSnapshot(25, 250, 800, 5));

    ASSERT_EQ(25, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderNumSorted));
    ASSERT_EQ(250, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderBytesSorted));
    ASSERT_EQ(800, _capturer.readInt64Gauge(MetricNames::kIndexBulkBuilderMemUsage));
    ASSERT_EQ(5, _capturer.readInt64Counter(MetricNames::kIndexBulkBuilderSpilledRanges));
}

}  // namespace
}  // namespace mongo
