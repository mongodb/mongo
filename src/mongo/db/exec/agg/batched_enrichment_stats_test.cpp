// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/batched_enrichment_stats.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::exec::agg {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

// Each recordBatchStarted() call increments the change-stream cell's counter.
TEST(BatchedEnrichmentStatsTest, ChangeStreamRecorderIncrementsBatchesStarted) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const auto before =
        capturer.readInt64Counter(MetricNames::kChangeStreamUpdateLookupEnrichBatchesStarted);

    BatchedEnrichmentStatsRecorder recorder =
        BatchedEnrichmentStatsRecorder::makeChangeStreamUpdateLookupRecorder();
    recorder.recordBatchStarted();
    recorder.recordBatchStarted();

    const auto after =
        capturer.readInt64Counter(MetricNames::kChangeStreamUpdateLookupEnrichBatchesStarted);
    ASSERT_EQ(after, before + 2);
}

// Same shape, for the search cell.
TEST(BatchedEnrichmentStatsTest, SearchRecorderIncrementsBatchesStarted) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const auto before = capturer.readInt64Counter(MetricNames::kSearchIdLookupEnrichBatchesStarted);

    BatchedEnrichmentStatsRecorder recorder =
        BatchedEnrichmentStatsRecorder::makeSearchIdLookupRecorder();
    recorder.recordBatchStarted();

    const auto after = capturer.readInt64Counter(MetricNames::kSearchIdLookupEnrichBatchesStarted);
    ASSERT_EQ(after, before + 1);
}

// The consumers' cells are independent: recording into one leaves the other untouched.
TEST(BatchedEnrichmentStatsTest, ConsumerCellsAreIndependent) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const auto csBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamUpdateLookupEnrichBatchesStarted);
    const auto searchBefore =
        capturer.readInt64Counter(MetricNames::kSearchIdLookupEnrichBatchesStarted);

    BatchedEnrichmentStatsRecorder::makeSearchIdLookupRecorder().recordBatchStarted();

    const auto csAfter =
        capturer.readInt64Counter(MetricNames::kChangeStreamUpdateLookupEnrichBatchesStarted);
    const auto searchAfter =
        capturer.readInt64Counter(MetricNames::kSearchIdLookupEnrichBatchesStarted);
    ASSERT_EQ(searchAfter, searchBefore + 1);
    ASSERT_EQ(csAfter, csBefore);
}

// The noop test double must not touch either production cell.
TEST(BatchedEnrichmentStatsTest, NoopRecorderDoesNotTouchProductionCells) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    const auto csBefore =
        capturer.readInt64Counter(MetricNames::kChangeStreamUpdateLookupEnrichBatchesStarted);
    const auto searchBefore =
        capturer.readInt64Counter(MetricNames::kSearchIdLookupEnrichBatchesStarted);

    BatchedEnrichmentStatsRecorder::makeNoopRecorder_forTest().recordBatchStarted();

    const auto csAfter =
        capturer.readInt64Counter(MetricNames::kChangeStreamUpdateLookupEnrichBatchesStarted);
    const auto searchAfter =
        capturer.readInt64Counter(MetricNames::kSearchIdLookupEnrichBatchesStarted);
    ASSERT_EQ(csAfter, csBefore);
    ASSERT_EQ(searchAfter, searchBefore);
}

}  // namespace
}  // namespace mongo::exec::agg
