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

// Calls incrementChangeStreamErrorCounters() with an exception of the given code.
void triggerErrorCounter(ErrorCodes::Error code) {
    try {
        uasserted(code, "test-induced error");
    } catch (const DBException& ex) {
        incrementChangeStreamErrorCounters(ex);
    }
}

TEST(RecordChangeStreamErrorTest, ChangeStreamHistoryLostIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = readCounter(
        capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableHistoryLost);
    triggerErrorCounter(ErrorCodes::ChangeStreamHistoryLost);
    ASSERT_EQ(readCounter(capturer,
                          otel::metrics::MetricNames::kChangeStreamErrorNonRetriableHistoryLost),
              before + 1);
}

TEST(RecordChangeStreamErrorTest, ChangeStreamFatalErrorIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableFatalError);
    triggerErrorCounter(ErrorCodes::ChangeStreamFatalError);
    ASSERT_EQ(
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableFatalError),
        before + 1);
}

TEST(RecordChangeStreamErrorTest, BSONObjectTooLargeIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = readCounter(
        capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableBsonObjectTooLarge);
    triggerErrorCounter(ErrorCodes::BSONObjectTooLarge);
    ASSERT_EQ(
        readCounter(capturer,
                    otel::metrics::MetricNames::kChangeStreamErrorNonRetriableBsonObjectTooLarge),
        before + 1);
}

TEST(RecordChangeStreamErrorTest, InterruptedDueToReplStateChangeIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = readCounter(
        capturer,
        otel::metrics::MetricNames::kChangeStreamErrorRetriableInterruptedDueToReplStateChange);
    triggerErrorCounter(ErrorCodes::InterruptedDueToReplStateChange);
    ASSERT_EQ(
        readCounter(
            capturer,
            otel::metrics::MetricNames::kChangeStreamErrorRetriableInterruptedDueToReplStateChange),
        before + 1);
}

// NetworkTimeout is retriable but not a named change stream error — goes to retriable.other.
TEST(RecordChangeStreamErrorTest, RetriableOtherIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorRetriableOther);
    triggerErrorCounter(ErrorCodes::NetworkTimeout);
    ASSERT_EQ(readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorRetriableOther),
              before + 1);
}

// ShardRemovedError is a NonResumableChangeStreamError but has no dedicated counter — goes to
// nonRetriable.other via the category fallback in incrementChangeStreamErrorCounters().
TEST(RecordChangeStreamErrorTest, ShardRemovedErrorIncrementsNonRetriableOther) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableOther);
    triggerErrorCounter(ErrorCodes::ShardRemovedError);
    ASSERT_EQ(
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableOther),
        before + 1);
}

// BadValue is non-retriable and not a named change stream error — goes to nonRetriable.other.
TEST(RecordChangeStreamErrorTest, NonRetriableOtherIncrements) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableOther);
    triggerErrorCounter(ErrorCodes::BadValue);
    ASSERT_EQ(
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableOther),
        before + 1);
}

// CloseChangeStream has no error category, so it falls through to nonRetriable.other inside
// incrementChangeStreamErrorCounters. Callers (e.g. getmore_cmd.cpp, run_aggregate.cpp) must guard
// against this code before calling incrementChangeStreamErrorCounters to avoid miscounting
// lifecycle events.
TEST(RecordChangeStreamErrorTest, CloseChangeStreamIncrementsNonRetriableOther) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before =
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableOther);
    triggerErrorCounter(ErrorCodes::CloseChangeStream);
    ASSERT_EQ(
        readCounter(capturer, otel::metrics::MetricNames::kChangeStreamErrorNonRetriableOther),
        before + 1);
}


}  // namespace
}  // namespace mongo::change_stream
