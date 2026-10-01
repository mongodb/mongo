// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/historical_placement_fetcher_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

// Reads the latency histogram's count/sum, defaulting to zero if it has no recorded observations
// yet (an OTel histogram with no points is not present in the collected data, unlike counters).
std::pair<uint64_t, int64_t> readLatencyHistogramOrZero(OtelMetricsCapturer& capturer) {
    try {
        auto latency = capturer.readInt64Histogram(
            MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupLatencyMillis);
        return {latency.count, latency.sum};
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::KeyNotFound) {
            return {0, 0};
        }
        throw;
    }
}

// Every placement-history outcome increments its own counter and records one latency observation.
TEST(HistoricalPlacementFetcherMetricsTest, LookupOutcomeUpdatesMatchingCounterAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto okBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupOk);
    auto futureClusterTimeBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupFutureClusterTime);
    auto notAvailableBefore = capturer.readInt64Counter(
        MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupNotAvailable);
    auto [latencyCountBefore, latencySumBefore] = readLatencyHistogramOrZero(capturer);

    auto metrics = getHistoricalPlacementFetcherMetricsRecorder();
    metrics.recordLookup(HistoricalPlacementStatus::OK, Milliseconds{5});
    metrics.recordLookup(HistoricalPlacementStatus::FutureClusterTime, Milliseconds{10});
    metrics.recordLookup(HistoricalPlacementStatus::NotAvailable, Milliseconds{25});

    ASSERT_EQ(
        capturer.readInt64Counter(MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupOk),
        okBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupFutureClusterTime),
              futureClusterTimeBefore + 1);
    ASSERT_EQ(capturer.readInt64Counter(
                  MetricNames::kChangeStreamShardTargetingPlacementHistoryLookupNotAvailable),
              notAvailableBefore + 1);

    auto [latencyCountAfter, latencySumAfter] = readLatencyHistogramOrZero(capturer);
    ASSERT_EQ(latencyCountAfter, latencyCountBefore + 3);
    ASSERT_EQ(latencySumAfter, latencySumBefore + 40);
}

}  // namespace
}  // namespace mongo
