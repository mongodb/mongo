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

}  // namespace
}  // namespace mongo::exec
