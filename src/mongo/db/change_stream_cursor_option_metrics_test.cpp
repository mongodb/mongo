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

#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <boost/optional.hpp>

namespace mongo::change_stream {
namespace {

// Warms up both histograms with a value that always lands in the first bucket, so that the
// "before" snapshot in each test below is guaranteed to already have a data point (avoiding
// KeyNotFound on the very first read in the binary).
void warmUp() {
    recordCursorOptionMetrics(0, 0);
}

TEST(RecordCursorOptionMetricsTest, BatchSizeIsRecordedWithCorrectValue) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    recordCursorOptionMetrics(42, boost::none);
    auto after =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);

    ASSERT_EQ(after.count, before.count + 1);
    ASSERT_EQ(after.sum, before.sum + 42);
}

TEST(RecordCursorOptionMetricsTest, MaxTimeMSIsRecordedWithCorrectValue) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorMaxTimeMS);
    recordCursorOptionMetrics(boost::none, 12345);
    auto after =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorMaxTimeMS);

    ASSERT_EQ(after.count, before.count + 1);
    ASSERT_EQ(after.sum, before.sum + 12345);
}

TEST(RecordCursorOptionMetricsTest, UnsetBatchSizeDoesNotRecord) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    recordCursorOptionMetrics(boost::none, boost::none);
    auto after =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);

    ASSERT_EQ(after.count, before.count);
    ASSERT_EQ(after.sum, before.sum);
}

TEST(RecordCursorOptionMetricsTest, UnsetMaxTimeMSDoesNotRecord) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorMaxTimeMS);
    recordCursorOptionMetrics(boost::none, boost::none);
    auto after =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorMaxTimeMS);

    ASSERT_EQ(after.count, before.count);
    ASSERT_EQ(after.sum, before.sum);
}

TEST(RecordCursorOptionMetricsTest, BothOptionsAreRecordedIndependently) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto beforeBatchSize =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    auto beforeMaxTimeMS =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorMaxTimeMS);

    recordCursorOptionMetrics(7, 700);

    auto afterBatchSize =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    auto afterMaxTimeMS =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorMaxTimeMS);

    ASSERT_EQ(afterBatchSize.count, beforeBatchSize.count + 1);
    ASSERT_EQ(afterBatchSize.sum, beforeBatchSize.sum + 7);
    ASSERT_EQ(afterMaxTimeMS.count, beforeMaxTimeMS.count + 1);
    ASSERT_EQ(afterMaxTimeMS.sum, beforeMaxTimeMS.sum + 700);
}

}  // namespace
}  // namespace mongo::change_stream
