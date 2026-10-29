// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"

#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats_test_util.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo::exec {
namespace {

using otel::metrics::OtelMetricsCapturer;

// Recording into the express cell increments the matching outcome counter and, for the lookups that
// actually ran (found / not-found), the latency histogram; a declined (not-handled) lookup records
// no latency.
TEST(SingleDocumentLookupStatsTest, ExpressRecorderUpdatesCountersAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = snapshotExpressCell(capturer);

    SingleDocumentLookupStatsRecorder recorder =
        SingleDocumentLookupStatsRecorder::makeUpdateLookupExpressRecorder();
    recorder.recordFound(Microseconds{100});
    recorder.recordNotFound(Microseconds{200});
    recorder.recordNotHandled();

    auto after = snapshotExpressCell(capturer);
    ASSERT_EQ(after.found, before.found + 1);
    ASSERT_EQ(after.notFound, before.notFound + 1);
    ASSERT_EQ(after.notHandled, before.notHandled + 1);

    // Latency recorded for found + not-found only; not-handled contributes nothing.
    ASSERT_EQ(after.latencyCount, before.latencyCount + 2);
    ASSERT_EQ(after.latencySum, before.latencySum + 300);
}

// The express and aggregation cells are independent: recording into one leaves the other untouched.
TEST(SingleDocumentLookupStatsTest, AggregationCellIsIndependentOfExpress) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto expressBefore = snapshotExpressCell(capturer);
    auto aggBefore = snapshotAggregationCell(capturer);

    SingleDocumentLookupStatsRecorder recorder =
        SingleDocumentLookupStatsRecorder::makeUpdateLookupAggregationRecorder();
    recorder.recordFound(Microseconds{500});

    auto expressAfter = snapshotExpressCell(capturer);
    auto aggAfter = snapshotAggregationCell(capturer);
    ASSERT_EQ(aggAfter.found, aggBefore.found + 1);
    ASSERT_EQ(expressAfter.found, expressBefore.found);
}

// Same shape as the express test, for the change-stream SBE cell.
TEST(SingleDocumentLookupStatsTest, SbeRecorderUpdatesCountersAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = snapshotSbeCell(capturer);

    SingleDocumentLookupStatsRecorder recorder =
        SingleDocumentLookupStatsRecorder::makeUpdateLookupSbeRecorder();
    recorder.recordFound(Microseconds{100});
    recorder.recordNotFound(Microseconds{200});
    recorder.recordNotHandled();

    auto after = snapshotSbeCell(capturer);
    ASSERT_EQ(after.found, before.found + 1);
    ASSERT_EQ(after.notFound, before.notFound + 1);
    ASSERT_EQ(after.notHandled, before.notHandled + 1);
    ASSERT_EQ(after.latencyCount, before.latencyCount + 2);
    ASSERT_EQ(after.latencySum, before.latencySum + 300);
}

// Same shape as the express test, for the search SBE cell.
TEST(SingleDocumentLookupStatsTest, SearchSbeRecorderUpdatesCountersAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = snapshotSearchSbeCell(capturer);

    SingleDocumentLookupStatsRecorder recorder =
        SingleDocumentLookupStatsRecorder::makeSearchIdLookupSbeRecorder();
    recorder.recordFound(Microseconds{100});
    recorder.recordNotFound(Microseconds{200});
    recorder.recordNotHandled();

    auto after = snapshotSearchSbeCell(capturer);
    ASSERT_EQ(after.found, before.found + 1);
    ASSERT_EQ(after.notFound, before.notFound + 1);
    ASSERT_EQ(after.notHandled, before.notHandled + 1);
    ASSERT_EQ(after.latencyCount, before.latencyCount + 2);
    ASSERT_EQ(after.latencySum, before.latencySum + 300);
}

// Same shape as the express test, for the search aggregation (local-read) cell.
TEST(SingleDocumentLookupStatsTest, SearchAggregationRecorderUpdatesCountersAndLatency) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto before = snapshotSearchAggregationCell(capturer);

    SingleDocumentLookupStatsRecorder recorder =
        SingleDocumentLookupStatsRecorder::makeSearchIdLookupAggregationRecorder();
    recorder.recordFound(Microseconds{100});
    recorder.recordNotFound(Microseconds{200});
    recorder.recordNotHandled();

    auto after = snapshotSearchAggregationCell(capturer);
    ASSERT_EQ(after.found, before.found + 1);
    ASSERT_EQ(after.notFound, before.notFound + 1);
    ASSERT_EQ(after.notHandled, before.notHandled + 1);
    ASSERT_EQ(after.latencyCount, before.latencyCount + 2);
    ASSERT_EQ(after.latencySum, before.latencySum + 300);
}

// The search cells are independent of each other and of every change-stream cell: a recorder
// bound to the wrong cell would corrupt another consumer's metrics.
TEST(SingleDocumentLookupStatsTest, SearchCellsAreIndependentOfChangeStreamCells) {
    OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }

    auto expressBefore = snapshotExpressCell(capturer);
    auto aggBefore = snapshotAggregationCell(capturer);
    auto sbeBefore = snapshotSbeCell(capturer);
    auto searchSbeBefore = snapshotSearchSbeCell(capturer);
    auto searchAggBefore = snapshotSearchAggregationCell(capturer);

    SingleDocumentLookupStatsRecorder::makeSearchIdLookupSbeRecorder().recordFound(Microseconds{1});
    SingleDocumentLookupStatsRecorder::makeSearchIdLookupAggregationRecorder().recordNotFound(
        Microseconds{1});

    auto expressAfter = snapshotExpressCell(capturer);
    auto aggAfter = snapshotAggregationCell(capturer);
    auto sbeAfter = snapshotSbeCell(capturer);
    auto searchSbeAfter = snapshotSearchSbeCell(capturer);
    auto searchAggAfter = snapshotSearchAggregationCell(capturer);

    ASSERT_EQ(searchSbeAfter.found, searchSbeBefore.found + 1);
    ASSERT_EQ(searchAggAfter.notFound, searchAggBefore.notFound + 1);
    ASSERT_EQ(searchSbeAfter.notFound, searchSbeBefore.notFound);
    ASSERT_EQ(searchAggAfter.found, searchAggBefore.found);

    ASSERT_EQ(expressAfter.found + expressAfter.notFound + expressAfter.notHandled,
              expressBefore.found + expressBefore.notFound + expressBefore.notHandled);
    ASSERT_EQ(aggAfter.found + aggAfter.notFound + aggAfter.notHandled,
              aggBefore.found + aggBefore.notFound + aggBefore.notHandled);
    ASSERT_EQ(sbeAfter.found + sbeAfter.notFound + sbeAfter.notHandled,
              sbeBefore.found + sbeBefore.notFound + sbeBefore.notHandled);
}

}  // namespace
}  // namespace mongo::exec
