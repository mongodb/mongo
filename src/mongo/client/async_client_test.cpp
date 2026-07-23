// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/async_client.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/database_name.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/otel/telemetry_context_holder.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/traces_test_util.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <deque>
#include <functional>
#include <limits>

#include <boost/optional.hpp>

namespace mongo {
namespace {

using otel::TelemetryContextHolder;
using otel::traces::HasError;
using otel::traces::HasKind;
using otel::traces::HasSpanName;
using otel::traces::OtelTracesCapturer;
using otel::traces::Parent;
using otel::traces::registerCommandSpanName;
using otel::traces::Span;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::IsNull;
using ::testing::Not;
using ::testing::SizeIs;
using unittest::match::StatusIsOK;
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

// Returns the telemetry section that makeEgressTelemetrySection produces for the given
// telemetry context and wire version.
boost::optional<TelemetryContextSection> callMakeEgressTelemetrySection(
    std::shared_ptr<otel::TelemetryContext> telemetryCtx, int maxWireVersion) {
    executor::RemoteCommandRequest request;
    request.telemetryContext = std::move(telemetryCtx);
    return AsyncDBClient::makeEgressTelemetrySection(request, maxWireVersion);
}

TEST_F(MakeEgressTelemetrySectionTest, ActiveSpanAndSupportedWireVersion) {
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(callMakeEgressTelemetrySection(telemetryCtx, WireVersion::WIRE_VERSION_90),
                Not(Eq(boost::none)));
    EXPECT_THAT(callMakeEgressTelemetrySection(telemetryCtx, WireVersion::WIRE_VERSION_90 + 1),
                Not(Eq(boost::none)));
}

TEST_F(MakeEgressTelemetrySectionTest, ActiveSpanButOldWireVersion) {
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(callMakeEgressTelemetrySection(telemetryCtx, WireVersion::WIRE_VERSION_90 - 1),
                Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, NullTelemetryContext) {
    EXPECT_THAT(
        callMakeEgressTelemetrySection(/*telemetryCtx=*/nullptr, WireVersion::WIRE_VERSION_90),
        Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, NoActiveSpan) {
    // Context exists but no span has been started on it, so there is no active span.
    auto telemetryCtx = Span::createTelemetryContext();
    EXPECT_THAT(callMakeEgressTelemetrySection(telemetryCtx, WireVersion::WIRE_VERSION_90),
                Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, UnknownWireVersionSentinel) {
    // StreamableReplicaSetMonitor returns std::numeric_limits<int>::max() when no server
    // descriptions are known yet. We must not send the telemetry section in that case.
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(callMakeEgressTelemetrySection(telemetryCtx, std::numeric_limits<int>::max()),
                Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, TracingDisabled) {
    unittest::ServerParameterGuard featureFlagTracing{"featureFlagTracing", false};

    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_THAT(callMakeEgressTelemetrySection(telemetryCtx, WireVersion::WIRE_VERSION_90),
                Eq(boost::none));
}

TEST_F(MakeEgressTelemetrySectionTest, TraceparentIsNonEmptyWhenSectionIsSet) {
    auto telemetryCtx = Span::createTelemetryContext();
    auto span = Span::start(telemetryCtx, span_names::kTest1);

    auto section = callMakeEgressTelemetrySection(telemetryCtx, WireVersion::WIRE_VERSION_90);
    ASSERT_THAT(section, Not(Eq(boost::none)));
    EXPECT_THAT(section->getOtel().getTraceparent(), Not(Eq("")));
}

/**
 * Fixture for tests that exercise `AsyncDBClient::startEgressSpan` and
 * `AsyncDBClient::maybeEndExhaustSpan`. These are the static helpers used by
 * `runCommandRequest` and the exhaust command path to start/end spans around outgoing commands
 * without requiring a live connection.
 */
class EgressSpanTest : public AsyncClientTest {
public:
    void setUp() override {
        AsyncClientTest::setUp();
        if (!OtelTracesCapturer::canReadSpans()) {
            GTEST_SKIP() << "Requires OTel build";
        }
    }

protected:
    OtelTracesCapturer _capturer;
};

using StartEgressSpanTest = EgressSpanTest;

TEST_F(StartEgressSpanTest, FallsBackToGenericSpanNameForUnregisteredCommand) {
    std::shared_ptr<otel::TelemetryContext> telemetryCtx;
    {
        auto span = AsyncDBClient::startEgressSpan(
            telemetryCtx, "test_only.async_client_unregistered", /*fireAndForget=*/false);
    }

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), SizeIs(1));
}

TEST_F(StartEgressSpanTest, UsesRegisteredSpanNameForKnownCommand) {
    static const auto& registeredSpan = registerCommandSpanName("test_only.async_client_known");
    std::shared_ptr<otel::TelemetryContext> telemetryCtx;
    {
        auto span = AsyncDBClient::startEgressSpan(
            telemetryCtx, "test_only.async_client_known", /*fireAndForget=*/false);
    }

    EXPECT_THAT(_capturer.getSpans(registeredSpan), SizeIs(1));
    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC), IsEmpty());
}

TEST_F(StartEgressSpanTest, PopulatesNullTelemetryContext) {
    std::shared_ptr<otel::TelemetryContext> telemetryCtx;
    ASSERT_THAT(telemetryCtx, IsNull());

    auto span = AsyncDBClient::startEgressSpan(
        telemetryCtx, "test_only.async_client_ping", /*fireAndForget=*/false);

    EXPECT_THAT(telemetryCtx, Not(IsNull()));
}

TEST_F(StartEgressSpanTest, IsChildOfExistingSpanOnTelemetryContext) {
    auto telemetryCtx = Span::createTelemetryContext();
    {
        auto parentSpan = Span::start(telemetryCtx, span_names::kTest1);
        // Use a command name that is guaranteed to not be registered (unlike e.g. "ping", which
        // is registered as a real command in this test binary), so this exercises the kMongoRPC
        // fallback path.
        {
            auto childSpan = AsyncDBClient::startEgressSpan(
                telemetryCtx, "test_only.unregistered", /*fireAndForget=*/false);
        }
    }

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC),
                ElementsAre(Parent(HasSpanName(span_names::kTest1))));
}

TEST_F(StartEgressSpanTest, CreatesClientSpanKind) {
    std::shared_ptr<otel::TelemetryContext> telemetryCtx;
    {
        auto span = AsyncDBClient::startEgressSpan(
            telemetryCtx, "test_only.async_client_kind", /*fireAndForget=*/false);
    }

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC),
                ElementsAre(HasKind(otel::traces::SpanKind::kClient)));
}

TEST_F(StartEgressSpanTest, CreatesProducerSpanKindForFireAndForget) {
    std::shared_ptr<otel::TelemetryContext> telemetryCtx;
    {
        auto span = AsyncDBClient::startEgressSpan(
            telemetryCtx, "test_only.async_client_fire_and_forget_kind", /*fireAndForget=*/true);
    }

    EXPECT_THAT(_capturer.getSpans(span_names::kMongoRPC),
                ElementsAre(HasKind(otel::traces::SpanKind::kProducer)));
}

using MaybeEndExhaustSpanTest = EgressSpanTest;

TEST_F(MaybeEndExhaustSpanTest, KeepsSpanAliveWhileMoreToComeIsSet) {
    auto telemetryCtx = Span::createTelemetryContext();
    boost::optional<Span> span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_FALSE(AsyncDBClient::maybeEndExhaustSpan(span, /*isMoreToComeSet=*/true, Status::OK()));
    EXPECT_TRUE(span.has_value());
    EXPECT_THAT(_capturer.getSpans(span_names::kTest1), IsEmpty());
}

TEST_F(MaybeEndExhaustSpanTest, EndsSpanOnceMoreToComeIsNotSet) {
    auto telemetryCtx = Span::createTelemetryContext();
    boost::optional<Span> span = Span::start(telemetryCtx, span_names::kTest1);

    EXPECT_TRUE(AsyncDBClient::maybeEndExhaustSpan(span, /*isMoreToComeSet=*/false, Status::OK()));
    EXPECT_FALSE(span.has_value());

    EXPECT_THAT(_capturer.getSpans(span_names::kTest1), ElementsAre(Not(HasError())));
}

TEST_F(MaybeEndExhaustSpanTest, RecordsErrorStatus) {
    auto telemetryCtx = Span::createTelemetryContext();
    boost::optional<Span> span = Span::start(telemetryCtx, span_names::kTest1);

    AsyncDBClient::maybeEndExhaustSpan(
        span, /*isMoreToComeSet=*/false, Status(ErrorCodes::HostUnreachable, "mock error"));

    EXPECT_THAT(_capturer.getSpans(span_names::kTest1), ElementsAre(HasError()));
}

TEST_F(MaybeEndExhaustSpanTest, IsNoopWhenNoSpanIsPresent) {
    boost::optional<Span> span;

    EXPECT_FALSE(AsyncDBClient::maybeEndExhaustSpan(span, false, Status::OK()));
    EXPECT_FALSE(span.has_value());
}

/**
 * A minimal in-memory Session that lets us drive AsyncDBClient's public command APIs
 * (`runCommandRequest`, `beginExhaustCommandRequest`, `awaitExhaustCommand`) without a real
 * network connection. `asyncSinkMessage` completes immediately. `asyncSourceMessage` returns
 * whatever Future was queued via `queueReply` / `queueDeferredReply`. Combined with
 * `transport::test::InlineReactor` (which runs scheduled work inline), ready replies make the
 * whole command future chain resolve synchronously; deferred replies leave it pending until
 * the returned Promise is fulfilled.
 */
class FakeAsyncSession : public transport::MockSessionBase {
public:
    transport::TransportLayer* getTransportLayer() const override {
        return nullptr;
    }
    void end() override {}
    bool isConnected() override {
        return true;
    }
    Status waitForData() override {
        return Status::OK();
    }
    Future<void> asyncWaitForData() override {
        return Future<void>::makeReady();
    }

    Status sinkMessage(Message message) override {
        _lastSent = message;
        return Status::OK();
    }
    Future<void> asyncSinkMessage(Message message, const BatonHandle& = nullptr) override {
        return Future<void>::makeReady(sinkMessage(std::move(message)));
    }

    StatusWith<Message> sourceMessage() override {
        return asyncSourceMessage().getNoThrow();
    }
    Future<Message> asyncSourceMessage(const BatonHandle& = nullptr) override {
        if (_replies.empty()) {
            return Status(ErrorCodes::HostUnreachable, "FakeAsyncSession has no reply queued");
        }
        invariant(_lastSent);
        auto makeReply = std::move(_replies.front());
        _replies.pop_front();
        return makeReply(*_lastSent);
    }

    /**
     * Queues a reply to be returned immediately from the next call to
     * `sourceMessage`/`asyncSourceMessage`. The provided function receives the most recently
     * sent request so it can address the reply to it (i.e. set `responseTo` to the request's
     * message id).
     */
    void queueReply(std::function<Message(const Message&)> makeReply) {
        _replies.push_back([makeReply = std::move(makeReply)](const Message& request) {
            return Future<Message>::makeReady(makeReply(request));
        });
    }

    /**
     * Queues a reply that builds its Message when `asyncSourceMessage` is invoked, but does not
     * complete until the returned Promise is fulfilled. Useful for asserting state while a
     * command response is still outstanding.
     */
    Promise<void> queueDeferredReply(std::function<Message(const Message&)> makeReply) {
        auto pf = makePromiseFuture<void>();
        // Future is move-only, so share it into the std::function stored on the reply queue.
        auto deferred = std::make_shared<Future<void>>(std::move(pf.future));
        _replies.push_back([makeReply = std::move(makeReply),
                            deferred = std::move(deferred)](const Message& request) mutable {
            auto reply = makeReply(request);
            return std::move(*deferred).then(
                [reply = std::move(reply)]() mutable { return std::move(reply); });
        });
        return std::move(pf.promise);
    }

private:
    boost::optional<Message> _lastSent;
    std::deque<std::function<Future<Message>(const Message&)>> _replies;
};

/** Builds an OpMsg command reply Message properly addressed as a response to `request`. */
Message makeReplyTo(const Message& request, BSONObj body, bool moreToComeSet = false) {
    rpc::OpMsgReplyBuilder replyBuilder;
    replyBuilder.setCommandReply(std::move(body));
    Message reply = replyBuilder.done();
    if (moreToComeSet) {
        OpMsg::setFlag(&reply, OpMsg::kMoreToCome);
    }
    reply.header().setId(nextMessageId());
    reply.header().setResponseToMsgId(request.header().getId());
    return reply;
}

executor::RemoteCommandRequest makeRemoteCommandRequest(std::string_view commandName) {
    return executor::RemoteCommandRequest(HostAndPort("localhost", 27017),
                                          DatabaseName::kAdmin,
                                          BSON(commandName << 1),
                                          /*opCtx=*/nullptr);
}

/**
 * Fixture for tests that exercise span lifetimes through AsyncDBClient's public command APIs
 * (`runCommandRequest`, `beginExhaustCommandRequest`, `awaitExhaustCommand`), using a
 * FakeAsyncSession in place of a real network connection.
 */
class CommandSpanLifetimeTest : public EgressSpanTest {
public:
    void setUp() override {
        EgressSpanTest::setUp();
        _session = std::make_shared<FakeAsyncSession>();
        _client =
            std::make_shared<AsyncDBClient>(HostAndPort("localhost", 27017),
                                            _session,
                                            getServiceContext(),
                                            std::make_shared<transport::test::InlineReactor>());
    }

protected:
    std::shared_ptr<FakeAsyncSession> _session;
    std::shared_ptr<AsyncDBClient> _client;
};

using RunCommandRequestSpanTest = CommandSpanLifetimeTest;

TEST_F(RunCommandRequestSpanTest, IsOpenUntilResponseAndThenEndsOk) {
    static const auto& registeredSpan = registerCommandSpanName("test_only.run_command_request_ok");
    auto continueReply = _session->queueDeferredReply(
        [](const Message& request) { return makeReplyTo(request, BSON("ok" << 1)); });

    auto future =
        _client->runCommandRequest(makeRemoteCommandRequest("test_only.run_command_request_ok"));

    // The request has been sunk, but the deferred reply has not been fulfilled yet, so the
    // command future (and therefore the egress span) must still be open.
    ASSERT_FALSE(future.isReady());
    EXPECT_THAT(_capturer.getSpans(registeredSpan), IsEmpty());

    continueReply.emplaceValue();
    ASSERT_TRUE(future.isReady());
    auto swResponse = std::move(future).getNoThrow();
    EXPECT_THAT(swResponse, StatusIsOK());

    EXPECT_THAT(_capturer.getSpans(registeredSpan), ElementsAre(Not(HasError())));
}

TEST_F(RunCommandRequestSpanTest, EndsWithErrorOnCommandLevelErrorInSuccessfulResponse) {
    static const auto& registeredSpan =
        registerCommandSpanName("test_only.run_command_request_command_error");
    // The transport round-trip itself succeeds, but the command reply body reports a
    // command-level failure (ok: 0). The span must still be marked as an error in this case.
    _session->queueReply([](const Message& request) {
        return makeReplyTo(request,
                           BSON("ok" << 0 << "code" << ErrorCodes::BadValue << "errmsg"
                                     << "mock command error"));
    });

    auto future = _client->runCommandRequest(
        makeRemoteCommandRequest("test_only.run_command_request_command_error"));
    auto swResponse = std::move(future).getNoThrow();
    // runCommandRequest itself succeeds (it returns the RemoteCommandResponse containing the
    // error reply); it is the caller's responsibility to interpret the command reply's status.
    EXPECT_THAT(swResponse, StatusIsOK());

    EXPECT_THAT(_capturer.getSpans(registeredSpan), ElementsAre(HasError()));
}

TEST_F(RunCommandRequestSpanTest, EndsWithErrorOnFailedResponse) {
    static const auto& registeredSpan =
        registerCommandSpanName("test_only.run_command_request_error");
    // No reply is queued, so `sourceMessage` will return an error status, simulating a network
    // failure while waiting for the response.

    auto future =
        _client->runCommandRequest(makeRemoteCommandRequest("test_only.run_command_request_error"));
    auto swResponse = std::move(future).getNoThrow();
    EXPECT_THAT(swResponse, Not(StatusIsOK()));

    EXPECT_THAT(_capturer.getSpans(registeredSpan), ElementsAre(HasError()));
}

// When an operation is cancelled, it's possible that the OpCtx is destroyed before the request has
// been sent, so it's important that `runCommandRequest` doesn't rely on the OpCtx being valid.
TEST_F(RunCommandRequestSpanTest, OkWhenOpCtxDestroyedBeforeRequestRuns) {
    static const auto& registeredSpan =
        registerCommandSpanName("test_only.run_command_request_destroyed_opctx");

    executor::RemoteCommandRequest request;
    {
        auto opCtx = makeOperationContext();
        auto span = Span::start(opCtx.get(), span_names::kTest1);
        request = makeRemoteCommandRequest("test_only.run_command_request_destroyed_opctx");
        request.opCtx = opCtx.get();
        request.telemetryContext =
            TelemetryContextHolder::getDecoration(opCtx.get()).cloneTelemetryContext();
    }

    auto continueReply = _session->queueDeferredReply(
        [](const Message& request) { return makeReplyTo(request, BSON("ok" << 1)); });

    auto future = _client->runCommandRequest(std::move(request));

    // The request has been sunk, but the deferred reply has not been fulfilled yet. The
    // OperationContext that request.opCtx pointed to is already destroyed.
    ASSERT_FALSE(future.isReady());

    continueReply.emplaceValue();
    auto swResponse = std::move(future).getNoThrow();
    EXPECT_THAT(swResponse, StatusIsOK());

    EXPECT_THAT(_capturer.getSpans(registeredSpan), ElementsAre(Not(HasError())));
}

TEST_F(RunCommandRequestSpanTest, EndsEvenWhenCanceledBeforeStarting) {
    static const auto& registeredSpan =
        registerCommandSpanName("test_only.run_command_request_canceled");

    CancellationSource source;
    source.cancel();

    auto future = _client->runCommandRequest(
        makeRemoteCommandRequest("test_only.run_command_request_canceled"),
        /*baton=*/nullptr,
        /*fromConnAcquiredTimer=*/nullptr,
        source.token());
    auto swResponse = std::move(future).getNoThrow();
    ASSERT_NOT_OK(swResponse);

    // Even though the command was canceled before ever hitting the wire, the span opened by
    // `runCommandRequest` must still be ended (with the cancellation error recorded).
    EXPECT_THAT(_capturer.getSpans(registeredSpan), ElementsAre(HasError()));
}

using BeginExhaustCommandRequestSpanTest = CommandSpanLifetimeTest;

TEST_F(BeginExhaustCommandRequestSpanTest,
       StaysOpenAcrossResponsesUntilMoreToComeIsUnsetThenEndsOk) {
    static const auto& registeredSpan = registerCommandSpanName("test_only.exhaust_ok");
    _session->queueReply(
        [](const Message& request) { return makeReplyTo(request, BSON("ok" << 1), true); });

    auto beginFuture =
        _client->beginExhaustCommandRequest(makeRemoteCommandRequest("test_only.exhaust_ok"));
    auto swFirstResponse = std::move(beginFuture).getNoThrow();
    ASSERT_OK(swFirstResponse);
    ASSERT_TRUE(swFirstResponse.getValue().moreToCome);

    // The exhaust stream is still open (isMoreToComeSet was true), so the span covering the whole
    // stream must not have ended yet.
    EXPECT_THAT(_capturer.getSpans(registeredSpan), IsEmpty());

    _session->queueReply(
        [](const Message& request) { return makeReplyTo(request, BSON("ok" << 1), false); });
    auto awaitFuture = _client->awaitExhaustCommand();
    auto swSecondResponse = std::move(awaitFuture).getNoThrow();
    ASSERT_OK(swSecondResponse);
    ASSERT_FALSE(swSecondResponse.getValue().moreToCome);

    // Now that isMoreToComeSet is false, the span should be ended (and not an error).
    EXPECT_THAT(_capturer.getSpans(registeredSpan), ElementsAre(Not(HasError())));
}

TEST_F(BeginExhaustCommandRequestSpanTest, EndsWithErrorIfAwaitExhaustCommandFails) {
    static const auto& registeredSpan = registerCommandSpanName("test_only.exhaust_error");
    _session->queueReply(
        [](const Message& request) { return makeReplyTo(request, BSON("ok" << 1), true); });

    auto beginFuture =
        _client->beginExhaustCommandRequest(makeRemoteCommandRequest("test_only.exhaust_error"));
    auto swFirstResponse = std::move(beginFuture).getNoThrow();
    ASSERT_OK(swFirstResponse);
    EXPECT_THAT(_capturer.getSpans(registeredSpan), IsEmpty());

    // No reply is queued this time, so awaiting the next response fails.
    auto awaitFuture = _client->awaitExhaustCommand();
    auto swSecondResponse = std::move(awaitFuture).getNoThrow();
    ASSERT_NOT_OK(swSecondResponse);

    EXPECT_THAT(_capturer.getSpans(registeredSpan), ElementsAre(HasError()));
}

}  // namespace
}  // namespace mongo
