// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#include <set>
#include <string>

#include <boost/optional.hpp>

namespace mongo::change_stream {
namespace {

// Match the cursor components' static registration pattern, including in this standalone binary.
auto& cursorsTotalOpened = createCurorsTotalOpened();
auto& cursorsLifespan = createCursorsLifespan();
auto& cursorsOpenTotal = createCursorsOpenTotal();
auto& cursorsOpenPinned = createCursorsOpenPinned();

TEST(RecordCursorOptionMetricsTest, StaticCursorRegistrationsUseCompleteOptions) {
    // Repeated factory calls must agree with the definitions registered before main().
    ASSERT_EQ(&createCurorsTotalOpened(), &cursorsTotalOpened);
    ASSERT_EQ(&createCursorsLifespan(), &cursorsLifespan);
    ASSERT_EQ(&createCursorsOpenTotal(), &cursorsOpenTotal);
    ASSERT_EQ(&createCursorsOpenPinned(), &cursorsOpenPinned);

    BSONObjBuilder builder;
    globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    auto root = builder.obj();
    auto cursor =
        root.getObjectField("metrics").getObjectField("changeStreams").getObjectField("cursor");
    ASSERT(cursor["totalOpened"].isNumber());
    ASSERT(cursor.getObjectField("lifespan")["totalCount"].isNumber());
    auto open = cursor.getObjectField("open");
    ASSERT(open["total"].isNumber());
    ASSERT(open["pinned"].isNumber());
}

// Warms up both histograms with a value that always lands in the first bucket, so that the
// "before" snapshot in each test below is guaranteed to already have a data point (avoiding
// KeyNotFound on the very first read in the binary).
void warmUp() {
    recordCursorBatchSize(0);
    recordCursorMaxAwaitTimeMS(0);
}

// Reads the actual serverStatus tree, including when OTel is disabled on Windows.
BSONObj readCursorOptionMetrics() {
    BSONObjBuilder builder;
    globalMetricTreeSet()[ClusterRole::None].appendTo(builder);
    return builder.obj()
        .getObjectField("metrics")
        .getObjectField("changeStreams")
        .getObjectField("option")
        .getObjectField("cursor")
        .getOwned();
}

TEST(RecordCursorOptionMetricsTest, RepeatedRecordingIsReflectedInServerStatus) {
    // The option histograms must already be registered before the first command records them.
    auto before = readCursorOptionMetrics();

    recordCursorBatchSize(7);
    recordCursorBatchSize(7);
    recordCursorMaxAwaitTimeMS(700);
    recordCursorMaxAwaitTimeMS(700);

    auto after = readCursorOptionMetrics();

    auto assertTwoRecords = [](const BSONObj& before, const BSONObj& after, const char* bucket) {
        ASSERT_EQ(after.getIntField("totalCount"), before.getIntField("totalCount") + 2);
        ASSERT_EQ(after.getObjectField(bucket).getIntField("count"),
                  before.getObjectField(bucket).getIntField("count") + 2);
    };
    assertTwoRecords(
        before.getObjectField("batchSize"), after.getObjectField("batchSize"), "[1, 10)");
    assertTwoRecords(before.getObjectField("maxAwaitTimeMS"),
                     after.getObjectField("maxAwaitTimeMS"),
                     "[500, 1000)");
}

TEST(RecordCursorOptionMetricsTest, OnlyBatchSizeAndMaxAwaitTimeMSAreExposed) {
    auto cursor = readCursorOptionMetrics();
    auto fieldNames = cursor.getFieldNames<std::set<std::string>>();
    ASSERT_EQ(fieldNames, (std::set<std::string>{"batchSize", "maxAwaitTimeMS"}));
    ASSERT(cursor["maxTimeMS"].eoo());
}

TEST(RecordCursorOptionMetricsTest, BatchSizeIsRecordedWithCorrectValue) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    recordCursorBatchSize(42);
    auto after =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);

    ASSERT_EQ(after.count, before.count + 1);
    ASSERT_EQ(after.sum, before.sum + 42);
}

TEST(RecordCursorOptionMetricsTest, UnsetBatchSizeDoesNotRecord) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    recordCursorBatchSize(boost::none);
    auto after =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);

    ASSERT_EQ(after.count, before.count);
    ASSERT_EQ(after.sum, before.sum);
}

TEST(RecordCursorOptionMetricsTest, MaxAwaitTimeMSIsRecordedWithCorrectValue) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before = capturer.readInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS);
    recordCursorMaxAwaitTimeMS(12345);
    auto after = capturer.readInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS);

    ASSERT_EQ(after.count, before.count + 1);
    ASSERT_EQ(after.sum, before.sum + 12345);
}

TEST(RecordCursorOptionMetricsTest, BucketBoundariesAreRespected) {
    // Bucket boundaries are 100, 200, ..., 1000000, and buckets are [lower, upper).
    recordCursorMaxAwaitTimeMS(0);  // Ensure the histogram has data points.
    auto before = readCursorOptionMetrics().getObjectField("maxAwaitTimeMS").getOwned();
    recordCursorMaxAwaitTimeMS(99);
    recordCursorMaxAwaitTimeMS(100);
    recordCursorMaxAwaitTimeMS(999);
    recordCursorMaxAwaitTimeMS(1000);
    auto after = readCursorOptionMetrics().getObjectField("maxAwaitTimeMS").getOwned();

    ASSERT_EQ(after.getIntField("totalCount"), before.getIntField("totalCount") + 4);
    auto delta = [&](const char* bucket) {
        return after.getObjectField(bucket).getIntField("count") -
            before.getObjectField(bucket).getIntField("count");
    };
    ASSERT_EQ(delta("(-inf, 100)"), 1);
    ASSERT_EQ(delta("[100, 200)"), 1);
    ASSERT_EQ(delta("[500, 1000)"), 1);
    ASSERT_EQ(delta("[1000, 2000)"), 1);
}

TEST(RecordCursorOptionMetricsTest, UnsetMaxAwaitTimeMSDoesNotRecord) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto before = capturer.readInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS);
    recordCursorMaxAwaitTimeMS(boost::none);
    auto after = capturer.readInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS);

    ASSERT_EQ(after.count, before.count);
    ASSERT_EQ(after.sum, before.sum);
}

TEST(RecordCursorOptionMetricsTest, OptionsAreRecordedIndependently) {
    otel::metrics::OtelMetricsCapturer capturer;
    if (!capturer.canReadMetrics()) {
        return;
    }
    warmUp();

    auto beforeBatchSize =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    auto beforeMaxAwaitTimeMS = capturer.readInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS);

    recordCursorMaxAwaitTimeMS(700);

    auto afterBatchSize =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    auto afterMaxAwaitTimeMS = capturer.readInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS);
    ASSERT_EQ(afterBatchSize.count, beforeBatchSize.count);
    ASSERT_EQ(afterMaxAwaitTimeMS.count, beforeMaxAwaitTimeMS.count + 1);

    recordCursorBatchSize(7);

    auto finalBatchSize =
        capturer.readInt64Histogram(otel::metrics::MetricNames::kChangeStreamOptionCursorBatchSize);
    auto finalMaxAwaitTimeMS = capturer.readInt64Histogram(
        otel::metrics::MetricNames::kChangeStreamOptionCursorMaxAwaitTimeMS);
    ASSERT_EQ(finalBatchSize.count, beforeBatchSize.count + 1);
    ASSERT_EQ(finalBatchSize.sum, beforeBatchSize.sum + 7);
    ASSERT_EQ(finalMaxAwaitTimeMS.count, afterMaxAwaitTimeMS.count);
}

}  // namespace
}  // namespace mongo::change_stream
