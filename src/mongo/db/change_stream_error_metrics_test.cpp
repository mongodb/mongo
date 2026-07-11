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
