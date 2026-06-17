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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

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
