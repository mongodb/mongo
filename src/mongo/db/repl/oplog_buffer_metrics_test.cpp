// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace repl {
namespace {

using ::mongo::otel::metrics::MetricName;
using ::mongo::otel::metrics::MetricNames;
using ::mongo::otel::metrics::OtelMetricsCapturer;

void testBufferMetrics(OtelMetricsCapturer& capturer,
                       OplogBuffer::Counters* counters,
                       MetricName countMetric,
                       MetricName sizeMetric,
                       MetricName maxSizeMetric) {
    counters->setMaxSize(1024 * 1024);
    ASSERT_EQ(1024 * 1024, capturer.readInt64Gauge(maxSizeMetric));

    // incrementN / decrementN update the count and size up-down counters.
    counters->incrementN(3, 300);
    ASSERT_EQ(3, capturer.readInt64Counter(countMetric));
    ASSERT_EQ(300, capturer.readInt64Counter(sizeMetric));

    counters->incrementN(2, 200);
    ASSERT_EQ(5, capturer.readInt64Counter(countMetric));
    ASSERT_EQ(500, capturer.readInt64Counter(sizeMetric));

    counters->decrementN(1, 100);
    ASSERT_EQ(4, capturer.readInt64Counter(countMetric));
    ASSERT_EQ(400, capturer.readInt64Counter(sizeMetric));

    // increment(Value) / decrement(Value) use the BSONObj's objsize() as the delta.
    const auto obj = BSON("x" << 1);
    const size_t objSize = obj.objsize();

    counters->increment(obj);
    ASSERT_EQ(5, capturer.readInt64Counter(countMetric));
    ASSERT_EQ(400 + objSize, capturer.readInt64Counter(sizeMetric));

    counters->decrement(obj);
    ASSERT_EQ(4, capturer.readInt64Counter(countMetric));
    ASSERT_EQ(400, capturer.readInt64Counter(sizeMetric));

    // clear() drains the count and size up-down counters back to zero.
    counters->clear();
    ASSERT_EQ(0, capturer.readInt64Counter(countMetric));
    ASSERT_EQ(0, capturer.readInt64Counter(sizeMetric));
}

/**
 * Verifies that the Counters wired up by OplogBufferMetrics propagate updates to the
 * corresponding OTel-exported oplog buffer metrics for the apply buffer, as well as
 * the maxCount metric, which exists only for the apply buffer.
 */
TEST(OplogBufferMetricsOtelTest, ApplyCountersUpdatesPropagateToOtelMetrics) {
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    OtelMetricsCapturer capturer;

    OplogBufferMetrics oplogBufferMetrics;

    OplogBuffer::Counters* counters = oplogBufferMetrics.getApplyBufferCounter();

    counters->setMaxCount(1024);
    ASSERT_EQ(1024, capturer.readInt64Gauge(MetricNames::kOplogApplyBufferMaxCount));

    testBufferMetrics(capturer,
                      counters,
                      MetricNames::kOplogApplyBufferCount,
                      MetricNames::kOplogApplyBufferSize,
                      MetricNames::kOplogApplyBufferMaxSize);
}

/**
 * Verifies that the Counters wired up by OplogBufferMetrics propagate updates to the
 * corresponding OTel-exported oplog buffer metrics for the write buffer.
 */
TEST(OplogBufferMetricsOtelTest, WriteCountersUpdatesPropagateToOtelMetrics) {
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    OtelMetricsCapturer capturer;

    OplogBufferMetrics oplogBufferMetrics;

    OplogBuffer::Counters* counters = oplogBufferMetrics.getWriteBufferCounter();

    testBufferMetrics(capturer,
                      counters,
                      MetricNames::kOplogWriteBufferCount,
                      MetricNames::kOplogWriteBufferSize,
                      MetricNames::kOplogWriteBufferMaxSize);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
