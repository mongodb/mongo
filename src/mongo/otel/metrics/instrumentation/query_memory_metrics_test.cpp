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

#include "mongo/otel/metrics/instrumentation/query_memory_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <limits>

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class QueryMemoryOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    QueryMemoryMetrics _metrics;
};

TEST_F(QueryMemoryOtelMetricsTest, UpdateSetsGauge) {
    _metrics.update(1024 * 1024);

    ASSERT_EQ(
        1024 * 1024,
        _capturer.readInt64Gauge(MetricNames::kQueryConfiguredMaxMemoryUsageBytesPerOperation));
}

TEST_F(QueryMemoryOtelMetricsTest, GaugeTracksLatestValue) {
    _metrics.update(5000);
    _metrics.update(9000);

    ASSERT_EQ(
        9000,
        _capturer.readInt64Gauge(MetricNames::kQueryConfiguredMaxMemoryUsageBytesPerOperation));
}

TEST_F(QueryMemoryOtelMetricsTest, GaugeReportsUnboundedDefault) {
    // The server parameter defaults to std::numeric_limits<long long>::max(), which represents an
    // effectively unbounded operation-wide memory limit.
    _metrics.update(std::numeric_limits<int64_t>::max());

    ASSERT_EQ(
        std::numeric_limits<int64_t>::max(),
        _capturer.readInt64Gauge(MetricNames::kQueryConfiguredMaxMemoryUsageBytesPerOperation));
}

}  // namespace
}  // namespace mongo
