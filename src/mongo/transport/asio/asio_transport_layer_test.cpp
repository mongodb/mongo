/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/transport/asio/asio_transport_layer.h"

#include <exception>
#include <fstream>
#include <queue>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <fmt/format.h>

#include "mongo/client/dbclient_connection.h"
#include "mongo/config.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/stats/counters.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/asio/asio_tcp_fast_open.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

using namespace fmt::literals;

#ifdef _WIN32
using SetsockoptPtr = char*;
#else
using SetsockoptPtr = void*;
#endif

std::string testHostName() {
    return "127.0.0.1";
}

asio::ip::address testHostAddr() {
    return asio::ip::make_address(testHostName());
}

template <typename T>
struct ScopedValueGuard {
public:
    ScopedValueGuard(T& target, T value)
        : _target{target}, _saved{std::exchange(_target, std::move(value))} {}

    ~ScopedValueGuard() {
        _target = std::move(_saved);
    }

private:
    T& _target;
    T _saved;
};

std::string loadFile(std::string filename) try {
    std::ifstream f;
    f.exceptions(f.exceptions() | std::ios::failbit);
    f.open(filename);
    return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
} catch (const std::ifstream::failure& ex) {
    auto ec = lastSystemError();
    FAIL("Failed to load file: \"{}\": {}: {}"_format(filename, ex.what(), errorMessage(ec)));
    MONGO_UNREACHABLE;
}

class ConnectionThread {
public:
    explicit ConnectionThread(int port) : ConnectionThread(port, nullptr) {}
    ConnectionThread(int port, std::function<void(ConnectionThread&)> onConnect)
        : _port{port}, _onConnect{std::move(onConnect)}, _thread{[this] {
              _run();
          }} {}

    ~ConnectionThread() {
        LOGV2(6109500, "connection: Tx stop request");
        _stopRequest.set(true);
        _thread.join();
        LOGV2(6109501, "connection: joined");
    }

    void close() {
        _s.close();
    }

    Socket& socket() {
        return _s;
    }

    void wait() {
        LOGV2(7079402, "connection: wait()");
        _ready.get();
        if (_exception) {
            LOGV2(7079403, "connection: wait() rethrowing");
            std::rethrow_exception(_exception);
        }
    }

private:
    void _run() try {
        ASSERT_TRUE(_s.connect(SockAddr::create(testHostName(), _port, AF_INET)));
        LOGV2(6109502, "connected", "port"_attr = _port);
        if (_onConnect)
            _onConnect(*this);
        _ready.set();
        LOGV2(7079404, "connection: await stop request");
        _stopRequest.get();
        LOGV2(6109503, "connection: Rx stop request");
    } catch (...) {
        LOGV2(7079405, "connection: catching exception");
        _exception = std::current_exception();
        _ready.set();
    }

    int _port;
    std::function<void(ConnectionThread&)> _onConnect;
    std::exception_ptr _exception;
    Notification<void> _ready;
    Notification<bool> _stopRequest;
    Socket _s;
    test::JoinThread _thread;  // Appears after the members _run uses.
};

class SyncClient {
public:
    explicit SyncClient(int port) {
        std::error_code ec;
        _sock.connect(asio::ip::tcp::endpoint(testHostAddr(), port), ec);
        ASSERT_FALSE(ec) << errorMessage(ec);
        LOGV2(6109504, "sync client connected");
    }

    std::error_code write(const char* buf, size_t bufSize) {
        std::error_code ec;
        asio::write(_sock, asio::buffer(buf, bufSize), ec);
        return ec;
    }

private:
    asio::io_context _ctx{};
    asio::ip::tcp::socket _sock{_ctx};
};

template <typename AsioWriter>
void ping(AsioWriter& client) {
    OpMsgBuilder builder;
    builder.setBody(BSON("ping" << 1));
    Message msg = builder.finish();
    msg.header().setResponseToMsgId(0);
    msg.header().setId(0);
    OpMsg::appendChecksum(&msg);
    auto ec = client.write(msg.buf(), msg.size());
    ASSERT_FALSE(ec) << errorMessage(ec);
}

AsioTransportLayer::Options defaultTLAOptions() {
    ServerGlobalParams params;
    params.noUnixSocket = true;
    params.bind_ips = {testHostName()};
    AsioTransportLayer::Options opts(&params);
    opts.port = 0;
    return opts;
}

std::unique_ptr<AsioTransportLayer> makeTLA(std::unique_ptr<SessionManager> sessionManager,
                                            const AsioTransportLayer::Options& options) {
    auto tla = std::make_unique<AsioTransportLayer>(options, std::move(sessionManager));
    ASSERT_OK(tla->setup());
    ASSERT_OK(tla->start());
    return tla;
}

std::unique_ptr<AsioTransportLayer> makeTLA(std::unique_ptr<SessionManager> sessionManager) {
    return makeTLA(std::move(sessionManager), defaultTLAOptions());
}

/**
 * Properly setting up and tearing down the MockSessionManager and AsioTransportLayer is
 * tricky. Most tests can delegate the details to this TestFixture.
 */
class TestFixture {
public:
    TestFixture() : TestFixture(defaultTLAOptions()) {}

    explicit TestFixture(const AsioTransportLayer::Options& options) {
        auto sm = std::make_unique<test::MockSessionManager>();
        _sessionManager = sm.get();
        _tla = makeTLA(std::move(sm), options);
    }

    ~TestFixture() {
        _sessionManager->endAllSessions({});
        _tla->shutdown();
    }

    test::MockSessionManager& sessionManager() {
        return *_sessionManager;
    }

    AsioTransportLayer& tla() {
        return *_tla;
    }

    void setUpHangDuringAcceptingFirstConnection() {
        _hangDuringAcceptTimesEntered = _hangDuringAccept.setMode(FailPoint::alwaysOn);
    }

    void waitForHangDuringAcceptingFirstConnection() {
        _hangDuringAccept.waitForTimesEntered(_hangDuringAcceptTimesEntered + 1);
    }

    void waitForHangDuringAcceptingNextConnection() {
        _hangBeforeAcceptTimesEntered = _hangBeforeAccept.setMode(FailPoint::alwaysOn);
        _hangDuringAccept.setMode(FailPoint::off);
        _hangBeforeAccept.waitForTimesEntered(_hangBeforeAcceptTimesEntered + 1);

        _hangDuringAcceptTimesEntered = _hangDuringAccept.setMode(FailPoint::alwaysOn);
        _hangBeforeAccept.setMode(FailPoint::off);
        _hangDuringAccept.waitForTimesEntered(_hangDuringAcceptTimesEntered + 1);
    }

    void stopHangDuringAcceptingConnection() {
        _hangDuringAccept.setMode(FailPoint::off);
    }

private:
    std::unique_ptr<AsioTransportLayer> _tla;
    test::MockSessionManager* _sessionManager;

    FailPoint& _hangBeforeAccept = asioTransportLayerHangBeforeAcceptCallback;
    FailPoint& _hangDuringAccept = asioTransportLayerHangDuringAcceptCallback;

    FailPoint::EntryCountT _hangBeforeAcceptTimesEntered{0};
    FailPoint::EntryCountT _hangDuringAcceptTimesEntered{0};
};

TEST(AsioTransportLayer, ListenerPortZeroTreatedAsEphemeral) {
    Notification<bool> connected;
    TestFixture tf;
    tf.sessionManager().setOnStartSession([&](auto&&) { connected.set(true); });

    int port = tf.tla().listenerPort();
    ASSERT_GT(port, 0);
    LOGV2(6109514, "AsioTransportLayer listening", "port"_attr = port);

    ConnectionThread connectThread(port);
    connected.get();
}

void setNoLinger(ConnectionThread& conn) {
    // Linger timeout = 0 causes a RST packet on close.
    struct linger sl = {1, 0};
    if (setsockopt(conn.socket().rawFD(),
                   SOL_SOCKET,
                   SO_LINGER,
                   reinterpret_cast<SetsockoptPtr>(&sl),
                   sizeof(sl)) != 0) {
        auto err = make_error_code(std::errc{errno});
        LOGV2_ERROR(6276301, "setsockopt", "error"_attr = err.message());
    }
}

/**
 * Test that the server appropriately handles a client-side socket disconnection, and that the
 * client sends an RST packet when the socket is forcibly closed.
 */
TEST(AsioTransportLayer, TCPResetAfterConnectionIsSilentlySwallowed) {
    TestFixture tf;

    AtomicWord<int> sessionsCreated{0};
    tf.sessionManager().setOnStartSession([&](auto&&) { sessionsCreated.fetchAndAdd(1); });

    LOGV2(6109515, "connecting");
    auto& fp = asioTransportLayerHangDuringAcceptCallback;
    auto timesEntered = fp.setMode(FailPoint::alwaysOn);
    ConnectionThread connectThread(tf.tla().listenerPort(), &setNoLinger);
    fp.waitForTimesEntered(timesEntered + 1);
    // Test case thread does not close socket until client thread has set options to ensure that an
    // RST packet will be set on close.
    connectThread.wait();

    LOGV2(6109516, "closing");
    connectThread.close();
    fp.setMode(FailPoint::off);

    ASSERT_EQ(sessionsCreated.load(), 0);
}

TEST(AsioTransportLayer, StopAcceptingSessionsBeforeStart) {
    auto sm = std::make_unique<test::MockSessionManager>();
    auto tla = std::make_unique<AsioTransportLayer>(defaultTLAOptions(), std::move(sm));
    ON_BLOCK_EXIT([&] { tla->shutdown(); });

    ASSERT_OK(tla->setup());
    tla->stopAcceptingSessions();
    ASSERT_OK(tla->start());
}

#ifdef __linux__
/**
 * Test that the server successfully captures the TCP socket queue depth, and places the value both
 * into the AsioTransportLayer class and FTDC output.
 */
TEST(AsioTransportLayer, TCPCheckQueueDepth) {
    // Set the listenBacklog to a parameter greater than the number of connection threads we intend
    // to create (temporarily).
    ScopeGuard sg = [v = std::exchange(serverGlobalParams.listenBacklog, 10)] {
        serverGlobalParams.listenBacklog = v;
    };
    TestFixture tf;

    LOGV2(6400501, "Starting and hanging three connection threads");
    tf.setUpHangDuringAcceptingFirstConnection();

    ConnectionThread connectThread1(tf.tla().listenerPort());
    ConnectionThread connectThread2(tf.tla().listenerPort());
    ConnectionThread connectThread3(tf.tla().listenerPort());

    tf.waitForHangDuringAcceptingFirstConnection();
    connectThread1.wait();
    connectThread2.wait();
    connectThread3.wait();

    LOGV2(6400502, "Processing one connection thread");

    tf.waitForHangDuringAcceptingNextConnection();

    const auto& depths = tf.tla().getListenerSocketBacklogQueueDepths();
    ASSERT_EQ(depths.size(), 1);

    auto depth = depths[0];
    ASSERT_EQ(depth.first.getPort(), tf.tla().listenerPort());
    ASSERT_EQ(depth.second, 2);


    BSONObjBuilder tlaFTDCBuilder;
    tf.tla().appendStatsForFTDC(tlaFTDCBuilder);
    BSONObj tlaFTDCStats = tlaFTDCBuilder.obj();

    const auto& queueDepthsArray =
        tlaFTDCStats.getField("listenerSocketBacklogQueueDepths").Array();
    ASSERT_EQ(queueDepthsArray.size(), 1);

    const auto& queueDepthObj = queueDepthsArray[0].Obj();
    ASSERT_EQ(HostAndPort(queueDepthObj.firstElementFieldName()).port(), tf.tla().listenerPort());
    ASSERT_EQ(queueDepthObj.firstElement().Int(), 2);

    LOGV2(6400503, "Stopping failpoints, shutting down test");

    tf.stopHangDuringAcceptingConnection();
}
#endif

TEST(AsioTransportLayer, ThrowOnNetworkErrorInEnsureSync) {
    TestFixture tf;
    Notification<test::SessionThread*> mockSessionCreated;
    tf.sessionManager().setOnStartSession(
        [&](test::SessionThread& st) { mockSessionCreated.set(&st); });

    ConnectionThread connectThread(tf.tla().listenerPort(), &setNoLinger);

    // We set the timeout to ensure that the setsockopt calls are actually made in ensureSync()
    auto& st = *mockSessionCreated.get();
    st.session()->setTimeout(Milliseconds{500});

    // Synchronize with the connection thread to ensure the connection is closed only after the
    // connection thread returns from calling `setsockopt`.
    connectThread.wait();
    connectThread.close();

    // On Mac, setsockopt will immediately throw a SocketException since the socket is closed.
    // On Linux, we will throw HostUnreachable once we try to actually read the socket.
    // We allow for either exception here.
    using namespace unittest::match;
    ASSERT_THAT(
        st.session()->sourceMessage().getStatus(),
        StatusIs(AnyOf(Eq(ErrorCodes::HostUnreachable), Eq(ErrorCodes::SocketException)), Any()));
}

/* check that timeouts actually time out */
TEST(AsioTransportLayer, SourceSyncTimeoutTimesOut) {
    TestFixture tf;
    Notification<StatusWith<Message>> received;
    tf.sessionManager().setOnStartSession([&](test::SessionThread& st) {
        st.session()->setTimeout(Milliseconds{500});
        st.schedule([&](auto& session) { received.set(session.sourceMessage()); });
    });
    SyncClient conn(tf.tla().listenerPort());
    ASSERT_EQ(received.get().getStatus(), ErrorCodes::NetworkTimeout);
}

/* check that timeouts don't time out unless there's an actual timeout */
TEST(AsioTransportLayer, SourceSyncTimeoutSucceeds) {
    TestFixture tf;
    Notification<StatusWith<Message>> received;
    tf.sessionManager().setOnStartSession([&](test::SessionThread& st) {
        st.session()->setTimeout(Milliseconds{500});
        st.schedule([&](auto& session) { received.set(session.sourceMessage()); });
    });
    SyncClient conn(tf.tla().listenerPort());
    ping(conn);  // This time we send a message
    ASSERT_OK(received.get().getStatus());
}

/** Switching from timeouts to no timeouts must reset the timeout to unlimited. */
TEST(AsioTransportLayer, SwitchTimeoutModes) {
    TestFixture tf;
    Notification<test::SessionThread*> mockSessionCreated;
    tf.sessionManager().setOnStartSession(
        [&](test::SessionThread& st) { mockSessionCreated.set(&st); });

    SyncClient conn(tf.tla().listenerPort());
    auto& st = *mockSessionCreated.get();

    {
        LOGV2(6109525, "The first message we source should time out");
        Notification<StatusWith<Message>> done;
        st.schedule([&](auto& session) {
            session.setTimeout(Milliseconds{500});
            done.set(session.sourceMessage());
        });
        ASSERT_EQ(done.get().getStatus(), ErrorCodes::NetworkTimeout);
        LOGV2(6109526, "timed out successfully");
    }
    {
        LOGV2(6109527, "Verify a message can be successfully received");
        Notification<StatusWith<Message>> done;
        st.schedule([&](auto& session) { done.set(session.sourceMessage()); });
        ping(conn);
        ASSERT_OK(done.get().getStatus());
    }
    {
        LOGV2(6109528, "Clear the timeout and verify reception of a late message.");
        Notification<StatusWith<Message>> done;
        st.schedule([&](auto& session) {
            LOGV2(6109529, "waiting for message without a timeout");
            session.setTimeout({});
            done.set(session.sourceMessage());
        });
        sleepFor(Seconds{1});
        ping(conn);
        ASSERT_OK(done.get().getStatus());
    }
}

class Acceptor {
public:
    struct Connection {
        explicit Connection(asio::io_context& ioCtx) : socket(ioCtx) {}
        asio::ip::tcp::socket socket;
    };

    explicit Acceptor(asio::io_context& ioCtx) : _ioCtx(ioCtx) {
        asio::ip::tcp::endpoint endpoint(testHostAddr(), 0);
        _acceptor.open(endpoint.protocol());
        _acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        _acceptor.bind(endpoint);
        _acceptor.listen();
        LOGV2(6101600, "Acceptor: listening", "port"_attr = _acceptor.local_endpoint().port());

        _acceptLoop();
    }

    int port() const {
        return _acceptor.local_endpoint().port();
    }

    void setOnAccept(std::function<void(std::shared_ptr<Connection>)> cb) {
        _onAccept = std::move(cb);
    }

private:
    void _acceptLoop() {
        auto conn = std::make_shared<Connection>(_acceptor.get_executor().context());
        LOGV2(6101603, "Acceptor: await connection");
        _acceptor.async_accept(conn->socket, [this, conn](const asio::error_code& ec) {
            if (ec != asio::error_code{}) {
                LOGV2(6101605, "Accept", "error"_attr = errorCodeToStatus(ec));
            } else {
                if (_onAccept)
                    _onAccept(conn);
            }
            _acceptLoop();
        });
    }

    asio::io_context& _ioCtx;
    asio::ip::tcp::acceptor _acceptor{_ioCtx};
    std::function<void(std::shared_ptr<Connection>)> _onAccept;
};

/**
 * Create a connection and configure it with transport::tfo routines.
 */
class TfoClient {
public:
    explicit TfoClient(int port) {
        std::error_code ec;
        auto ep = asio::ip::tcp::endpoint(testHostAddr(), port);

        _sock.open(ep.protocol(), ec);
        ASSERT_FALSE(ec) << errorMessage(ec);

        ec = tfo::initOutgoingSocket(_sock);
        ASSERT_FALSE(ec) << errorMessage(ec);

        _sock.non_blocking(true);
        _sock.connect(ep, ec);
        ASSERT_FALSE(ec) << errorMessage(ec);
        LOGV2(7097407, "TFO client connected");
        _sock.non_blocking(false);
    }

    std::error_code write(const char* buf, size_t bufSize) {
        std::error_code ec;
        asio::write(_sock, asio::buffer(buf, bufSize), ec);
        return ec;
    }

private:
    asio::io_context _ctx{};
    AsioSession::GenericSocket _sock{_ctx};
};

void runTfoScenario(bool serverOn, bool clientOn, bool expectTfo) {
    if (auto err = tfo::ensureInitialized(true); !err.isOK()) {
        LOGV2(7097410, "Skipping: No TcpFastOpen", "error"_attr = err);
        return;
    }
    // Simulate a TFO system for which the initialization had produced the
    // specified configuration:
    auto passive = false;           // The TFO options were explicitly given
    auto initError = Status::OK();  // Initialization succeeded
    auto qSz = 20;                  // Queue depth
    auto config = tfo::setConfigForTest(passive, clientOn, serverOn, qSz, initError);

    auto extractTfoAccepted = [] {
        BSONObjBuilder bob;
        networkCounter.append(bob);
        auto obj = bob.obj();
        auto accepted = obj["tcpFastOpen"]["accepted"].numberInt();
        return std::pair{accepted, obj};
    };

    TestFixture tf;
    test::BlockingQueue<StatusWith<Message>> received;

    auto connectOnce = [&] {
        TfoClient conn(tf.tla().listenerPort());
        ping(conn);
        ASSERT_OK(received.pop().getStatus());
    };

    // Minimal test service. Consumes one `Message` from an incoming connection,
    // pushing it to the `received` queue.
    tf.sessionManager().setOnStartSession([&](test::SessionThread& st) {
        st.schedule([&](auto& session) { received.push(session.sourceMessage()); });
    });

    // Make one throwaway connection just to allow establishment of a TFO cookie.
    connectOnce();
    auto rec0 = extractTfoAccepted();
    connectOnce();
    auto rec1 = extractTfoAccepted();

    auto newTfos = rec1.first - rec0.first;
    if (newTfos != static_cast<int>(expectTfo)) {
        LOGV2_WARNING(8397800,
                      "In practice we sometimes do not get TFO when we expect to. "
                      "We are logging these events but not failing the test (See SERVER-83978)",
                      "serverOn"_attr = serverOn,
                      "clientOn"_attr = clientOn,
                      "expectTfo"_attr = expectTfo,
                      "rec0"_attr = rec0.second,
                      "rec1"_attr = rec1.second);
        // Only really asserting that we do not unexpectedly get TFO.
        ASSERT_LTE(newTfos, static_cast<int>(expectTfo));
    }
}

TEST(AsioTransportLayer, TcpFastOpenWithServerOffClientOff) {
    runTfoScenario(false, false, false);
}

TEST(AsioTransportLayer, TcpFastOpenWithServerOffClientOn) {
    runTfoScenario(false, true, false);
}

TEST(AsioTransportLayer, TcpFastOpenWithServerOnClientOn) {
    runTfoScenario(true, true, true);
}

TEST(AsioTransportLayer, TcpFastOpenWithServerOnClientOff) {
    runTfoScenario(true, false, false);
}

/**
 * Have `AsioTransportLayer` make a egress connection and observe behavior when
 * that connection is immediately reset by the peer. Test that if this happens
 * during the `AsioSession` constructor, that the thrown `asio::system_error`
 * is handled safely (translated to a Status holding a SocketException).
 */
TEST(AsioTransportLayer, EgressConnectionResetByPeerDuringSessionCtor) {
    // Under TFO, no SYN is sent until the client has data to send.  For this
    // test, we need the server to respond when the client hits the failpoint
    // in the AsioSession ctor. So we have to disable TFO.
    auto tfoConfig = tfo::setConfigForTest(0, 0, 0, 1024, Status::OK());

    // The `server` accepts connections, only to immediately reset them.
    TestFixture tf;
    asio::io_context ioContext;

    // `fp` pauses the `AsioSession` constructor immediately prior to its
    // `setsockopt` sequence, to allow time for the peer reset to propagate.
    auto fp = std::make_unique<FailPointEnableBlock>(
        "asioTransportLayerSessionPauseBeforeSetSocketOption");

    Acceptor server(ioContext);
    server.setOnAccept([&](std::shared_ptr<Acceptor::Connection> conn) {
        LOGV2(7598701, "waiting for the client to reach the fail-point");
        (*fp)->waitForTimesEntered(fp->initialTimesEntered() + 1);
        LOGV2(6101604, "handling a connection by resetting it");
        conn->socket.set_option(asio::socket_base::linger(true, 0));
        conn->socket.close();
        fp.reset();
    });
    test::JoinThread ioThread{[&] {
        ioContext.run();
    }};
    ScopeGuard ioContextStop = [&] {
        ioContext.stop();
    };

    LOGV2(6101602, "Connecting", "port"_attr = server.port());
    using namespace unittest::match;
    // On MacOS, calling `setsockopt` on a peer-reset connection yields an
    // `EINVAL`. On Linux and Windows, the `setsockopt` completes successfully.
    // Either is okay, but the `AsioSession` ctor caller is expected to handle
    // `asio::system_error` and convert it to `SocketException`.
    ASSERT_THAT(
        tf.tla()
            .connect({testHostName(), server.port()}, ConnectSSLMode::kDisableSSL, Seconds{10}, {})
            .getStatus(),
        StatusIs(AnyOf(Eq(ErrorCodes::SocketException), Eq(ErrorCodes::OK)), Any()));
}

/**
 * With no regard to mongo code, just check what the ASIO socket
 * implementation does in the reset connection scenario.
 */
TEST(AsioTransportLayer, ConfirmSocketSetOptionOnResetConnections) {
    asio::io_context ioContext;
    Acceptor server{ioContext};
    Notification<bool> accepted;
    Notification<bool> connected;
    Notification<boost::optional<std::error_code>> caught;
    server.setOnAccept([&](auto conn) {
        // onAccept callbacks can run before the client-side connect() call returns,
        // which means there's a race between this socket closing and connect()
        // returning. We use the connected flag to prevent the race.
        connected.get();
        conn->socket.set_option(asio::socket_base::linger(true, 0));
        conn->socket.close();
        sleepFor(Seconds{1});
        accepted.set(true);
    });
    test::JoinThread ioThread{[&] {
        ioContext.run();
    }};
    ScopeGuard ioContextStop = [&] {
        ioContext.stop();
    };
    test::JoinThread client{[&] {
        asio::ip::tcp::socket client{ioContext};
        client.connect(asio::ip::tcp::endpoint(testHostAddr(), server.port()));
        connected.set(true);
        accepted.get();
        // Just set any option and see what happens.
        try {
            client.set_option(asio::ip::tcp::no_delay(true));
            caught.set({});
        } catch (const std::system_error& e) {
            caught.set(e.code());
        }
    }};
    auto thrown = caught.get();
    LOGV2(6101610,
          "ASIO set_option response on peer-reset connection",
          "msg"_attr = "{}"_format(thrown ? thrown->message() : ""));
}

class AsioTransportLayerWithServiceContextTest : public ServiceContextTest {
public:
    class ThreadCounter {
    public:
        std::function<stdx::thread(std::function<void()>)> makeSpawnFunc() {
            return [core = _core](std::function<void()> cb) {
                {
                    stdx::lock_guard lk(core->mutex);
                    ++core->created;
                    core->cv.notify_all();
                }
                return stdx::thread{[core, cb = std::move(cb)]() mutable {
                    {
                        stdx::lock_guard lk(core->mutex);
                        ++core->started;
                        core->cv.notify_all();
                    }
                    cb();
                }};
            };
        }

        int64_t created() const {
            stdx::lock_guard lk(_core->mutex);
            return _core->created;
        }

        int64_t started() const {
            stdx::lock_guard lk(_core->mutex);
            return _core->started;
        }

        template <typename Pred>
        void waitForStarted(const Pred& pred) const {
            stdx::unique_lock lk(_core->mutex);
            _core->cv.wait(lk, [&] { return pred(_core->started); });
        }

    private:
        struct Core {
            mutable stdx::mutex mutex;  // NOLINT
            mutable stdx::condition_variable cv;
            int64_t created = 0;
            int64_t started = 0;
        };
        std::shared_ptr<Core> _core = std::make_shared<Core>();
    };

    void setUp() override {
        auto* svcCtx = getServiceContext();
        svcCtx->getService()->setServiceEntryPoint(
            std::make_unique<test::ServiceEntryPointUnimplemented>());
        auto tl = makeTLA(std::make_unique<test::MockSessionManager>());
        svcCtx->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
    }

    void tearDown() override {
        getServiceContext()->getTransportLayerManager()->shutdown();
    }

    AsioTransportLayer& tla() {
        auto tl = getServiceContext()->getTransportLayerManager()->getEgressLayer();
        return *checked_cast<AsioTransportLayer*>(tl);
    }
};

TEST_F(AsioTransportLayerWithServiceContextTest, TimerServiceDoesNotSpawnThreadsBeforeStart) {
    ThreadCounter counter;
    { AsioTransportLayer::TimerService service{{counter.makeSpawnFunc()}}; }
    ASSERT_EQ(counter.created(), 0);
}

TEST_F(AsioTransportLayerWithServiceContextTest, TimerServiceOneShotStart) {
    ThreadCounter counter;
    AsioTransportLayer::TimerService service{{counter.makeSpawnFunc()}};
    service.start();
    LOGV2(5490004, "Awaiting timer thread start", "threads"_attr = counter.started());
    counter.waitForStarted([](auto n) { return n > 0; });
    LOGV2(5490005, "Awaited timer thread start", "threads"_attr = counter.started());

    service.start();
    service.start();
    service.start();
    ASSERT_EQ(counter.created(), 1) << "Redundant start should spawn only once";
}

TEST_F(AsioTransportLayerWithServiceContextTest, TimerServiceDoesNotStartAfterStop) {
    ThreadCounter counter;
    AsioTransportLayer::TimerService service{{counter.makeSpawnFunc()}};
    service.stop();
    service.start();
    ASSERT_EQ(counter.created(), 0) << "Stop then start should not spawn";
}

TEST_F(AsioTransportLayerWithServiceContextTest, TimerServiceCanStopMoreThanOnce) {
    // Verifying that it is safe to have multiple calls to `stop()`.
    {
        AsioTransportLayer::TimerService service;
        service.start();
        service.stop();
        service.stop();
    }
    {
        AsioTransportLayer::TimerService service;
        service.stop();
        service.stop();
    }
}

TEST_F(AsioTransportLayerWithServiceContextTest, TransportStartAfterShutDown) {
    tla().shutdown();
    ASSERT_EQ(tla().start(), TransportLayer::ShutdownStatus);
}

#ifdef MONGO_CONFIG_SSL
#ifndef _WIN32
// TODO SERVER-62035: enable the following on Windows.
TEST_F(AsioTransportLayerWithServiceContextTest, ShutdownDuringSSLHandshake) {
    /**
     * Creates a server and a client thread:
     * - The server listens for incoming connections, but doesn't participate in SSL handshake.
     * - The client connects to the server, and is configured to perform SSL handshake.
     * The server never writes on the socket in response to the handshake request, thus the client
     * should block until it is timed out.
     * The goal is to simulate a server crash, and verify the behavior of the client, during the
     * handshake process.
     */
    int port = tla().listenerPort();

    DBClientConnection conn;
    conn.setSoTimeout(1);  // 1 second timeout

    TransientSSLParams params;
    params.sslClusterPEMPayload = loadFile("jstests/libs/client.pem");
    params.targetedClusterConnectionString = ConnectionString::forLocal();

    ASSERT_THROWS_CODE(conn.connectNoHello({testHostName(), port}, std::move(params)),
                       DBException,
                       ErrorCodes::HostUnreachable);
}
#endif  // _WIN32
#endif  // MONGO_CONFIG_SSL

class AsioTransportLayerWithRouterPortTest : public ServiceContextTest {
public:
    // We opted to use static ports for simplicity. If this results in test failures due to busy
    // ports, we may change the fixture, as well as the underlying transport layer, to dynamically
    // choose the listening ports.
    static constexpr auto kMainPort = 22000;
    static constexpr auto kRouterPort = 22001;

    void setUp() override {
        auto options = defaultTLAOptions();
        options.port = kMainPort;
        options.routerPort = kRouterPort;
        _fixture = std::make_unique<TestFixture>(options);
    }

    void tearDown() override {
        _fixture.reset();
    }

    test::MockSessionManager& sessionManager() {
        return _fixture->sessionManager();
    }

    StatusWith<std::shared_ptr<Session>> connect(HostAndPort remote) {
        return _fixture->tla().connect(remote, ConnectSSLMode::kDisableSSL, Seconds{10}, {});
    }

    void doDifferentiatesConnectionsCase(bool useRouterPort) {
        auto onStartSession = std::make_shared<Notification<void>>();
        sessionManager().setOnStartSession([&](test::SessionThread& st) {
            ASSERT_EQ(st.session()->isFromRouterPort(), useRouterPort);

            auto client =
                getServiceContext()->getService()->makeClient("RouterPortTest", st.session());
            ASSERT_EQ(client->isRouterClient(), useRouterPort);

            onStartSession->set();
        });
        HostAndPort target{testHostName(), useRouterPort ? kRouterPort : kMainPort};
        auto conn = connect(target);
        ASSERT_OK(conn) << " target={}"_format(target);
        onStartSession->get();
    }

private:
    RAIIServerParameterControllerForTest _scopedFeature{"featureFlagEmbeddedRouter", true};
    ScopedValueGuard<ClusterRole> _scopedClusterRole{
        serverGlobalParams.clusterRole, {ClusterRole::RouterServer, ClusterRole::ShardServer}};
    std::shared_ptr<void> _disableTfo = tfo::setConfigForTest(0, 0, 0, 1024, Status::OK());
    std::unique_ptr<TestFixture> _fixture;
};

TEST_F(AsioTransportLayerWithRouterPortTest, ListensOnBothPorts) {
    for (auto port : {kRouterPort, kMainPort}) {
        HostAndPort remote(testHostName(), port);
        ASSERT_OK(connect(remote).getStatus()) << "Unable to connect to " << remote;
    }
}

TEST_F(AsioTransportLayerWithRouterPortTest, DifferentiatesConnectionsMainPort) {
    doDifferentiatesConnectionsCase(false);
}

TEST_F(AsioTransportLayerWithRouterPortTest, DifferentiatesConnectionsRouterPort) {
    doDifferentiatesConnectionsCase(true);
}

#ifdef __linux__

/**
 * Creates a connection between a client and a server, then runs tests against the
 * `AsioNetworkingBaton` associated with the server-side of the connection (i.e., `Client`). The
 * client-side of this connection is associated with `_connThread`, and the server-side is wrapped
 * inside `_client`.
 */
class AsioNetworkingBatonTest : public ServiceContextTest {
public:
    /**
     * Emplaces a Promise with the first ingress session. Can optionally accept
     * further sessions, of which it takes co-ownership.
     */
    class FirstSessionManager : public SessionManager {
    public:
        explicit FirstSessionManager(Promise<std::shared_ptr<Session>> promise)
            : _promise(std::move(promise)) {}

        ~FirstSessionManager() override {
            _join();
        }

        void startSession(std::shared_ptr<Session> session) override {
            stdx::lock_guard lk{_mutex};
            _sessions.push_back(session);
            if (_promise) {
                _promise->emplaceValue(std::move(session));
                _promise.reset();
                return;
            }
            invariant(_allowMultipleSessions, "Unexpected multiple ingress sessions");
        }

        void endAllSessions(Client::TagMask tags) override {
            _join();
        }

        void endSessionByClient(Client*) override {}

        bool shutdown(Milliseconds) override {
            _join();
            return true;
        }

        size_t numOpenSessions() const override {
            stdx::lock_guard lk{_mutex};
            return _sessions.size();
        }

        void setAllowMultipleSessions() {
            _allowMultipleSessions = true;
        }

    private:
        void _join() {
            stdx::lock_guard lk{_mutex};
            _sessions.clear();
        }

        bool _allowMultipleSessions = false;
        mutable Mutex _mutex;
        std::vector<std::shared_ptr<Session>> _sessions;
        boost::optional<Promise<std::shared_ptr<Session>>> _promise;
    };

    // Used for setting and canceling timers on the networking baton. Does not offer any timer
    // functionality, and is only used for its unique id.
    class DummyTimer final : public ReactorTimer {
    public:
        void cancel(const BatonHandle& baton = nullptr) override {
            MONGO_UNREACHABLE;
        }

        Future<void> waitUntil(Date_t timeout, const BatonHandle& baton = nullptr) override {
            MONGO_UNREACHABLE;
        }
    };

    virtual void configureSessionManager(FirstSessionManager& mgr) {}

    void setUp() override {
        auto pf = makePromiseFuture<std::shared_ptr<Session>>();
        auto sessionManager = std::make_unique<FirstSessionManager>(std::move(pf.promise));
        configureSessionManager(*sessionManager);

        auto tl = makeTLA(std::move(sessionManager));
        const auto listenerPort = tl->listenerPort();

        auto* svcCtx = getServiceContext();
        svcCtx->getService()->setServiceEntryPoint(
            std::make_unique<test::ServiceEntryPointUnimplemented>());
        svcCtx->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));

        _connThread = std::make_unique<ConnectionThread>(listenerPort);
        _client = svcCtx->getService()->makeClient("NetworkBatonTest", pf.future.get());
    }

    void tearDown() override {
        _connThread.reset();
        getServiceContext()->getTransportLayerManager()->shutdown();
    }

    Client& client() {
        return *_client;
    }

    ConnectionThread& connection() {
        return *_connThread;
    }

    std::unique_ptr<ReactorTimer> makeDummyTimer() const {
        return std::make_unique<DummyTimer>();
    }

private:
    ServiceContext::UniqueClient _client;
    std::unique_ptr<ConnectionThread> _connThread;
};

class IngressAsioNetworkingBatonTest : public AsioNetworkingBatonTest {};

class EgressAsioNetworkingBatonTest : public AsioNetworkingBatonTest {
    void configureSessionManager(FirstSessionManager& mgr) override {
        mgr.setAllowMultipleSessions();
    }
};

// A `JoinThread` that waits for a ready signal from its underlying worker thread before returning
// from its constructor.
class MilestoneThread {
public:
    explicit MilestoneThread(std::function<void(Notification<void>&)> body)
        : _thread([this, body = std::move(body)]() mutable { body(_isReady); }) {
        _isReady.get();
    }

private:
    Notification<void> _isReady;
    test::JoinThread _thread;
};

void waitForTimesEntered(const FailPointEnableBlock& fp, FailPoint::EntryCountT times) {
    fp->waitForTimesEntered(fp.initialTimesEntered() + times);
}

TEST_F(IngressAsioNetworkingBatonTest, CanWait) {
    auto opCtx = client().makeOperationContext();
    BatonHandle baton = opCtx->getBaton();  // ensures the baton outlives its opCtx.

    auto netBaton = baton->networking();
    ASSERT(netBaton);
    ASSERT_TRUE(netBaton->canWait());

    opCtx.reset();  // detaches the baton, so it's no longer associated with an opCtx.
    ASSERT_FALSE(netBaton->canWait());
}

TEST_F(IngressAsioNetworkingBatonTest, MarkKillOnClientDisconnect) {
    auto opCtx = client().makeOperationContext();
    opCtx->markKillOnClientDisconnect();
    ASSERT_FALSE(opCtx->isKillPending());
    connection().wait();
    connection().close();

    // Once the connection is closed, `sleepFor` is expected to throw this exception.
    ASSERT_THROWS_CODE(opCtx->sleepFor(Seconds(5)), DBException, ErrorCodes::ClientDisconnect);
    ASSERT_EQ(opCtx->getKillStatus(), ErrorCodes::ClientDisconnect);
}

TEST_F(IngressAsioNetworkingBatonTest, Schedule) {
    // Note that the baton runs all scheduled jobs on the main test thread, so it's safe to use
    // assertions inside tasks scheduled on the baton.
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton();

    // Schedules a task on the baton, runs `func`, and expects the baton to run the scheduled task
    // and provide it with the `expected` status.
    auto runTest = [&](Status expected, std::function<void()> func) {
        bool pending = true;
        baton->schedule([&](Status status) {
            ASSERT_EQ(status, expected);
            pending = false;
        });
        ASSERT_TRUE(pending);
        func();
        ASSERT_FALSE(pending);
    };

    // 1) Baton runs the scheduled task when current thread blocks on `opCtx`.
    runTest(Status::OK(), [&] { opCtx->sleepFor(Milliseconds(1)); });

    // 2) Baton must run pending tasks on detach.
    const Status detachedError{ErrorCodes::ShutdownInProgress, "Baton detached"};
    runTest(detachedError, [&] { opCtx.reset(); });

    // 3) A detached baton immediately runs scheduled tasks.
    bool pending = true;
    baton->schedule([&](Status status) {
        ASSERT_EQ(status, detachedError);
        pending = false;
    });
    ASSERT_FALSE(pending);
}

TEST_F(IngressAsioNetworkingBatonTest, AddAndRemoveSession) {
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();

    auto session = client().session();
    auto future = baton->addSession(*session, NetworkingBaton::Type::In);
    ASSERT_TRUE(baton->cancelSession(*session));
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);

    // Canceling the same session again should not succeed
    ASSERT_FALSE(baton->cancelSession(*session));
}

TEST_F(IngressAsioNetworkingBatonTest, CancelUnknownSession) {
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();

    auto session = client().session();
    ASSERT_FALSE(baton->cancelSession(*session));
}

TEST_F(IngressAsioNetworkingBatonTest, AddAndRemoveSessionWhileInPoll) {
    // Attempts to add and remove a session while the baton is polling. This, for example, could
    // happen on `mongos` while an operation is blocked, waiting for `AsyncDBClient` to create an
    // egress connection, and then the connection has to be ended for some reason before the baton
    // returns from polling.
    auto opCtx = client().makeOperationContext();
    Notification<bool> cancelSessionResult;
    Notification<bool> cancelSessionAgainResult;

    MilestoneThread thread([&](Notification<void>& isReady) {
        auto baton = opCtx->getBaton()->networking();
        auto session = client().session();

        FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");
        isReady.set();
        waitForTimesEntered(fp, 1);

        // This thread is an external observer to the baton, so the expected behavior is for
        // `cancelSession` to happen after `addSession`, and thus it must return `true`.
        baton->addSession(*session, NetworkingBaton::Type::In).getAsync([](Status) {});
        cancelSessionResult.set(baton->cancelSession(*session));
        // A second cancel should return false
        cancelSessionAgainResult.set(baton->cancelSession(*session));
    });

    ASSERT_TRUE(cancelSessionResult.get(opCtx.get()));
    ASSERT_FALSE(cancelSessionAgainResult.get(opCtx.get()));
}

TEST_F(IngressAsioNetworkingBatonTest, CancelSessionTwiceWhileInPoll) {
    // Exercises the case where we cancel the same session twice while blocked polling,
    // specifically in the case where we can't run any scheduled tasks in between the two
    // cancellations.
    unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
        auto opCtx = client().makeOperationContext();
        auto baton = opCtx->getBaton()->networking();
        auto session = client().session();

        std::vector<Future<void>> futures;
        futures.push_back(baton->addSession(*session, NetworkingBaton::Type::In));

        stdx::thread thread;
        {
            FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");

            thread = monitor.spawn([&] {
                auto clkSource = getServiceContext()->getPreciseClockSource();
                auto state = baton->run_until(clkSource, Date_t::max());
                ASSERT_EQ(state, Waitable::TimeoutState::NoTimeout);
            });

            waitForTimesEntered(fp, 1);

            // We should be able to cancel the session, but the second attempt should return false.
            ASSERT_TRUE(baton->cancelSession(*session));
            ASSERT_FALSE(baton->cancelSession(*session));

            // Try to cancel again after re-adding the timer.
            futures.push_back(baton->addSession(*session, NetworkingBaton::Type::In));
            ASSERT_TRUE(baton->cancelSession(*session));
            ASSERT_FALSE(baton->cancelSession(*session));
        }

        // Let the polling thread finish polling
        baton->notify();

        thread.join();

        // Cancel should still return false.
        ASSERT_FALSE(baton->cancelSession(*session));

        // Check the futures all resolved to cancellation errors
        for (auto& future : futures) {
            ASSERT_EQ(future.getNoThrow(), ErrorCodes::CallbackCanceled);
        }
    });
}

TEST_F(IngressAsioNetworkingBatonTest, WaitAndNotify) {
    // Exercises the underlying `wait` and `notify` functionality through `AsioNetworkingBaton::run`
    // and `AsioNetworkingBaton::schedule`, respectively. Here is how this is done: 1) The main
    // thread starts polling (from inside `run`) when waiting on the notification. 2) Once the main
    // thread is ready to poll, `thread` notifies it through `baton->schedule`. 3) `schedule` calls
    // into `notify` internally, which should interrupt the polling. 4) Once polling is interrupted,
    // `baton` runs the scheduled job and sets the notification.
    auto opCtx = client().makeOperationContext();

    Notification<void> notification;
    MilestoneThread thread([&](Notification<void>& isReady) {
        auto baton = opCtx->getBaton()->networking();
        FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");
        isReady.set();
        waitForTimesEntered(fp, 1);
        baton->schedule([&](Status) { notification.set(); });
    });

    notification.get(opCtx.get());
}

TEST_F(IngressAsioNetworkingBatonTest, NotifyDuringPollWithSessions) {
    // Exercises the interaction between `notify()` and polling, specifically in the case where the
    // notification occurs during polling. `thread` waits until polling has begun and then sends
    // a notification, while the main thread verifies that `run_until()` is interrupted. This
    // test covers the case where the baton has an active session, which may affect the mechanism
    // used for polling.
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto clkSource = getServiceContext()->getPreciseClockSource();
    auto session = client().session();

    baton->addSession(*session, NetworkingBaton::Type::In).getAsync([](Status) {});

    MilestoneThread thread([&](Notification<void>& isReady) {
        FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");
        isReady.set();
        waitForTimesEntered(fp, 1);
        baton->notify();
    });

    const auto state = baton->run_until(clkSource, Date_t::max());
    ASSERT_EQ(state, Waitable::TimeoutState::NoTimeout);
}

TEST_F(IngressAsioNetworkingBatonTest, NotifyDuringPollNoSessions) {
    // Exercises the interaction between `notify()` and polling, specifically in the case where the
    // notification occurs during polling. `thread` waits until polling has begun and then sends
    // a notification, while the main thread verifies that `run_until()` is interrupted. This test
    // covers the case where the baton has no active sessions, which may affect the mechanism used
    // for polling.
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto clkSource = getServiceContext()->getPreciseClockSource();

    MilestoneThread thread([&](Notification<void>& isReady) {
        FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");
        isReady.set();
        waitForTimesEntered(fp, 1);
        baton->notify();
    });

    const auto state = baton->run_until(clkSource, Date_t::max());
    ASSERT_EQ(state, Waitable::TimeoutState::NoTimeout);
}

TEST_F(IngressAsioNetworkingBatonTest, NotifyBeforePollWithSessions) {
    // Exercises the interaction between `notify()` and polling in the case where the notification
    // occurs outside of polling. This test covers the case where the baton has active sessions,
    // which is a slightly different codepath from the no sessions case.
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto clkSource = getServiceContext()->getPreciseClockSource();
    auto session = client().session();

    baton->addSession(*session, NetworkingBaton::Type::In).getAsync([](Status) {});

    // Notification prevents timeout
    baton->notify();
    auto state = baton->run_until(clkSource, Date_t::max());
    ASSERT_EQ(state, Waitable::TimeoutState::NoTimeout);

    // No pre-existing notification yields a timeout
    state = baton->run_until(clkSource, clkSource->now() + Milliseconds(1));
    ASSERT_EQ(state, Waitable::TimeoutState::Timeout);
}

TEST_F(IngressAsioNetworkingBatonTest, NotifyBeforePollNoSessions) {
    // Exercises the interaction between `notify()` and polling in the case where the notification
    // occurs outside of polling. This test covers the case where the baton has no active sessions,
    // which is a slightly different codepath from the case where it has active sessions.
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto clkSource = getServiceContext()->getPreciseClockSource();

    // Notification prevents timeout
    baton->notify();
    auto state = baton->run_until(clkSource, Date_t::max());
    ASSERT_EQ(state, Waitable::TimeoutState::NoTimeout);

    // No pre-existing notification yields a timeout
    state = baton->run_until(clkSource, clkSource->now() + Milliseconds(1));
    ASSERT_EQ(state, Waitable::TimeoutState::Timeout);
}

void blockIfBatonPolls(Client& client,
                       std::function<void(const BatonHandle&, Notification<void>&)> modifyBaton) {
    Notification<void> notification;
    auto opCtx = client.makeOperationContext();

    FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");

    modifyBaton(opCtx->getBaton(), notification);

    // This will internally call into `AsioNetworkingBaton::run()`, which will block forever (since
    // the failpoint is enabled) if the baton starts polling.
    notification.get(opCtx.get());
}

TEST_F(IngressAsioNetworkingBatonTest, BatonWithPendingTasksNeverPolls) {
    blockIfBatonPolls(client(), [](const BatonHandle& baton, Notification<void>& notification) {
        baton->schedule([&](Status) { notification.set(); });
    });
}

TEST_F(IngressAsioNetworkingBatonTest, BatonWithAnExpiredTimerNeverPolls) {
    auto timer = makeDummyTimer();
    auto clkSource = getServiceContext()->getPreciseClockSource();

    // Use the ReactorTimer-accepting overload of waitUntil
    blockIfBatonPolls(client(), [&](const BatonHandle& baton, Notification<void>& notification) {
        // Batons use the precise clock source internally. We use the current time (i.e., `now()`)
        // as the deadline to schedule an expired timer on the baton.
        baton->networking()->waitUntil(*timer, clkSource->now()).getAsync([&](Status) {
            notification.set();
        });
    });
    // Do the same but with the CancellationToken-accepting overload of waitUntil
    blockIfBatonPolls(client(), [&](const BatonHandle& baton, Notification<void>& notification) {
        baton->networking()
            ->waitUntil(clkSource->now(), CancellationToken::uncancelable())
            .getAsync([&](Status) { notification.set(); });
    });
}

TEST_F(IngressAsioNetworkingBatonTest, WaitUntilWithUncancellableTokenFiresAtDeadline) {
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto deadline = Date_t::now() + Milliseconds(10);
    auto fut = baton->waitUntil(deadline, CancellationToken::uncancelable());
    // We expect this assertion to be evaluated before the deadline is reached.
    ASSERT_FALSE(fut.isReady());
    // Now wait until we reach the deadline. Since we wait on the baton's associated
    // opCtx, the baton's run() function should be invoked, and the
    // baton should be woken up to fire the timer and ready `fut` at the deadline.
    ASSERT_EQ(fut.getNoThrow(opCtx.get()), Status::OK());
}

TEST_F(IngressAsioNetworkingBatonTest, WaitUntilWithCanceledTokenIsCanceled) {
    CancellationSource source;
    auto token = source.token();
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto fut = baton->waitUntil(Date_t::now() + Seconds(10), token);
    ASSERT_FALSE(fut.isReady());
    source.cancel();
    auto expectedError = Status{ErrorCodes::CallbackCanceled, "Baton wait canceled"};
    ASSERT_EQ(fut.getNoThrow(opCtx.get()), expectedError);
}

TEST_F(IngressAsioNetworkingBatonTest, NotifyInterruptsRunUntilBeforeTimeout) {
    auto opCtx = client().makeOperationContext();
    MilestoneThread thread([&](Notification<void>& isReady) {
        auto baton = opCtx->getBaton();
        FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");
        isReady.set();
        waitForTimesEntered(fp, 1);
        baton->notify();
    });

    auto clkSource = getServiceContext()->getPreciseClockSource();
    const auto state = opCtx->getBaton()->run_until(clkSource, Date_t::max());
    ASSERT(state == Waitable::TimeoutState::NoTimeout);
}

TEST_F(IngressAsioNetworkingBatonTest, RunUntilProperlyTimesout) {
    auto opCtx = client().makeOperationContext();
    auto clkSource = getServiceContext()->getPreciseClockSource();
    const auto state = opCtx->getBaton()->run_until(clkSource, clkSource->now() + Milliseconds(1));
    ASSERT(state == Waitable::TimeoutState::Timeout);
}

TEST_F(IngressAsioNetworkingBatonTest, AddAndRemoveTimer) {
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();

    auto timer = makeDummyTimer();
    baton->waitUntil(*timer, Date_t::max()).getAsync([](Status) {});

    ASSERT_TRUE(baton->cancelTimer(*timer));

    // Canceling the same timer again should not succeed
    ASSERT_FALSE(baton->cancelTimer(*timer));
}

TEST_F(IngressAsioNetworkingBatonTest, CancelUnknownTimer) {
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();

    auto timer = makeDummyTimer();
    ASSERT_FALSE(baton->cancelTimer(*timer));
}

TEST_F(IngressAsioNetworkingBatonTest, AddAndRemoveTimerWhileInPoll) {
    auto opCtx = client().makeOperationContext();
    Notification<bool> cancelTimerResult;

    MilestoneThread thread([&](Notification<void>& isReady) {
        auto baton = opCtx->getBaton()->networking();

        FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");
        isReady.set();
        waitForTimesEntered(fp, 1);

        // This thread is an external observer to the baton, so the expected behavior is for
        // `cancelTimer` to happen after `waitUntil`, thus canceling the timer must return `true`.
        auto timer = makeDummyTimer();
        baton->waitUntil(*timer, Date_t::max()).getAsync([](Status) {});
        cancelTimerResult.set(baton->cancelTimer(*timer));
    });

    ASSERT_TRUE(cancelTimerResult.get(opCtx.get()));
}

TEST_F(IngressAsioNetworkingBatonTest, CancelTimerTwiceWhileInPoll) {
    // Exercises the case where we cancel the same timer twice while blocked polling,
    // specifically in the case where we can't run any scheduled tasks in between the two
    // cancellations.
    unittest::threadAssertionMonitoredTest([&](unittest::ThreadAssertionMonitor& monitor) {
        auto opCtx = client().makeOperationContext();
        auto baton = opCtx->getBaton()->networking();
        auto timer = makeDummyTimer();

        std::vector<Future<void>> futures;
        futures.push_back(baton->waitUntil(*timer, Date_t::max()));

        stdx::thread thread;
        {
            FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");

            thread = monitor.spawn([&] {
                auto clkSource = getServiceContext()->getPreciseClockSource();
                auto state = baton->run_until(clkSource, Date_t::max());
                ASSERT_EQ(state, Waitable::TimeoutState::NoTimeout);
            });

            waitForTimesEntered(fp, 1);

            // We should be able to cancel the timer. A second attempt should return false
            ASSERT_TRUE(baton->cancelTimer(*timer));
            ASSERT_FALSE(baton->cancelTimer(*timer));

            // Try to cancel again after re-adding the timer
            futures.push_back(baton->waitUntil(*timer, Date_t::max()));
            ASSERT_TRUE(baton->cancelTimer(*timer));
            ASSERT_FALSE(baton->cancelTimer(*timer));
        }

        // Let the polling thread finish polling
        baton->notify();

        thread.join();

        // We should still get false from cancel
        ASSERT_FALSE(baton->cancelTimer(*timer));

        // Check the futures resolved to cancellation errors
        for (auto& future : futures) {
            ASSERT_EQ(future.getNoThrow(), ErrorCodes::CallbackCanceled);
        }
    });
}

DEATH_TEST_F(IngressAsioNetworkingBatonTest, AddAnAlreadyAddedSession, "invariant") {
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto session = client().session();

    baton->addSession(*session, NetworkingBaton::Type::In).getAsync([](Status) {});
    baton->addSession(*session, NetworkingBaton::Type::In).getAsync([](Status) {});
}

class EgressSessionWithScopedReactor {
public:
    EgressSessionWithScopedReactor(ServiceContext* sc)
        : _sc(sc),
          _reactor(sc->getTransportLayerManager()->getEgressLayer()->getReactor(
              TransportLayer::kNewReactor)),
          _reactorThread([&] { _reactor->run(); }),
          _session(_makeEgressSession()) {}

    ~EgressSessionWithScopedReactor() {
        _reactor->stop();
        _reactorThread.join();
    }

    std::shared_ptr<Session>& session() {
        return _session;
    }

private:
    std::shared_ptr<Session> _makeEgressSession() {
        auto tla =
            checked_cast<AsioTransportLayer*>(_sc->getTransportLayerManager()->getEgressLayer());

        HostAndPort localTarget(testHostName(), tla->listenerPort());

        return _sc->getTransportLayerManager()
            ->getEgressLayer()
            ->asyncConnect(localTarget,
                           ConnectSSLMode::kGlobalSSLMode,
                           _reactor,
                           Milliseconds{500},
                           std::make_shared<ConnectionMetrics>(_sc->getFastClockSource()),
                           nullptr)
            .get();
    }

    ServiceContext* _sc;
    ReactorHandle _reactor;
    stdx::thread _reactorThread;
    std::shared_ptr<Session> _session;
};

// This could be considered a test for either `AsioSession` or `AsioNetworkingBaton`, as it's
// testing the interaction between the two when `AsioSession` calls `addSession` and
// `cancelAsyncOperations` on the networking baton. This is currently added to the
// `EgressAsioNetworkingBatonTest` fixture to utilize the existing infrastructure.
TEST_F(EgressAsioNetworkingBatonTest, CancelAsyncOperationsInterruptsOngoingOperations) {
    EgressSessionWithScopedReactor es(getGlobalServiceContext());

    MilestoneThread thread([&](Notification<void>& isReady) {
        // Blocks the main thread as it schedules an opportunistic read, but before it starts
        // polling on the networking baton. Then it cancels the operation before unblocking the main
        // thread.
        FailPointEnableBlock fp("asioTransportLayerBlockBeforeOpportunisticRead");
        isReady.set();
        waitForTimesEntered(fp, 1);
        es.session()->cancelAsyncOperations();
    });

    auto opCtx = client().makeOperationContext();
    ASSERT_THROWS_CODE(es.session()->asyncSourceMessage(opCtx->getBaton()).get(),
                       DBException,
                       ErrorCodes::CallbackCanceled);
}

TEST_F(EgressAsioNetworkingBatonTest, AsyncOpsMakeProgressWhenSessionAddedToDetachedBaton) {
    EgressSessionWithScopedReactor es(getGlobalServiceContext());

    Notification<void> ready;
    auto opCtx = client().makeOperationContext();

    test::JoinThread thread([&] {
        auto baton = opCtx->getBaton();
        ready.get();
        es.session()->asyncSourceMessage(baton).ignoreValue().getAsync(
            [session = es.session()](Status) {
                // Capturing `session` is necessary as parts of this continuation run at fixture
                // destruction.
            });
    });

    FailPointEnableBlock fp("asioTransportLayerBlockBeforeAddSession");
    ready.set();
    waitForTimesEntered(fp, 1);

    // Destroying the `opCtx` results in detaching the baton. At this point, the thread running
    // `asyncSourceMessage` has acquired the mutex that orders asynchronous operations (i.e.,
    // `asyncOpMutex`) and is blocked by `fp`. Once we return from this function, that thread is
    // unblocked and will run `Baton::addSession` on a detached baton.
    opCtx.reset();
}

#endif  // __linux__

}  // namespace
}  // namespace mongo::transport
