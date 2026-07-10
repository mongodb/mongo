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
