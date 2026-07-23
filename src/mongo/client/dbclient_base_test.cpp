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
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>

#include <boost/optional.hpp>

namespace mongo {
namespace {

using otel::TelemetryContextHolder;
using otel::traces::HasAttribute;
using otel::traces::HasError;
using otel::traces::HasKind;
using otel::traces::OtelTracesCapturer;
using otel::traces::Span;
using ::testing::AllOf;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::SizeIs;
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
        if (_throwOnSayStatus) {
            uassertStatusOK(*_throwOnSayStatus);
        }
    }
    Message _call(Message& toSend, std::string* /*actualServer*/) override {
        _lastSent = toSend;
        if (_throwStatus) {
            uassertStatusOK(*_throwStatus);
        }
        if (_cannedReply) {
            return *_cannedReply;
        }
        MONGO_UNREACHABLE;
    }

    boost::optional<Message> lastSent() const {
        return _lastSent;
    }

    // Configures the reply that `_call` returns, so `runCommandWithTarget` can be exercised
    // without a real network round-trip.
    void setCannedReply(Message reply) {
        _cannedReply = std::move(reply);
    }

    // Configures `_call` to throw, simulating a network-level failure.
    void setThrowOnCall(Status status) {
        _throwStatus = std::move(status);
    }

    // Configures `say` to throw, simulating a failure on the fire-and-forget send path.
    void setThrowOnSay(Status status) {
        _throwOnSayStatus = std::move(status);
    }

private:
    int _maxWireVersion;
    ConnectionString::ConnectionType _connectionType;
    boost::optional<Message> _lastSent;
    boost::optional<Message> _cannedReply;
    boost::optional<Status> _throwStatus;
    boost::optional<Status> _throwOnSayStatus;
};

// Builds a minimal OK OpMsg command reply message usable as a canned response for `_call`.
Message makeOkOpMsgReply() {
    rpc::OpMsgReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(BSON("ok" << 1));
    return replyBuilder.done();
}

// Builds an OpMsg command reply message representing a failed command, usable as a canned
// response for `_call`.
Message makeErrorOpMsgReply(ErrorCodes::Error code, std::string_view errmsg) {
    rpc::OpMsgReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(
        BSON("ok" << 0 << "code" << int(code) << "errmsg" << std::string{errmsg}));
    return replyBuilder.done();
}

// Builds a legacy (OP_REPLY) command reply message. Since `runCommandWithTarget` sends an OpMsg
// request, replying with a legacy-protocol message triggers an RPC protocol negotiation failure.
Message makeLegacyOpReply() {
    rpc::LegacyReplyBuilder replyBuilder;
    replyBuilder.setRawCommandReply(BSON("ok" << 1));
    return replyBuilder.done();
}

// Base fixture for all dbclient_base tests. Does not require an OTel build.
// Future tests that test non-OTel DBClientBase behavior should extend this directly.
class DBClientBaseTest : public ServiceContextTest {};

// Fixture for tests that exercise telemetry metadata on the egress path.
// Sets up a real TracerProvider so Span::start creates non-no-op spans.
// If the binary was not built with OTel (build_otel=false), all tests in this fixture are skipped.
class SpanTest : public DBClientBaseTest {
public:
    void setUp() override {
        DBClientBaseTest::setUp();
        if (!OtelTracesCapturer::canReadSpans())
            GTEST_SKIP() << "Requires OTel build";
    }

protected:
    OtelTracesCapturer _capturer;
};

OpMsgRequest makeRequest(std::string_view commandName = "ping") {
    OpMsgRequest req;
    BSONObjBuilder bob;
    bob.append(commandName, 1);
    bob.append("$db", "admin");
    req.body = bob.obj();
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

using AppendMetadataTest = SpanTest;

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

TEST_F(AppendMetadataTest, NoPreexistingActiveSpanStillSetsTelemetryContextViaDBClientSpan) {
    auto opCtx = makeOperationContext();
    // Context exists in the holder but no span has been started on it yet by the caller.
    // `runFireAndForgetCommand` itself starts a span around the outgoing command, so the
    // telemetry context ends up populated regardless.
    auto ctx = Span::createTelemetryContext();
    TelemetryContextHolder::getDecoration(opCtx.get()).setTelemetryContext(ctx);

    EXPECT_TRUE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90).has_value());
}

TEST_F(AppendMetadataTest, NoPreexistingTelemetryContextStillSetsTelemetrySection) {
    auto opCtx = makeOperationContext();
    // `runFireAndForgetCommand` starts its own span around the outgoing command even when the
    // opCtx had no telemetry context beforehand, so a telemetry section is still sent.
    EXPECT_TRUE(sendAndGetTelemetryContext(WireVersion::WIRE_VERSION_90).has_value());
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

using RunFireAndForgetCommandSpanTest = SpanTest;
using RunCommandWithTargetSpanTest = SpanTest;

TEST_F(RunFireAndForgetCommandSpanTest, UsesRegisteredSpanNameForKnownCommand) {
    static const auto& registeredSpan =
        otel::traces::registerCommandSpanName("test_only.dbclient_fire_and_forget_known");

    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.runFireAndForgetCommand(makeRequest("test_only.dbclient_fire_and_forget_known"));

    EXPECT_THAT(_capturer.getSpans(registeredSpan), SizeIs(1));
    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), IsEmpty());
}

TEST_F(RunFireAndForgetCommandSpanTest, FallsBackToDefaultSpanNameForUnknownCommand) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.runFireAndForgetCommand(makeRequest("test_only.dbclient_fire_and_forget_unregistered"));

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), SizeIs(1));
}

TEST_F(RunFireAndForgetCommandSpanTest, CreatesProducerSpanKind) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.runFireAndForgetCommand(makeRequest("test_only.dbclient_fire_and_forget_kind"));

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC),
                ElementsAre(HasKind(otel::traces::SpanKind::kProducer)));
}

TEST_F(RunCommandWithTargetSpanTest, UsesRegisteredSpanNameForKnownCommand) {
    static const auto& registeredSpan =
        otel::traces::registerCommandSpanName("test_only.dbclient_run_command_known");

    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.setCannedReply(makeOkOpMsgReply());
    client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_known"));

    EXPECT_THAT(_capturer.getSpans(registeredSpan), SizeIs(1));
    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), IsEmpty());
}

TEST_F(RunCommandWithTargetSpanTest, FallsBackToDefaultSpanNameForUnknownCommand) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.setCannedReply(makeOkOpMsgReply());
    client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_unregistered"));

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), SizeIs(1));
}

TEST_F(RunCommandWithTargetSpanTest, CreatesClientSpanKind) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.setCannedReply(makeOkOpMsgReply());
    client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_kind"));

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC),
                ElementsAre(HasKind(otel::traces::SpanKind::kClient)));
}

TEST_F(RunFireAndForgetCommandSpanTest, DoesNotStartSpanForLocalConnection) {
    static const auto& registeredSpan =
        otel::traces::registerCommandSpanName("test_only.dbclient_fire_and_forget_local");

    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90, ConnectionString::ConnectionType::kLocal);
    client.runFireAndForgetCommand(makeRequest("test_only.dbclient_fire_and_forget_local"));

    EXPECT_THAT(_capturer.getSpans(registeredSpan), IsEmpty());
    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), IsEmpty());
}

TEST_F(RunCommandWithTargetSpanTest, DoesNotStartSpanForLocalConnection) {
    static const auto& registeredSpan =
        otel::traces::registerCommandSpanName("test_only.dbclient_run_command_local");

    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90, ConnectionString::ConnectionType::kLocal);
    client.setCannedReply(makeOkOpMsgReply());
    client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_local"));

    EXPECT_THAT(_capturer.getSpans(registeredSpan), IsEmpty());
    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), IsEmpty());
}

using RunFireAndForgetCommandSpanStatusTest = SpanTest;

TEST_F(RunFireAndForgetCommandSpanStatusTest, SuccessfulSendDoesNotMarkSpanAsError) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.runFireAndForgetCommand(makeRequest("test_only.dbclient_fire_and_forget_status_ok"));

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), ElementsAre(Not(HasError())));
}

TEST_F(RunFireAndForgetCommandSpanStatusTest, SendFailureMarksSpanAsError) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.setThrowOnSay(Status(ErrorCodes::HostUnreachable, "simulated send error"));

    ASSERT_THROWS_CODE(client.runFireAndForgetCommand(
                           makeRequest("test_only.dbclient_fire_and_forget_status_error")),
                       DBException,
                       ErrorCodes::HostUnreachable);

    // Verify the thrown status was recorded on the span, not just that the span was marked as
    // errored by unwinding.
    EXPECT_THAT(
        _capturer.getSpans(span_names::kMongoRPC),
        ElementsAre(AllOf(HasError(), HasAttribute("errorCodeString", "simulated send error"))));
}

using RunCommandWithTargetSpanStatusTest = SpanTest;

TEST_F(RunCommandWithTargetSpanStatusTest, OkReplyDoesNotMarkSpanAsError) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.setCannedReply(makeOkOpMsgReply());
    client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_status_ok"));

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), ElementsAre(Not(HasError())));
}

TEST_F(RunCommandWithTargetSpanStatusTest, CommandErrorReplyMarksSpanAsError) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.setCannedReply(makeErrorOpMsgReply(ErrorCodes::BadValue, "some command error"));
    client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_status_error"));

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), ElementsAre(HasError()));
}

TEST_F(RunCommandWithTargetSpanStatusTest, NetworkErrorMarksSpanAsError) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    client.setThrowOnCall(Status(ErrorCodes::HostUnreachable, "simulated network error"));

    ASSERT_THROWS_CODE(
        client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_status_network")),
        DBException,
        ErrorCodes::HostUnreachable);

    // Verify the thrown status (with its added network-error context) was recorded on the span,
    // not just that the span was marked as errored by unwinding.
    EXPECT_THAT(
        _capturer.getSpans(span_names::kMongoRPC),
        ElementsAre(AllOf(HasError(),
                          HasAttribute("errorCodeString", HasSubstr("simulated network error")))));
}

TEST_F(RunCommandWithTargetSpanStatusTest, ProtocolNegotiationFailureMarksSpanAsError) {
    auto opCtx = makeOperationContext();
    FakeDBClient client(WireVersion::WIRE_VERSION_90);
    // Reply with a legacy-protocol message even though the request was OpMsg, which fails RPC
    // protocol negotiation.
    client.setCannedReply(makeLegacyOpReply());

    ASSERT_THROWS_CODE(
        client.runCommandWithTarget(makeRequest("test_only.dbclient_run_command_status_protocol")),
        DBException,
        ErrorCodes::RPCProtocolNegotiationFailed);

    // Verify the protocol-negotiation status was recorded on the span, not just that the span was
    // marked as errored by unwinding.
    EXPECT_THAT(
        _capturer.getSpans(span_names::kMongoRPC),
        ElementsAre(AllOf(HasError(),
                          HasAttribute("errorCodeString", HasSubstr("Mismatched RPC protocols")))));
}

}  // namespace
}  // namespace mongo
