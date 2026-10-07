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
using otel::metrics::HasCount;
using otel::metrics::HasSum;
using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;
using testing::AllOf;

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

TEST_F(SessionOtelMetricsTest, UpdateRecordsEgressFieldsToHistograms) {
    _metrics.update({
        {.isIngress = false,
         .congestionWindowSizeBytes = 10,
         .receiveBufferSizeBytes = 1000,
         .receiveBufferBytes = 100},
        {.isIngress = false,
         .congestionWindowSizeBytes = 30,
         .receiveBufferSizeBytes = 3000,
         .receiveBufferBytes = 300},
    });

    EXPECT_THAT(
        _capturer.readInt64Histogram(MetricNames::kReplicationSecondaryTcpCongestionWindowSize),
        AllOf(HasSum(40), HasCount(2)));

    EXPECT_THAT(_capturer.readInt64Histogram(MetricNames::kReplicationSecondaryReceiveQueueSize),
                AllOf(HasSum(4000), HasCount(2)));

    EXPECT_THAT(_capturer.readInt64Histogram(MetricNames::kReplicationSecondaryReceiveQueueBytes),
                AllOf(HasSum(400), HasCount(2)));
}

TEST_F(SessionOtelMetricsTest, UpdateRecordsIngressFieldsToHistograms) {
    _metrics.update({
        {.isIngress = true,
         .peerId = 0,
         .congestionWindowSizeBytes = 10,
         .sendBufferSizeBytes = 1000,
         .sendBufferBytes = 100},
        {.isIngress = true,
         .peerId = 1,
         .congestionWindowSizeBytes = 30,
         .sendBufferSizeBytes = 3000,
         .sendBufferBytes = 300},
    });

    auto peer0 = std::make_tuple(int64_t{0});
    auto peer1 = std::make_tuple(int64_t{1});

    EXPECT_THAT(_capturer.readInt64Histogram<int64_t>(
                    MetricNames::kReplicationPrimaryTcpCongestionWindowSize, peer0),
                AllOf(HasSum(10), HasCount(1)));
    EXPECT_THAT(_capturer.readInt64Histogram<int64_t>(
                    MetricNames::kReplicationPrimaryTcpCongestionWindowSize, peer1),
                AllOf(HasSum(30), HasCount(1)));
    EXPECT_THAT(
        _capturer.readInt64Histogram<int64_t>(MetricNames::kReplicationPrimarySendQueueSize, peer0),
        AllOf(HasSum(1000), HasCount(1)));
    EXPECT_THAT(
        _capturer.readInt64Histogram<int64_t>(MetricNames::kReplicationPrimarySendQueueSize, peer1),
        AllOf(HasSum(3000), HasCount(1)));
    EXPECT_THAT(_capturer.readInt64Histogram<int64_t>(
                    MetricNames::kReplicationPrimarySendQueueBytes, peer0),
                AllOf(HasSum(100), HasCount(1)));
    EXPECT_THAT(_capturer.readInt64Histogram<int64_t>(
                    MetricNames::kReplicationPrimarySendQueueBytes, peer1),
                AllOf(HasSum(300), HasCount(1)));
}

// The below tests that when updated with metrics that contain egress metrics but tagged as ingress,
// only ingress values are updated (and vice versa).
TEST_F(SessionOtelMetricsTest, IngressOrEgressIgnoredAsAppropriate) {
    _metrics.update({
        {.isIngress = true,
         .peerId = 0,
         .congestionWindowSizeBytes = 10,
         .receiveBufferSizeBytes = 1,
         .sendBufferSizeBytes = 1000,
         .receiveBufferBytes = 1,
         .sendBufferBytes = 100},
        {.isIngress = false,
         .congestionWindowSizeBytes = 30,
         .receiveBufferSizeBytes = 3000,
         .sendBufferSizeBytes = 1,
         .receiveBufferBytes = 300,
         .sendBufferBytes = 1},
    });

    auto peer = std::make_tuple(int64_t{0});

    EXPECT_THAT(_capturer.readInt64Histogram<int64_t>(
                    MetricNames::kReplicationPrimaryTcpCongestionWindowSize, peer),
                AllOf(HasSum(10), HasCount(1)));
    EXPECT_THAT(
        _capturer.readInt64Histogram<int64_t>(MetricNames::kReplicationPrimarySendQueueSize, peer),
        AllOf(HasSum(1000), HasCount(1)));
    EXPECT_THAT(
        _capturer.readInt64Histogram<int64_t>(MetricNames::kReplicationPrimarySendQueueBytes, peer),
        AllOf(HasSum(100), HasCount(1)));

    EXPECT_THAT(
        _capturer.readInt64Histogram(MetricNames::kReplicationSecondaryTcpCongestionWindowSize),
        AllOf(HasSum(30), HasCount(1)));
    EXPECT_THAT(_capturer.readInt64Histogram(MetricNames::kReplicationSecondaryReceiveQueueSize),
                AllOf(HasSum(3000), HasCount(1)));
    EXPECT_THAT(_capturer.readInt64Histogram(MetricNames::kReplicationSecondaryReceiveQueueBytes),
                AllOf(HasSum(300), HasCount(1)));
}

#ifdef __linux__
TEST_F(SessionOtelMetricsTest, CollectErrorsIncrementOnFailure) {
    FailPointEnableBlock fp("failCollectReplicationSessionStats", BSON("errorCount" << 4));
    runReplicationSessionCollectionCycle(_metrics);
    EXPECT_EQ(4, _capturer.readInt64Counter(MetricNames::kReplicationCollectErrors));
    runReplicationSessionCollectionCycle(_metrics);
    EXPECT_EQ(8, _capturer.readInt64Counter(MetricNames::kReplicationCollectErrors));
}
#endif
}  // namespace
}  // namespace mongo
