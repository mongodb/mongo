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
    EXPECT_EQ(histogram.sum, 123);
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
    EXPECT_EQ(histogram.sum, 120);
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
    EXPECT_EQ(histogram.sum, 5);
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

}  // namespace
}  // namespace mongo::transport
