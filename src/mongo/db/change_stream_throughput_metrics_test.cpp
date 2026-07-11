// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::change_stream {
namespace {

// Returns the current value of a counter, or 0 if it has not been recorded yet.
int64_t readCounter(otel::metrics::OtelMetricsCapturer& capturer, otel::metrics::MetricName name) {
    try {
        return capturer.readInt64Counter(name);
    } catch (const ExceptionFor<ErrorCodes::KeyNotFound>&) {
        return 0;
    }
}

TEST(RecordChangeStreamThroughputTest, DocsReturnedIncrementsByOne) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsReturned);
    cursorDocsReturned().add(1);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsReturned),
              before + 1);
}

TEST(RecordChangeStreamThroughputTest, DocsReturnedIncrementsByMany) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsReturned);
    cursorDocsReturned().add(5);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsReturned),
              before + 5);
}

TEST(RecordChangeStreamThroughputTest, BytesReturnedIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesReturned);
    cursorBytesReturned().add(1024);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesReturned),
              before + 1024);
}

TEST(RecordChangeStreamThroughputTest, BytesReturnedAccumulatesAcrossMultipleCalls) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesReturned);
    cursorBytesReturned().add(100);
    cursorBytesReturned().add(200);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesReturned),
              before + 300);
}

TEST(RecordChangeStreamThroughputTest, BatchesReturnedIncrementsByOne) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBatchesReturned);
    cursorBatchesReturned().add(1);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBatchesReturned),
              before + 1);
}

TEST(RecordChangeStreamThroughputTest, BatchesReturnedIncrementsByMany) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBatchesReturned);
    cursorBatchesReturned().add(3);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBatchesReturned),
              before + 3);
}

TEST(RecordChangeStreamThroughputTest, DocsExaminedIncrementsByOne) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsExamined);
    cursorDocsExamined().add(1);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsExamined),
              before + 1);
}

TEST(RecordChangeStreamThroughputTest, DocsExaminedIncrementsByMany) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsExamined);
    cursorDocsExamined().add(7);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorDocsExamined),
              before + 7);
}

TEST(RecordChangeStreamThroughputTest, BytesReadIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesRead);
    cursorBytesRead().add(2048);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesRead),
              before + 2048);
}

TEST(RecordChangeStreamThroughputTest, BytesReadAccumulatesAcrossMultipleCalls) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesRead);
    cursorBytesRead().add(512);
    cursorBytesRead().add(256);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamCursorBytesRead),
              before + 768);
}

}  // namespace
}  // namespace mongo::change_stream
