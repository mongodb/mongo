// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/dbclient_connection.h"

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/rpc_local_timing.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

#include <deque>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mongo {
namespace {

Message makeOpMsg(BSONObj body) {
    OpMsgBuilder builder;
    builder.setBody(std::move(body));
    return builder.finish();
}

OpMsgRequest makeCommandRequest() {
    return rpc::upconvertRequest(DatabaseName::kAdmin, BSON("ping" << 1), 0);
}

Message makeHelloReply() {
    return makeOpMsg(BSON("ok" << 1 << "minWireVersion" << 0 << "maxWireVersion"
                               << WireVersion::LATEST_WIRE_VERSION));
}

class FakeSyncSession : public transport::MockSessionBase {
public:
    explicit FakeSyncSession(TickSourceMock<Microseconds>& clock) : _clock(clock) {}

    transport::TransportLayer* getTransportLayer() const override {
        return nullptr;
    }

    void end() override {
        _clock.advance(_endAdvance);
    }

    Status waitForData() override {
        return Status::OK();
    }

    Future<void> asyncWaitForData() override {
        return Future<void>::makeReady();
    }

    Status sinkMessage(Message message) override {
        _clock.advance(_writeAdvance);
        if (_writeStatus.isOK()) {
            _sent.push_back(std::move(message));
        }
        return _writeStatus;
    }

    Future<void> asyncSinkMessage(Message message, const BatonHandle& = nullptr) override {
        return Future<void>::makeReady(sinkMessage(std::move(message)));
    }

    StatusWith<Message> sourceMessage() override {
        _clock.advance(_readAdvance);
        if (!_readStatus.isOK()) {
            return _readStatus;
        }
        if (_replies.empty()) {
            return Status(ErrorCodes::HostUnreachable, "No fake reply was queued");
        }
        auto reply = std::move(_replies.front());
        _replies.pop_front();
        return reply;
    }

    Future<Message> asyncSourceMessage(const BatonHandle& = nullptr) override {
        auto reply = sourceMessage();
        return reply.isOK() ? Future<Message>(std::move(reply.getValue()))
                            : Future<Message>(reply.getStatus());
    }

    void queueReply(Message reply) {
        _replies.push_back(std::move(reply));
    }

    void setWriteAdvance(Microseconds advance) {
        _writeAdvance = advance;
    }

    void setReadAdvance(Microseconds advance) {
        _readAdvance = advance;
    }

    void setEndAdvance(Microseconds advance) {
        _endAdvance = advance;
    }

    void setWriteStatus(Status status) {
        _writeStatus = std::move(status);
    }

    void setReadStatus(Status status) {
        _readStatus = std::move(status);
    }

    const std::vector<Message>& sentMessages() const {
        return _sent;
    }

private:
    TickSourceMock<Microseconds>& _clock;
    std::deque<Message> _replies;
    std::vector<Message> _sent;
    Microseconds _writeAdvance{0};
    Microseconds _readAdvance{0};
    Microseconds _endAdvance{0};
    Status _writeStatus = Status::OK();
    Status _readStatus = Status::OK();
};

class FakeSyncConnection : public DBClientConnection {
public:
    explicit FakeSyncConnection(TickSourceMock<Microseconds>& clock,
                                bool autoReconnect = false,
                                bool disableCompression = false)
        : DBClientConnection(
              {.autoReconnect = autoReconnect, .disableCompression = disableCompression}),
          _fakeSession(std::make_shared<FakeSyncSession>(clock)) {
        setTickSource_forTest(&clock);
    }

    FakeSyncSession& fakeSession() {
        return *_fakeSession;
    }

    void setMakeSessionStatus(Status status) {
        _makeSessionStatus = std::move(status);
    }

    void markFailed() {
        _markFailed(kSetFlag);
    }

private:
    StatusWith<std::shared_ptr<transport::Session>> _makeSession(
        const HostAndPort&,
        transport::ConnectSSLMode,
        Milliseconds,
        const boost::optional<TransientSSLParams>&) override {
        if (!_makeSessionStatus.isOK()) {
            return _makeSessionStatus;
        }
        return std::static_pointer_cast<transport::Session>(_fakeSession);
    }

    void _reconnectSession() override {
        connect(_serverAddress, "test", boost::none);
    }

    void _killSession() override {
        _fakeSession->end();
    }

    std::shared_ptr<FakeSyncSession> _fakeSession;
    Status _makeSessionStatus = Status::OK();
};

class DBClientConnectionTimingFixture : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        if (!_capturer.canReadMetrics()) {
            GTEST_SKIP() << "OTel metrics reader unavailable";
        }
    }

    int64_t countFor(std::string_view kind) {
        try {
            return _capturer
                .readInt64Histogram(otel::metrics::MetricNames::kNetworkRpcLocalLatency,
                                    std::tuple{kind, "sync"sv})
                .count;
        } catch (const DBException&) {
            return 0;
        }
    }

    int64_t sumFor(std::string_view kind) {
        try {
            return _capturer
                .readInt64Histogram(otel::metrics::MetricNames::kNetworkRpcLocalLatency,
                                    std::tuple{kind, "sync"sv})
                .sum;
        } catch (const DBException&) {
            return 0;
        }
    }

    void connect(FakeSyncConnection& connection) {
        connection.connectNoHello(HostAndPort("localhost", 27017), boost::none);
    }

    TickSourceMock<Microseconds> _clock;

private:
    otel::metrics::OtelMetricsCapturer _capturer;
};

TEST_F(DBClientConnectionTimingFixture, RunCommandRecordsLocalTimeOnceAndExcludesReadWait) {
    FakeSyncConnection connection(_clock);
    connect(connection);
    connection.fakeSession().queueReply(makeOpMsg(BSON("ok" << 1)));
    connection.fakeSession().setWriteAdvance(Microseconds{3000});
    connection.fakeSession().setReadAdvance(Seconds{10});
    connection.setRequestMetadataWriter([&](OperationContext*, BSONObjBuilder*) {
        _clock.advance(Microseconds{2000});
        return Status::OK();
    });
    connection.setReplyMetadataReader([&](OperationContext*, const BSONObj&, std::string_view) {
        _clock.advance(Microseconds{700});
        return Status::OK();
    });

    connection.runCommandWithTarget(makeCommandRequest());

    EXPECT_EQ(countFor("initial"), 1);
    EXPECT_EQ(sumFor("initial"), 5700);
}

TEST_F(DBClientConnectionTimingFixture, FireAndForgetRecordsItsOwnKindOnce) {
    FakeSyncConnection connection(_clock);
    connect(connection);
    connection.fakeSession().setWriteAdvance(Microseconds{3000});

    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    EXPECT_EQ(&connection, connection.runFireAndForgetCommand(makeCommandRequest()));
    connection.killCursor(nss, 42);

    EXPECT_EQ(countFor("fire_and_forget"), 2);
    EXPECT_EQ(sumFor("fire_and_forget"), 6000);
    EXPECT_EQ(countFor("initial"), 0);
}

TEST_F(DBClientConnectionTimingFixture, ExhaustCursorRecordsContinuationsWithoutCallerGaps) {
    FakeSyncConnection connection(_clock);
    connect(connection);
    connection.fakeSession().setWriteAdvance(Microseconds{3000});
    connection.fakeSession().setReadAdvance(Seconds{10});

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    FindCommandRequest findCmd{nss};
    DBClientCursor cursor(&connection, findCmd, ReadPreferenceSetting{}, true /* isExhaust */);
    cursor.setBatchSize(0);

    BSONArrayBuilder emptyBatch;
    auto initialReply = makeOpMsg(BSON("ok" << 1 << "cursor"
                                            << BSON("id" << 42LL << "ns" << "test.coll"
                                                         << "firstBatch" << emptyBatch.arr())));
    OpMsg::setFlag(&initialReply, OpMsg::kMoreToCome);
    const auto initialReplyId = initialReply.header().getId();
    connection.fakeSession().queueReply(std::move(initialReply));

    ASSERT(cursor.init());

    auto firstContinuation =
        makeOpMsg(BSON("ok" << 1 << "cursor"
                            << BSON("id" << 42LL << "ns" << "test.coll"
                                         << "nextBatch" << BSON_ARRAY(BSON("_id" << 1)))));
    OpMsg::setFlag(&firstContinuation, OpMsg::kMoreToCome);
    firstContinuation.header().setResponseToMsgId(initialReplyId);
    const auto firstContinuationId = firstContinuation.header().getId();
    connection.fakeSession().queueReply(std::move(firstContinuation));

    auto finalContinuation =
        makeOpMsg(BSON("ok" << 1 << "cursor"
                            << BSON("id" << 0LL << "ns" << "test.coll"
                                         << "nextBatch" << BSON_ARRAY(BSON("_id" << 2)))));
    finalContinuation.header().setResponseToMsgId(firstContinuationId);
    connection.fakeSession().queueReply(std::move(finalContinuation));

    ASSERT(cursor.more());
    EXPECT_THAT(cursor.next(), unittest::match::BSONObjEQ(BSON("_id" << 1)));
    _clock.advance(Seconds{5});
    ASSERT(cursor.more());
    EXPECT_THAT(cursor.next(), unittest::match::BSONObjEQ(BSON("_id" << 2)));
    EXPECT_TRUE(cursor.isDead());

    EXPECT_EQ(countFor("initial"), 1);
    EXPECT_EQ(sumFor("initial"), 3000);
    EXPECT_EQ(countFor("exhaust_continuation"), 2);
    EXPECT_EQ(sumFor("exhaust_continuation"), 0);
}

TEST_F(DBClientConnectionTimingFixture, FailedReadStillRecordsTheAttempt) {
    FakeSyncConnection connection(_clock);
    connect(connection);
    connection.fakeSession().setWriteAdvance(Microseconds{3000});
    connection.fakeSession().setReadAdvance(Seconds{10});
    connection.fakeSession().setEndAdvance(Microseconds{1000});
    connection.fakeSession().setReadStatus(
        Status(ErrorCodes::HostUnreachable, "fake read failure"));

    ASSERT_THROWS_CODE(connection.runCommandWithTarget(makeCommandRequest()),
                       DBException,
                       ErrorCodes::HostUnreachable);

    EXPECT_EQ(countFor("initial"), 1);
    EXPECT_EQ(sumFor("initial"), 4000);
}

TEST_F(DBClientConnectionTimingFixture, FailedConnectRestoresTimingSuppression) {
    FakeSyncConnection connection(_clock);
    connect(connection);
    connection.setMakeSessionStatus(
        Status(ErrorCodes::HostUnreachable, "fake connection setup failure"));
    ASSERT_THROWS_CODE(connection.connect(HostAndPort("localhost", 27017), "test", boost::none),
                       DBException,
                       ErrorCodes::HostUnreachable);
    EXPECT_EQ(countFor("initial"), 0);

    connection.setMakeSessionStatus(Status::OK());
    connect(connection);
    connection.fakeSession().queueReply(makeOpMsg(BSON("ok" << 1)));
    connection.runCommandWithTarget(makeCommandRequest());
    EXPECT_EQ(countFor("initial"), 1);
}

TEST_F(DBClientConnectionTimingFixture, ExplicitConnectDoesNotRecordHandshake) {
    FakeSyncConnection connection(_clock);
    connection.fakeSession().queueReply(makeHelloReply());
    connection.connect(HostAndPort("localhost", 27017), "test", boost::none);

    EXPECT_EQ(countFor("initial"), 0);
    EXPECT_EQ(countFor("fire_and_forget"), 0);
    EXPECT_EQ(countFor("exhaust_continuation"), 0);
}

TEST_F(DBClientConnectionTimingFixture, ReconnectHandshakeTimeIsIncludedInCommandSample) {
    FakeSyncConnection connection(_clock, true /* autoReconnect */);
    connect(connection);
    connection.markFailed();
    connection.fakeSession().queueReply(makeHelloReply());
    connection.fakeSession().queueReply(makeOpMsg(BSON("ok" << 1)));
    connection.fakeSession().setReadAdvance(Microseconds{7000});

    connection.runCommandWithTarget(makeCommandRequest());

    EXPECT_EQ(countFor("initial"), 1);
    EXPECT_EQ(sumFor("initial"), 7000);
}

TEST_F(DBClientConnectionTimingFixture, FailedWriteStillRecordsTheAttempt) {
    FakeSyncConnection connection(_clock);
    connect(connection);
    connection.fakeSession().setWriteAdvance(Microseconds{3000});
    connection.fakeSession().setWriteStatus(
        Status(ErrorCodes::HostUnreachable, "fake write failure"));

    ASSERT_THROWS_CODE(connection.runCommandWithTarget(makeCommandRequest()),
                       DBException,
                       ErrorCodes::HostUnreachable);

    EXPECT_EQ(countFor("initial"), 1);
    EXPECT_EQ(sumFor("initial"), 3000);
}

MessageCompressorRegistry makeNoopRegistry() {
    MessageCompressorRegistry registry;
    auto compressor = std::make_unique<NoopMessageCompressor>();
    registry.setSupportedCompressors({compressor->getName()});
    registry.registerImplementation(std::move(compressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());
    return registry;
}

Message makeHelloReplyWithCompression() {
    return makeOpMsg(BSON("ok" << 1 << "minWireVersion" << 0 << "maxWireVersion"
                               << WireVersion::LATEST_WIRE_VERSION << "compression"
                               << BSON_ARRAY("noop")));
}

class DBClientConnectionCompressionFixture : public ServiceContextTest {
protected:
    // Replaces the connection's compressor manager with one backed by 'registry', which offers
    // the noop compressor, so the test does not depend on process-wide compression options.
    void useRegistry(FakeSyncConnection& connection) {
        connection.getCompressorManager() =
            MessageCompressorManager(MessageCompressionDirection::kEgress, &_registry);
    }

    static BSONObj sentBody(const Message& message) {
        ASSERT_NE(message.operation(), dbCompressed);
        return OpMsg::parseOwned(message).body;
    }

    void connectWithHello(FakeSyncConnection& connection, Message helloReply) {
        connection.fakeSession().queueReply(std::move(helloReply));
        connection.connect(HostAndPort("localhost", 27017), "test", boost::none);
    }

    TickSourceMock<Microseconds> _clock;
    MessageCompressorRegistry _registry = makeNoopRegistry();
};

TEST_F(DBClientConnectionCompressionFixture, DefaultConnectionNegotiatesCompression) {
    FakeSyncConnection connection(_clock);
    useRegistry(connection);
    connectWithHello(connection, makeHelloReplyWithCompression());

    const auto& sent = connection.fakeSession().sentMessages();
    ASSERT_EQ(sent.size(), 1U);
    auto hello = sentBody(sent[0]);
    ASSERT_TRUE(hello.hasField("compression"));
    ASSERT_EQ(connection.getCompressorManager().getNegotiatedCompressors().size(), 1U);

    connection.fakeSession().queueReply(makeOpMsg(BSON("ok" << 1)));
    connection.runCommandWithTarget(makeCommandRequest());
    ASSERT_EQ(sent.size(), 2U);
    ASSERT_EQ(sent[1].operation(), dbCompressed);
}

TEST_F(DBClientConnectionCompressionFixture, DisabledConnectionDoesNotOfferCompression) {
    FakeSyncConnection connection(_clock, false /* autoReconnect */, true /* disableCompression */);
    useRegistry(connection);
    ASSERT_TRUE(connection.isCompressionDisabled());

    // Even if the server (incorrectly) replies with a compressor list, it must be ignored.
    connectWithHello(connection, makeHelloReplyWithCompression());

    const auto& sent = connection.fakeSession().sentMessages();
    ASSERT_EQ(sent.size(), 1U);
    auto hello = sentBody(sent[0]);
    ASSERT_FALSE(hello.hasField("compression"));
    ASSERT_TRUE(connection.getCompressorManager().getNegotiatedCompressors().empty());

    connection.fakeSession().queueReply(makeOpMsg(BSON("ok" << 1)));
    connection.runCommandWithTarget(makeCommandRequest());
    ASSERT_EQ(sent.size(), 2U);
    ASSERT_NE(sent[1].operation(), dbCompressed);
}

TEST_F(DBClientConnectionCompressionFixture, DisabledConnectionClearsStaleStateOnReconnect) {
    FakeSyncConnection connection(_clock, true /* autoReconnect */, true /* disableCompression */);
    useRegistry(connection);

    // Simulate compressor state left over from a previous session on the same client object.
    auto& manager = connection.getCompressorManager();
    BSONObjBuilder offer;
    manager.clientBegin(&offer);
    manager.clientFinish(BSON("compression" << BSON_ARRAY("noop")));
    ASSERT_FALSE(manager.getNegotiatedCompressors().empty());

    // The reconnect handshake must neither be compressed with the stale compressor nor offer
    // compression.
    connectWithHello(connection, makeHelloReply());

    const auto& sent = connection.fakeSession().sentMessages();
    ASSERT_EQ(sent.size(), 1U);
    auto hello = sentBody(sent[0]);
    ASSERT_FALSE(hello.hasField("compression"));
    ASSERT_TRUE(manager.getNegotiatedCompressors().empty());
}

TEST_F(DBClientConnectionCompressionFixture, DisabledConnectionStaysUncompressedAfterAutoReconnect) {
    FakeSyncConnection connection(_clock, true /* autoReconnect */, true /* disableCompression */);
    useRegistry(connection);
    connectWithHello(connection, makeHelloReplyWithCompression());

    connection.markFailed();
    connection.fakeSession().queueReply(makeHelloReplyWithCompression());
    connection.fakeSession().queueReply(makeOpMsg(BSON("ok" << 1)));
    connection.runCommandWithTarget(makeCommandRequest());

    const auto& sent = connection.fakeSession().sentMessages();
    ASSERT_EQ(sent.size(), 3U);
    for (const auto& message : sent) {
        ASSERT_NE(message.operation(), dbCompressed);
    }
    ASSERT_FALSE(sentBody(sent[1]).hasField("compression"));
    ASSERT_TRUE(connection.getCompressorManager().getNegotiatedCompressors().empty());
}

}  // namespace

class DBClientConnectionFixture : public unittest::Test {};

TEST_F(DBClientConnectionFixture, TestTimeOutCtor) {
    ASSERT_EQUALS(DBClientConnection({.autoReconnect = true, .soTimeout = 10}).getSoTimeout(), 10);
    ASSERT_EQUALS(DBClientConnection({.autoReconnect = true}).getSoTimeout(), 0);
    ASSERT_EQUALS(DBClientConnection({.autoReconnect = true, .soTimeout = -1}).getSoTimeout(), 0);

    auto timeoutMx = Milliseconds::max().count() / 1000;
    ASSERT_EQUALS(
        DBClientConnection({.autoReconnect = true, .soTimeout = static_cast<double>(timeoutMx)})
            .getSoTimeout(),
        timeoutMx);

    auto timeout = timeoutMx + 1.0;
    ASSERT_EQUALS(DBClientConnection({.autoReconnect = true, .soTimeout = timeout}).getSoTimeout(),
                  timeoutMx);
}

TEST_F(DBClientConnectionFixture, TestTimeOutSetter) {
    auto session = DBClientConnection({.autoReconnect = true});

    session.setSoTimeout(10);
    ASSERT_EQUALS(session.getSoTimeout(), 10);

    session.setSoTimeout(-1);
    ASSERT_EQUALS(session.getSoTimeout(), 0);

    auto timeoutMx = Milliseconds::max().count() / 1000;
    session.setSoTimeout(timeoutMx);
    ASSERT_EQUALS(session.getSoTimeout(), timeoutMx);

    auto timeout = timeoutMx + 1.0;
    session.setSoTimeout(timeout);
    ASSERT_EQUALS(session.getSoTimeout(), timeoutMx);
}

}  // namespace mongo
