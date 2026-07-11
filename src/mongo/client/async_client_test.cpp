// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/async_client.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/traces_test_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>

#include <boost/optional.hpp>

namespace mongo {
namespace {

using otel::traces::OtelTracesCapturer;
using otel::traces::Span;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Not;
namespace span_names = otel::traces::span_names;

// Base fixture for all async_client tests.
class AsyncClientTest : public ServiceContextTest {};

// Fixture for tests that exercise makeEgressTelemetrySection.
// Sets up a real TracerProvider (via OtelTracesCapturer) so Span::start creates non-no-op spans.
class MakeEgressTelemetrySectionTest : public AsyncClientTest {
public:
    void setUp() override {
        AsyncClientTest::setUp();
        if (!OtelTracesCapturer::canReadSpans()) {
            GTEST_SKIP() << "Requires OTel build";
        }
    }

protected:
    unittest::ServerParameterGuard _featureFlagTracing{"featureFlagTracing", true};
    OtelTracesCapturer _capturer;
};

// Returns the telemetry section that makeEgressTelemetrySection produces for the given context,
// wire version, and OperationContext (used to check whether tracing is enabled).
boost::optional<TelemetryContextSection> callMakeEgressTelemetrySection(
    OperationContext* opCtx,
    std::shared_ptr<otel::TelemetryContext> telemetryCtx,
    int maxWireVersion) {
    executor::RemoteCommandRequest request;
    request.opCtx = opCtx;
    request.telemetryContext = std::move(telemetryCtx);
    return AsyncDBClient::makeEgressTelemetrySection(request, maxWireVersion);
}

TEST_F(MakeEgressTelemetrySectionTest, ActiveSpanAndSupportedWireVersion) {
    auto opCtx = makeOperationContext();
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(
        callMakeEgressTelemetrySection(opCtx.get(), telemetryCtx, WireVersion::WIRE_VERSION_90),
        Not(Eq(boost::none)));
    EXPECT_THAT(
        callMakeEgressTelemetrySection(opCtx.get(), telemetryCtx, WireVersion::WIRE_VERSION_90 + 1),
        Not(Eq(boost::none)));
}

TEST_F(MakeEgressTelemetrySectionTest, ActiveSpanButOldWireVersion) {
    auto opCtx = makeOperationContext();
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(
        callMakeEgressTelemetrySection(opCtx.get(), telemetryCtx, WireVersion::WIRE_VERSION_90 - 1),
        Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, NullTelemetryContext) {
    auto opCtx = makeOperationContext();
    EXPECT_THAT(callMakeEgressTelemetrySection(
                    opCtx.get(), /*telemetryCtx=*/nullptr, WireVersion::WIRE_VERSION_90),
                Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, NoActiveSpan) {
    // Context exists but no span has been started on it, so there is no active span.
    auto opCtx = makeOperationContext();
    auto telemetryCtx = Span::createTelemetryContext();
    EXPECT_THAT(
        callMakeEgressTelemetrySection(opCtx.get(), telemetryCtx, WireVersion::WIRE_VERSION_90),
        Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, UnknownWireVersionSentinel) {
    // StreamableReplicaSetMonitor returns std::numeric_limits<int>::max() when no server
    // descriptions are known yet. We must not send the telemetry section in that case.
    auto opCtx = makeOperationContext();
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(
        callMakeEgressTelemetrySection(opCtx.get(), telemetryCtx, std::numeric_limits<int>::max()),
        Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, TracingDisabled) {
    unittest::ServerParameterGuard featureFlagTracing{"featureFlagTracing", false};

    auto opCtx = makeOperationContext();
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(
        callMakeEgressTelemetrySection(opCtx.get(), telemetryCtx, WireVersion::WIRE_VERSION_90),
        Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, NullOpCtxDoesNotSetTelemetrySection) {
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(callMakeEgressTelemetrySection(
                    /*opCtx=*/nullptr, telemetryCtx, WireVersion::WIRE_VERSION_90),
                Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, TraceparentIsNonEmptyWhenSectionIsSet) {
    auto opCtx = makeOperationContext();
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    auto section =
        callMakeEgressTelemetrySection(opCtx.get(), telemetryCtx, WireVersion::WIRE_VERSION_90);
    ASSERT_THAT(section, Not(Eq(boost::none)));
    EXPECT_THAT(section->getOtel().getTraceparent(), Not(IsEmpty()));
}

}  // namespace
}  // namespace mongo
