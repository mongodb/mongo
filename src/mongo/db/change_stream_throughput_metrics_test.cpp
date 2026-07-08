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
