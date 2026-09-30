// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/replication_session_metrics.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/unittest.h"

#ifdef __linux__
#include "mongo/util/fail_point.h"
namespace mongo {
extern void runReplicationSessionCollectionCycle(SessionMetrics& metrics);
}
#endif

namespace mongo {
namespace {
using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;

class SessionOtelMetricsTest : public unittest::Test {
protected:
    void setUp() override {
        if (!OtelMetricsCapturer::canReadMetrics()) {
            GTEST_SKIP() << "Skipping test: OTel metrics unavailable on this platform";
        }
    }

    OtelMetricsCapturer _capturer;
    SessionMetrics _metrics;
};

TEST_F(SessionOtelMetricsTest, UpdateRecordsEachFieldToItsHistogram) {
    _metrics.update({
        {.congestionWindowSizeBytes = 10,
         .receiveBufferSizeBytes = 1000,
         .receiveBufferBytes = 100},
        {.congestionWindowSizeBytes = 30,
         .receiveBufferSizeBytes = 3000,
         .receiveBufferBytes = 300},
    });

    auto cwnd =
        _capturer.readInt64Histogram(MetricNames::kReplicationSecondaryTcpCongestionWindowSize);
    ASSERT_EQ(cwnd.count, 2);
    ASSERT_EQ(cwnd.sum, 40);
    ASSERT_EQ(cwnd.min, 10);
    ASSERT_EQ(cwnd.max, 30);

    auto rcvSize = _capturer.readInt64Histogram(MetricNames::kReplicationSecondaryReceiveQueueSize);
    ASSERT_EQ(rcvSize.count, 2);
    ASSERT_EQ(rcvSize.sum, 4000);
    ASSERT_EQ(rcvSize.min, 1000);
    ASSERT_EQ(rcvSize.max, 3000);

    auto rcvBytes =
        _capturer.readInt64Histogram(MetricNames::kReplicationSecondaryReceiveQueueBytes);
    ASSERT_EQ(rcvBytes.count, 2);
    ASSERT_EQ(rcvBytes.sum, 400);
    ASSERT_EQ(rcvBytes.min, 100);
    ASSERT_EQ(rcvBytes.max, 300);
}

#ifdef __linux__
TEST_F(SessionOtelMetricsTest, CollectErrorsIncrementOnFailure) {
    FailPointEnableBlock fp("failCollectReplicationSessionStats");
    runReplicationSessionCollectionCycle(_metrics);
    ASSERT_EQ(1, _capturer.readInt64Counter(MetricNames::kReplicationSecondaryCollectErrors));
}
#endif
}  // namespace
}  // namespace mongo
