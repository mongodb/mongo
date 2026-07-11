// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/rate_limiter_otel_metrics_recorder.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::admission {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

using namespace mongo::literals;

// The recorder's instruments carry no attributes, so every recorder registering these names uses
// the identical (empty) definition. Each test case constructs its own OtelMetricsCapturer first,
// which reinitializes the MetricsService and resets recorded values, so the cases stay isolated
// despite sharing instrument names.
RateLimiterOtelMetricsRecorder::MetricsSpec makeSpec() {
    return RateLimiterOtelMetricsRecorder::MetricsSpec{
        .attemptedAdmissions = MetricNames::kIngressRequestRateLimiterAttemptedAdmissions,
        .successfulAdmissions = MetricNames::kIngressRequestRateLimiterSuccessfulAdmissions,
        .rejectedAdmissions = MetricNames::kIngressRequestRateLimiterRejectedAdmissions,
        .exemptedAdmissions = MetricNames::kIngressRequestRateLimiterExemptedAdmissions,
        .addedToQueue = MetricNames::kIngressRequestRateLimiterAddedToQueue,
        .removedFromQueue = MetricNames::kIngressRequestRateLimiterRemovedFromQueue,
        .interruptedInQueue = MetricNames::kIngressRequestRateLimiterInterruptedInQueue,
        .tokensAcquired = MetricNames::kIngressRequestRateLimiterTokensAcquired,
        .currentQueueDepth = MetricNames::kIngressRequestRateLimiterCurrentQueueDepth,
        .totalAvailableTokens = MetricNames::kIngressRequestRateLimiterTotalAvailableTokens,
        .averageTimeQueuedMicros = MetricNames::kIngressRequestRateLimiterAverageTimeQueuedMicros,
        .timeQueuedMicros = MetricNames::kIngressRequestRateLimiterTimeQueuedMicros,
    };
}

TEST(RateLimiterMetricsRecorderOtelTest, EmitsCountersAndLabels) {
    RateLimiterOtelMetricsRecorder recorder{makeSpec()};

    recorder.record(AttemptedAdmission{});
    recorder.record(SuccessfulAdmission{3.0});
    recorder.record(RejectedAdmission{});
    recorder.record(ExemptedAdmission{});
    recorder.record(AddedToQueue{});
    recorder.record(RemovedFromQueue{});
    recorder.record(InterruptedInQueue{});
    recorder.record(TokensAcquired{2.5});

    ASSERT_EQ(recorder.attemptedAdmissions(), 1);
    ASSERT_EQ(recorder.successfulAdmissions(), 1);
    ASSERT_EQ(recorder.rejectedAdmissions(), 1);
    ASSERT_EQ(recorder.exemptedAdmissions(), 1);
    ASSERT_EQ(recorder.addedToQueue(), 1);
    ASSERT_EQ(recorder.removedFromQueue(), 1);
    ASSERT_EQ(recorder.interruptedInQueue(), 1);
    ASSERT_EQ(recorder.tokensAcquired(), 5.5);
}

TEST(RateLimiterMetricsRecorderOtelTest, EmitsGaugeAndMovingAverageWithLabels) {
    RateLimiterOtelMetricsRecorder recorder{makeSpec()};

    recorder.record(AddedToQueue{});
    recorder.record(AddedToQueue{});
    recorder.record(RemovedFromQueue{});
    recorder.record(TokensAvailable{42.8});
    recorder.record(AverageTimeQueuedMicros{120.0});
    recorder.record(AverageTimeQueuedMicros{80.0});

    ASSERT_EQ(recorder.currentQueueDepth(), 1);
    ASSERT_EQ(recorder.tokensAvailable(), 42.8);
    ASSERT_TRUE(recorder.averageTimeQueuedMicros());
    ASSERT_NEAR(*recorder.averageTimeQueuedMicros(), 112.0, 1e-9);
}

TEST(RateLimiterMetricsRecorderOtelTest, RecordsQueueTimeDistributionHistogram) {
    OtelMetricsCapturer capturer;

    RateLimiterOtelMetricsRecorder recorder{makeSpec()};

    // Each AverageTimeQueuedMicros sample feeds both the EMA gauge and the histogram. These four
    // samples land in the buckets bounded by {50, 100, 250, 500, 1000, ...}: 30 -> (-inf, 50],
    // 120 -> (100, 250], 120 -> (100, 250], 600 -> (500, 1000].
    recorder.record(AverageTimeQueuedMicros{30.0});
    recorder.record(AverageTimeQueuedMicros{120.0});
    recorder.record(AverageTimeQueuedMicros{120.0});
    recorder.record(AverageTimeQueuedMicros{600.0});

    if (!capturer.canReadMetrics()) {
        // The histogram can only be observed through the capturer. When it is unavailable, fall
        // back to validating the moving average, which is tracked in-process and reads back through
        // the recorder. With alpha 0.2 the samples 30, 120, 120, 600 produce a moving average of
        // 169.92.
        ASSERT_TRUE(recorder.averageTimeQueuedMicros());
        ASSERT_NEAR(*recorder.averageTimeQueuedMicros(), 169.92, 1e-9);
        return;
    }

    auto histogram =
        capturer.readInt64Histogram(MetricNames::kIngressRequestRateLimiterTimeQueuedMicros);

    ASSERT_EQ(histogram.count, 4u);
    ASSERT_EQ(histogram.sum, 30 + 120 + 120 + 600);
    ASSERT_EQ(histogram.min, 30);
    ASSERT_EQ(histogram.max, 600);

    // boundaries = {50, 100, 250, 500, 1000, ...}; counts has one extra bucket for (lastBound,
    // +inf). Bucket index i covers (boundaries[i-1], boundaries[i]].
    ASSERT_EQ(histogram.boundaries.front(), 50.0);
    ASSERT_EQ(histogram.counts.size(), histogram.boundaries.size() + 1);
    ASSERT_EQ(histogram.counts[0], 1u);  // (-inf, 50]   : 30
    ASSERT_EQ(histogram.counts[2], 2u);  // (100, 250]   : 120, 120
    ASSERT_EQ(histogram.counts[4], 1u);  // (500, 1000]  : 600
}

}  // namespace
}  // namespace mongo::admission
