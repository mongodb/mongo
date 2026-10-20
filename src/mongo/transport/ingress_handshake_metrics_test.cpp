// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/transport/ingress_handshake_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/optional.hpp>
#include <fmt/format.h>

namespace mongo::transport {
namespace {

using otel::metrics::MetricNames;
using otel::metrics::OtelMetricsCapturer;
using namespace testing;

class MockHandshakeCommand : public Command {
public:
    MockHandshakeCommand(std::string name, HandshakeRole role)
        : Command(std::move(name)), _role(role) {}

    std::unique_ptr<CommandInvocation> parse(OperationContext*, const OpMsgRequest&) override {
        MONGO_UNREACHABLE;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    HandshakeRole handshakeRole() const override {
        return _role;
    }

private:
    HandshakeRole _role;
};

TEST(IngressHandshakeMetricsTest, TLSHandshakeLatencyRecordsExactElapsedTime) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    TickSourceMock<Milliseconds> tickSource;
    IngressHandshakeMetrics metrics;

    metrics.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(123));
    metrics.onTLSHandshakeCompleted();

    const auto histogram = capturer.readInt64Histogram(MetricNames::kIngressTLSHandshakeLatency);
    EXPECT_EQ(histogram.count, 1);
    EXPECT_EQ(histogram.sum, 123000);
}

TEST(IngressHandshakeMetricsTest, TLSHandshakeLatencyAccumulatesAcrossSessions) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    TickSourceMock<Milliseconds> tickSource;

    IngressHandshakeMetrics first;
    first.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(100));
    first.onTLSHandshakeCompleted();

    IngressHandshakeMetrics second;
    second.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(20));
    second.onTLSHandshakeCompleted();

    const auto histogram = capturer.readInt64Histogram(MetricNames::kIngressTLSHandshakeLatency);
    EXPECT_EQ(histogram.count, 2);
    EXPECT_EQ(histogram.sum, 120000);
}

TEST(IngressHandshakeMetricsTest, TLSHandshakeLatencyNotRecordedWithoutHandshakeStart) {
    OtelMetricsCapturer capturer;
    if (!OtelMetricsCapturer::canReadMetrics()) {
        return;
    }

    TickSourceMock<Milliseconds> tickSource;

    IngressHandshakeMetrics withHandshake;
    withHandshake.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(5));
    withHandshake.onTLSHandshakeCompleted();

    // A session that never observed a TLS handshake start must not contribute a sample.
    IngressHandshakeMetrics withoutHandshake;
    withoutHandshake.onTLSHandshakeCompleted();

    const auto histogram = capturer.readInt64Histogram(MetricNames::kIngressTLSHandshakeLatency);
    EXPECT_EQ(histogram.count, 1);
    EXPECT_EQ(histogram.sum, 5000);
}

// Reads the totalCount of a histogram registered under metrics.network, as exposed in
// serverStatus, or 0 if the metric has never been reported.
long long getNetworkHistogramTotalCount(std::string_view metricName) {
    BSONObjBuilder b;
    appendMergedTrees({&globalMetricTreeSet()[ClusterRole::None]}, b);
    const auto obj = b.obj();
    const auto network = obj.getObjectField("metrics").getObjectField("network");
    const auto elem = network.getField(metricName);
    return elem.eoo() ? 0 : elem.Obj().getIntField("totalCount");
}

TEST(IngressHandshakeMetricsTest, TLSHandshakeServerStatusHistogramUsesBucketCounts) {
    const auto before = getNetworkHistogramTotalCount("ingressTLSHandshakeMicros");

    TickSourceMock<Milliseconds> tickSource;
    IngressHandshakeMetrics metrics;
    metrics.onTLSHandshakeStarted(&tickSource);
    tickSource.advance(Milliseconds(123));
    metrics.onTLSHandshakeCompleted();

    ASSERT_EQ(getNetworkHistogramTotalCount("ingressTLSHandshakeMicros"), before + 1);

    BSONObjBuilder b;
    appendMergedTrees({&globalMetricTreeSet()[ClusterRole::None]}, b);
    const auto obj = b.obj();
    const auto serverStatusHistogram = obj["metrics"]["network"]["ingressTLSHandshakeMicros"].Obj();
    // Besides totalCount, the histogram must report its per-bucket counts (one field per
    // bucket, keyed by range strings).
    ASSERT_GT(serverStatusHistogram.nFields(), 1);
}

// Get the value of the `averageTimeToCompletedAuthMicros` instrument from the metrics tree.
boost::optional<double> getAverageTimeToCompletedAuthMicros() {
    BSONObjBuilder b;
    appendMergedTrees({&globalMetricTreeSet()[ClusterRole::None]}, b);
    const BSONObj obj = b.obj();
    const BSONObj network = obj.getObjectField("metrics").getObjectField("network");
    const BSONElement elem = network.getField("averageTimeToCompletedAuthMicros");
    if (elem.eoo()) {
        return boost::none;
    }
    return elem.Double();
}

// Ensure that we don't try to calculate time to complete an auth if the auth did not complete.
TEST(IngressHandshakeMetricsTest, CompletedAuthMetricOkayWhenCommandNotProcessed) {
    MockHandshakeCommand authCommand("test_only.ingressHandshakeAuth",
                                     Command::HandshakeRole::kAuth);
    MockHandshakeCommand nonAuthCommand("test_only.ingressHandshakeNonAuth",
                                        Command::HandshakeRole::kNone);

    // Set the start time to non-zero.
    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto before = getAverageTimeToCompletedAuthMicros();
    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);

    // Run an auth and then cancel it with a non-auth command.
    tickSource.advance(Microseconds(10));
    metrics.onCommandReceived(&authCommand);
    tickSource.advance(Microseconds(10));
    metrics.onCommandReceived(&nonAuthCommand);

    // No auth happened, so the metric must be unchanged.
    const auto after = getAverageTimeToCompletedAuthMicros();
    ASSERT_EQ(after.has_value(), before.has_value());
    if (before.has_value()) {
        EXPECT_EQ(after, before);
    }
}

TEST(IngressHandshakeMetricsTest, CompletedAuthMetricRecordedOnHandshakeProcessed) {
    MockHandshakeCommand authCommand("test_only.ingressHandshakeAuthProcessed",
                                     Command::HandshakeRole::kAuth);
    MockHandshakeCommand nonAuthCommand("test_only.ingressHandshakeNonAuthProcessed",
                                        Command::HandshakeRole::kNone);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto before = getAverageTimeToCompletedAuthMicros();

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);

    tickSource.advance(Microseconds(10));
    metrics.onCommandReceived(&authCommand);
    tickSource.advance(Microseconds(25));
    metrics.onCommandProcessed(&authCommand, nullptr);

    tickSource.advance(Microseconds(10));
    metrics.onCommandReceived(&nonAuthCommand);

    const auto after = getAverageTimeToCompletedAuthMicros();
    EXPECT_THAT(after, Optional(Gt(0.0)));
    EXPECT_NE(after, before);
}

// A connection that never authenticates only records a completed-hello sample.
TEST(IngressHandshakeMetricsTest, CompletedHelloHistogramNotRecordedForAuth) {
    MockHandshakeCommand helloCommand("test_only.ingressHandshakeHelloHist",
                                      Command::HandshakeRole::kHello);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto helloBefore = getNetworkHistogramTotalCount("ingressHelloLatency");
    const auto authBefore = getNetworkHistogramTotalCount("ingressAuthLatency");

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);
    tickSource.advance(Milliseconds(10));
    metrics.onCommandReceived(&helloCommand);
    tickSource.advance(Milliseconds(5));
    metrics.onCommandProcessed(&helloCommand, nullptr);

    ASSERT_EQ(getNetworkHistogramTotalCount("ingressHelloLatency"), helloBefore + 1);
    ASSERT_EQ(getNetworkHistogramTotalCount("ingressAuthLatency"), authBefore);
}

// A connection that authenticates records both the completed-hello and completed-auth samples.
TEST(IngressHandshakeMetricsTest, CompletedAuthHistogramRecordedWhenAuthCompletes) {
    MockHandshakeCommand helloCommand("test_only.ingressHandshakeHelloAuthHist",
                                      Command::HandshakeRole::kHello);
    MockHandshakeCommand authCommand("test_only.ingressHandshakeAuthHist",
                                     Command::HandshakeRole::kAuth);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto helloBefore = getNetworkHistogramTotalCount("ingressHelloLatency");
    const auto authBefore = getNetworkHistogramTotalCount("ingressAuthLatency");

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);
    metrics.onCommandReceived(&helloCommand);
    metrics.onCommandProcessed(&helloCommand, nullptr);
    tickSource.advance(Milliseconds(7));
    metrics.onCommandReceived(&authCommand);
    metrics.onCommandProcessed(&authCommand, nullptr);
    metrics.onAuthCompleted(&authCommand);

    ASSERT_EQ(getNetworkHistogramTotalCount("ingressHelloLatency"), helloBefore + 1);
    ASSERT_EQ(getNetworkHistogramTotalCount("ingressAuthLatency"), authBefore + 1);
}

// Only one completed-auth sample is recorded per session, even across re-authentications.
TEST(IngressHandshakeMetricsTest, CompletedAuthHistogramRecordsOnlyOncePerSession) {
    MockHandshakeCommand authCommand("test_only.ingressHandshakeAuthOnlyOnce",
                                     Command::HandshakeRole::kAuth);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto authBefore = getNetworkHistogramTotalCount("ingressAuthLatency");

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);
    tickSource.advance(Milliseconds(7));
    metrics.onCommandReceived(&authCommand);
    metrics.onAuthCompleted(&authCommand);
    metrics.onAuthCompleted(&authCommand);

    ASSERT_EQ(getNetworkHistogramTotalCount("ingressAuthLatency"), authBefore + 1);
}

// Authentication that completes via a hello carrying speculative authentication does not record a
// sample in the auth histogram; that latency is already captured by the hello histogram.
TEST(IngressHandshakeMetricsTest, CompletedAuthHistogramNotRecordedForSpeculativeAuth) {
    MockHandshakeCommand helloCommand("test_only.ingressHandshakeSpeculativeAuth",
                                      Command::HandshakeRole::kHello);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto authBefore = getNetworkHistogramTotalCount("ingressAuthLatency");

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);
    metrics.onCommandReceived(&helloCommand);
    metrics.onCommandProcessed(&helloCommand, nullptr);
    metrics.onAuthCompleted(&helloCommand);

    ASSERT_EQ(getNetworkHistogramTotalCount("ingressAuthLatency"), authBefore);
}

// Returns the count of the bucket whose lowerBound is `lowerBound` for a histogram registered
// under metrics.network, or 0 if either is missing.
long long getNetworkHistogramBucketCount(std::string_view metricName, long long lowerBound) {
    BSONObjBuilder b;
    appendMergedTrees({&globalMetricTreeSet()[ClusterRole::None]}, b);
    const auto obj = b.obj();
    const auto network = obj.getObjectField("metrics").getObjectField("network");
    const auto elem = network.getField(metricName);
    if (elem.eoo()) {
        return 0;
    }
    // Bucket keys are range strings of the form "[<lower>, <upper>)", so look for the field whose
    // name begins with the requested lower bound.
    const std::string prefix = fmt::format("[{}, ", lowerBound);
    for (const auto& field : elem.Obj()) {
        if (field.fieldNameStringData().starts_with(prefix)) {
            return field.Obj()["count"].Long();
        }
    }
    return 0;
}

// Each histogram measures the duration of its own stage, not the cumulative time since session
// start: hello from receiving the command to finishing it, and authentication from the start of
// the auth exchange to its completion.
TEST(IngressHandshakeMetricsTest, HandshakeHistogramsMeasureStageDurations) {
    MockHandshakeCommand helloCommand("test_only.ingressHandshakeStageDurations",
                                      Command::HandshakeRole::kHello);
    MockHandshakeCommand authCommand("test_only.ingressHandshakeStageDurationsAuth",
                                     Command::HandshakeRole::kAuth);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);

    // Each command will run for 3ms which should put them in the [625, 3125) bucket. We don't
    // expect entries for the cumulative time (6ms) in the [3125, 15625) bucket.
    const auto helloBeforeGoodBucket = getNetworkHistogramBucketCount("ingressHelloLatency", 625);
    const auto authBeforeGoodBucket = getNetworkHistogramBucketCount("ingressAuthLatency", 625);
    const auto helloBeforeBadBucket = getNetworkHistogramBucketCount("ingressHelloLatency", 3125);
    const auto authBeforeBadBucket = getNetworkHistogramBucketCount("ingressAuthLatency", 3125);

    metrics.onCommandReceived(&helloCommand);
    tickSource.advance(Milliseconds(3));
    metrics.onCommandProcessed(&helloCommand, nullptr);
    metrics.onCommandReceived(&authCommand);
    tickSource.advance(Milliseconds(3));
    metrics.onCommandProcessed(&authCommand, nullptr);
    metrics.onAuthCompleted(&authCommand);

    ASSERT_EQ(getNetworkHistogramBucketCount("ingressHelloLatency", 625),
              helloBeforeGoodBucket + 1);
    ASSERT_EQ(getNetworkHistogramBucketCount("ingressAuthLatency", 625), authBeforeGoodBucket + 1);
    ASSERT_EQ(getNetworkHistogramBucketCount("ingressHelloLatency", 3125), helloBeforeBadBucket);
    ASSERT_EQ(getNetworkHistogramBucketCount("ingressAuthLatency", 3125), authBeforeBadBucket);
}

// The auth histogram measures from the first auth command received, not the last: a subsequent
// auth command (e.g. saslContinue) must not restart the clock.
TEST(IngressHandshakeMetricsTest, AuthHistogramMeasuresFromFirstAuthCommandReceived) {
    MockHandshakeCommand saslStartCommand("test_only.ingressHandshakeSaslStart",
                                          Command::HandshakeRole::kAuth);
    MockHandshakeCommand saslContinueCommand("test_only.ingressHandshakeSaslContinue",
                                             Command::HandshakeRole::kAuth);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto authBefore = getNetworkHistogramBucketCount("ingressAuthLatency", 625);
    const auto authBeforeNextBucket = getNetworkHistogramBucketCount("ingressAuthLatency", 15625);

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);

    tickSource.advance(Milliseconds(1));
    metrics.onCommandReceived(&saslStartCommand);
    tickSource.advance(Milliseconds(20));
    metrics.onCommandReceived(&saslContinueCommand);
    tickSource.advance(Milliseconds(2));
    metrics.onAuthCompleted(&saslContinueCommand);

    // The exchange began with the saslStart, 22ms before completing, so the sample must land in
    // the [15625, 78125) bucket; measuring only from the saslContinue would give a 2ms sample
    // landing in the [625, 3125) bucket.
    ASSERT_EQ(getNetworkHistogramBucketCount("ingressAuthLatency", 625), authBefore);
    ASSERT_EQ(getNetworkHistogramBucketCount("ingressAuthLatency", 15625),
              authBeforeNextBucket + 1);
}

// A non-auth command received before authentication (e.g. hello -> ping -> saslStart ->
// saslContinue) ends the handshake, but auth commands received afterwards must still start the
// auth clock so that a valid auth latency is recorded.
TEST(IngressHandshakeMetricsTest, AuthHistogramRecordedForAuthAfterHandshakeDone) {
    MockHandshakeCommand helloCommand("test_only.ingressHandshakeHelloBeforePing",
                                      Command::HandshakeRole::kHello);
    MockHandshakeCommand pingCommand("test_only.ingressHandshakePingBeforeAuth",
                                     Command::HandshakeRole::kNone);
    MockHandshakeCommand saslStartCommand("test_only.ingressHandshakeSaslStartAfterPing",
                                          Command::HandshakeRole::kAuth);
    MockHandshakeCommand saslContinueCommand("test_only.ingressHandshakeSaslContinueAfterPing",
                                             Command::HandshakeRole::kAuth);

    TickSourceMock<Microseconds> tickSource;
    tickSource.advance(Seconds(1));

    const auto authBefore = getNetworkHistogramBucketCount("ingressAuthLatency", 15625);

    IngressHandshakeMetrics metrics;
    metrics.onSessionStarted(&tickSource);
    metrics.onCommandReceived(&helloCommand);
    metrics.onCommandProcessed(&helloCommand, nullptr);

    // The ping ends the handshake before any auth command is received.
    metrics.onCommandReceived(&pingCommand);

    tickSource.advance(Milliseconds(1));
    metrics.onCommandReceived(&saslStartCommand);
    tickSource.advance(Milliseconds(20));
    metrics.onCommandReceived(&saslContinueCommand);
    tickSource.advance(Milliseconds(2));
    metrics.onAuthCompleted(&saslContinueCommand);

    ASSERT_EQ(getNetworkHistogramBucketCount("ingressAuthLatency", 15625), authBefore + 1);
}

}  // namespace
}  // namespace mongo::transport
