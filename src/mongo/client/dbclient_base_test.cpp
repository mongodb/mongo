// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/dbclient_base.h"

#include "mongo/client/connection_string.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/wire_version.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/traces_test_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>

#include <boost/optional.hpp>

namespace mongo {
namespace {

using otel::TelemetryContextHolder;
using otel::traces::OtelTracesCapturer;
using otel::traces::Span;
namespace span_names = otel::traces::span_names;

class FakeDBClient : public DBClientBase {
public:
    explicit FakeDBClient(int maxWireVersion,
                          ConnectionString::ConnectionType connectionType =
                              ConnectionString::ConnectionType::kStandalone)
        : _maxWireVersion(maxWireVersion), _connectionType(connectionType) {}

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
        return _connectionType;
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
    ConnectionString::ConnectionType _connectionType;
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
        if (!OtelTracesCapturer::canReadSpans())
            GTEST_SKIP() << "Requires OTel build";
    }

protected:
    unittest::ServerParameterGuard _featureFlagTracing{"featureFlagTracing", true};
    OtelTracesCapturer _capturer;
};

OpMsgRequest makeRequest() {
    OpMsgRequest req;
    req.body = BSON("ping" << 1 << "$db"
                           << "admin");
    return req;
}

// Sends a fire-and-forget command on a FakeDBClient with the given wire version and connection
// type, and returns the telemetry context attached to the serialized request.
boost::optional<TelemetryContextSection> sendAndGetTelemetryContext(
    int maxWireVersion,
    ConnectionString::ConnectionType connectionType =
        ConnectionString::ConnectionType::kStandalone) {
    FakeDBClient client(maxWireVersion, connectionType);
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

TEST_F(AppendMetadataTest, LocalConnectionTypeDoesNotSetTelemetryContext) {
    // A kLocal connection (e.g. DBDirectClient) talks to the server in-process, so there is no
    // network hop to trace and the telemetry context should not be propagated.
    auto opCtx = makeOperationContext();
    auto span = Span::start(opCtx.get(), span_names::kTest1);

    EXPECT_FALSE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90,
                                            ConnectionString::ConnectionType::kLocal)
                     .has_value());
}

TEST_F(AppendMetadataTest, FireAndForgetWithNullOperationContext) {
    ASSERT(!getClient()->getOperationContext());

    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.runFireAndForgetCommand(makeRequest());
    ASSERT_TRUE(client.lastSent().has_value());
    EXPECT_FALSE(OpMsgRequest::parse(*client.lastSent()).telemetryContext.has_value());
}

}  // namespace
}  // namespace mongo
