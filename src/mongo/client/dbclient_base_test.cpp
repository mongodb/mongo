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

#include "mongo/client/dbclient_base.h"

#include "mongo/client/connection_string.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/version_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/sampler/sampler.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#ifdef MONGO_CONFIG_OTEL
#include "mongo/otel/traces/mock_exporter.h"
#include "mongo/otel/traces/tracer_provider_service.h"

#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>

using mongo::otel::traces::MockExporter;
using mongo::otel::traces::TracerProviderService;
#endif

#include <limits>

#include <boost/optional.hpp>

namespace mongo {
namespace {

using otel::TelemetryContextHolder;
using otel::traces::ScopedSamplerOverride;
using otel::traces::setTraceSamplingFnForTest;
using otel::traces::Span;
namespace span_names = otel::traces::span_names;

class FakeDBClient : public DBClientBase {
public:
    explicit FakeDBClient(int maxWireVersion) : _maxWireVersion(maxWireVersion) {}

    std::string toString() const override {
        return "FakeDBClient";
    }
    std::string getServerAddress() const override {
        return "localhost:27017";
    }
    std::string getLocalAddress() const override {
        return "localhost";
    }
    bool isStillConnected() override {
        return true;
    }
    bool isFailed() const override {
        return false;
    }
    ConnectionString::ConnectionType type() const override {
        return ConnectionString::ConnectionType::kStandalone;
    }
    double getSoTimeout() const override {
        return 0.0;
    }
    bool isReplicaSetMember() const override {
        return false;
    }
    bool isMongos() const override {
        return false;
    }
    int getMinWireVersion() override {
        return 0;
    }
    int getMaxWireVersion() override {
        return _maxWireVersion;
    }
    const SSLConfiguration* getSSLConfiguration() override {
        return nullptr;
    }
    bool isTLS() override {
        return false;
    }
    void say(Message& toSend,
             bool /*isRetry*/ = false,
             std::string* /*actualServer*/ = nullptr) override {
        _lastSent = toSend;
    }
    Message _call(Message& /*toSend*/, std::string* /*actualServer*/) override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Message> lastSent() const {
        return _lastSent;
    }

private:
    int _maxWireVersion;
    boost::optional<Message> _lastSent;
};

// Base fixture for all dbclient_base tests. Does not require an OTel build.
// Future tests that test non-OTel DBClientBase behavior should extend this directly.
class DBClientBaseTest : public ServiceContextTest {};

// Fixture for tests that exercise telemetry metadata on the egress path.
// Sets up a real TracerProvider so Span::start creates non-no-op spans.
// If the binary was not built with OTel (build_otel=false), all tests in this fixture are skipped.
class AppendMetadataTest : public DBClientBaseTest {
public:
    void setUp() override {
        DBClientBaseTest::setUp();
#ifdef MONGO_CONFIG_OTEL
        auto exporter = std::make_unique<MockExporter>();
        auto processor =
            opentelemetry::sdk::trace::SimpleSpanProcessorFactory::Create(std::move(exporter));
        auto resource = opentelemetry::sdk::resource::Resource::Create(
            {{"service.name", "test"}, {"service.instance.id", 1}});
        std::shared_ptr<opentelemetry::trace::TracerProvider> provider =
            opentelemetry::sdk::trace::TracerProviderFactory::Create(std::move(processor),
                                                                     resource);
        auto svc = TracerProviderService::create();
        svc->setTracerProvider_ForTest(std::move(provider));
        otel::traces::setGlobalTracerProviderService(std::move(svc));
#else
        GTEST_SKIP() << "Requires OTel build";
#endif
    }

    void tearDown() override {
#ifdef MONGO_CONFIG_OTEL
        otel::traces::setGlobalTracerProviderService(nullptr);
#endif
        DBClientBaseTest::tearDown();
    }

protected:
    unittest::ServerParameterGuard _featureFlagTracing{"featureFlagTracing", true};
    unittest::ServerParameterGuard _featureFlagSampling{"featureFlagOtelTraceSampling", true};
    ScopedSamplerOverride _samplerGuard =
        setTraceSamplingFnForTest([](std::string_view, double) { return true; });
};

OpMsgRequest makeRequest() {
    OpMsgRequest req;
    req.body = BSON("ping" << 1 << "$db"
                           << "admin");
    return req;
}

// Sends a fire-and-forget command on a FakeDBClient with the given wire version and returns the
// telemetry context attached to the serialized request.
boost::optional<TelemetryContextSection> sendAndGetTelemetryContext(int maxWireVersion) {
    FakeDBClient client(maxWireVersion);
    client.runFireAndForgetCommand(makeRequest());
    invariant(client.lastSent().has_value());
    return OpMsgRequest::parse(*client.lastSent()).telemetryContext;
}

TEST_F(AppendMetadataTest, ActiveSpanAndSupportedWireVersionSetsTelemetryContext) {
    auto opCtx = makeOperationContext();
    auto span = Span::start(opCtx.get(), span_names::kTest1);

    EXPECT_TRUE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90).has_value());
    EXPECT_TRUE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90 + 1).has_value());
}

TEST_F(AppendMetadataTest, ActiveSpanButOldWireVersionDoesNotSetTelemetryContext) {
    auto opCtx = makeOperationContext();
    auto span = Span::start(opCtx.get(), span_names::kTest1);

    EXPECT_FALSE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90 - 1).has_value());
}

TEST_F(AppendMetadataTest, NoActiveSpanDoesNotSetTelemetryContext) {
    auto opCtx = makeOperationContext();
    // Context exists in the holder but no span is started, so there is no active span.
    auto ctx = Span::createTelemetryContext();
    TelemetryContextHolder::getDecoration(opCtx.get()).setTelemetryContext(ctx);

    EXPECT_FALSE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90).has_value());
}

TEST_F(AppendMetadataTest, NoTelemetryContextDoesNotSetTelemetrySection) {
    auto opCtx = makeOperationContext();
    EXPECT_FALSE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90).has_value());
}

TEST_F(AppendMetadataTest, UnknownWireVersionSentinelDoesNotSetTelemetryContext) {
    // StreamableReplicaSetMonitor returns INT_MAX when no server descriptions are known yet.
    // We must not send the telemetry section in that case.
    auto opCtx = makeOperationContext();
    auto span = Span::start(opCtx.get(), span_names::kTest1);

    EXPECT_FALSE(sendAndGetTelemetryContext(std::numeric_limits<int>::max()).has_value());
}

TEST_F(AppendMetadataTest, SamplingFlagChecksTargetFCVNotLocalFCV) {
    // Use a version (8.3) where featureFlagTracing  passes but featureFlagOtelTraceSampling (9.0)
    // would fail a local FCV check.
    auto opCtx = makeOperationContext();
    {
        ClientLock lk(opCtx->getClient());
        VersionContext::setDecoration(
            lk,
            opCtx.get(),
            VersionContext{multiversion::FeatureCompatibilityVersion::kVersion_8_3});
    }
    auto span = Span::start(opCtx.get(), span_names::kTest1);

    EXPECT_FALSE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90).has_value());
}

}  // namespace
}  // namespace mongo
