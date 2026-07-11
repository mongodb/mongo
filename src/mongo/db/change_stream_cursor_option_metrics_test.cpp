// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
