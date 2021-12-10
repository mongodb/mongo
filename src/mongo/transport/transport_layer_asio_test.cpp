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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/transport/transport_layer_asio.h"

#include <queue>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <fmt/format.h>

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session_asio.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using namespace fmt::literals;

#ifdef _WIN32
using SetsockoptPtr = char*;
#else
using SetsockoptPtr = void*;
#endif

class JoinThread : public stdx::thread {
public:
    using stdx::thread::thread;
    ~JoinThread() {
        if (joinable())
            join();
    }
};

template <typename T>
class BlockingQueue {
public:
    void push(T t) {
        stdx::unique_lock lk(_mu);
        _q.push(std::move(t));
        lk.unlock();
        _cv.notify_one();
    }

    T pop() {
        stdx::unique_lock lk(_mu);
        _cv.wait(lk, [&] { return !_q.empty(); });
        T r = std::move(_q.front());
        _q.pop();
        return r;
    }

private:
    mutable Mutex _mu;
    mutable stdx::condition_variable _cv;
    std::queue<T> _q;
};

class ConnectionThread {
public:
    explicit ConnectionThread(int port) : ConnectionThread(port, nullptr) {}
    ConnectionThread(int port, std::function<void(ConnectionThread&)> onConnect)
        : _port{port}, _onConnect{std::move(onConnect)}, _thread{[this] { _run(); }} {}

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

private:
    void _run() {
        _s.connect(SockAddr::create("localhost", _port, AF_INET));
        LOGV2(6109502, "connected", "port"_attr = _port);
        if (_onConnect)
            _onConnect(*this);
        _stopRequest.get();
        LOGV2(6109503, "connection: Rx stop request");
    }

    int _port;
    std::function<void(ConnectionThread&)> _onConnect;
    Notification<bool> _stopRequest;
    Socket _s;
    JoinThread _thread;  // Appears after the members _run uses.
};

class SyncClient {
public:
    explicit SyncClient(int port) {
        std::error_code ec;
        _sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
        ASSERT_EQ(ec, std::error_code());
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

void ping(SyncClient& client) {
    OpMsgBuilder builder;
    builder.setBody(BSON("ping" << 1));
    Message msg = builder.finish();
    msg.header().setResponseToMsgId(0);
    msg.header().setId(0);
    OpMsg::appendChecksum(&msg);
    ASSERT_EQ(client.write(msg.buf(), msg.size()), std::error_code{});
}

struct SessionThread {
    struct StopException {};

    explicit SessionThread(std::shared_ptr<transport::Session> s)
        : _session{std::move(s)}, _thread{[this] { _run(); }} {}

    ~SessionThread() {
        if (!_thread.joinable())
            return;
        schedule([](auto&&) { throw StopException{}; });
    }

    void schedule(std::function<void(transport::Session&)> task) {
        _tasks.push(std::move(task));
    }

    transport::Session& session() const {
        return *_session;
    }

private:
    void _run() {
        while (true) {
            try {
                LOGV2(6109508, "SessionThread: pop and execute a task");
                _tasks.pop()(*_session);
            } catch (const StopException&) {
                LOGV2(6109509, "SessionThread: stopping");
                return;
            }
        }
    }

    std::shared_ptr<transport::Session> _session;
    BlockingQueue<std::function<void(transport::Session&)>> _tasks;
    JoinThread _thread;  // Appears after the members _run uses.
};

class MockSEP : public ServiceEntryPoint {
public:
    MockSEP() = default;
    explicit MockSEP(std::function<void(SessionThread&)> onStartSession)
        : _onStartSession(std::move(onStartSession)) {}

    ~MockSEP() override {
        _join();
    }

    Status start() override {
        return Status::OK();
    }

    void appendStats(BSONObjBuilder*) const override {}

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) noexcept override {
        MONGO_UNREACHABLE;
    }

    void startSession(std::shared_ptr<transport::Session> session) override {
        LOGV2(6109510, "Accepted connection", "remote"_attr = session->remote());
        auto& newSession = [&]() -> SessionThread& {
            auto vec = *_sessions;
            vec->push_back(std::make_unique<SessionThread>(std::move(session)));
            return *vec->back();
        }();
        if (_onStartSession)
            _onStartSession(newSession);
        LOGV2(6109511, "started session");
    }

    void endAllSessions(transport::Session::TagMask tags) override {
        _join();
    }

    bool shutdown(Milliseconds timeout) override {
        _join();
        return true;
    }

    size_t numOpenSessions() const override {
        return _sessions->size();
    }

    void setOnStartSession(std::function<void(SessionThread&)> cb) {
        _onStartSession = std::move(cb);
    }

private:
    void _join() {
        LOGV2(6109513, "Joining all session threads");
        _sessions->clear();
    }

    std::function<void(SessionThread&)> _onStartSession;
    synchronized_value<std::vector<std::unique_ptr<SessionThread>>> _sessions;
};

/**
 * Properly setting up and tearing down the MockSEP and TransportLayerASIO is
 * tricky. Most tests can delegate the details to this TestFixture.
 */
class TestFixture {
public:
    TestFixture() : _tla{_makeTLA()} {}

    ~TestFixture() {
        _sep.endAllSessions({});
        _tla->shutdown();
    }

    MockSEP& sep() {
        return _sep;
    }

    transport::TransportLayerASIO& tla() {
        return *_tla;
    }

private:
    std::unique_ptr<transport::TransportLayerASIO> _makeTLA() {
        auto options = [] {
            ServerGlobalParams params;
            params.noUnixSocket = true;
            transport::TransportLayerASIO::Options opts(&params);
            // TODO SERVER-30212 should clean this up and assign a port from the supplied port range
            // provided by resmoke.
            opts.port = 0;
            return opts;
        }();
        auto tla = std::make_unique<transport::TransportLayerASIO>(options, &_sep);
        ASSERT_OK(tla->setup());
        ASSERT_OK(tla->start());
        return tla;
    }

    MockSEP _sep;
    std::unique_ptr<transport::TransportLayerASIO> _tla;
};

TEST(TransportLayerASIO, ListenerPortZeroTreatedAsEphemeral) {
    Notification<bool> connected;
    TestFixture tf;
    tf.sep().setOnStartSession([&](auto&&) { connected.set(true); });

    int port = tf.tla().listenerPort();
    ASSERT_GT(port, 0);
    LOGV2(6109514, "TransportLayerASIO listening", "port"_attr = port);

    ConnectionThread connectThread(port);
    connected.get();
}

/**
 * Test that the server appropriately handles a client-side socket disconnection, and that the
 * client sends an RST packet when the socket is forcibly closed.
 */
TEST(TransportLayerASIO, TCPResetAfterConnectionIsSilentlySwallowed) {
    TestFixture tf;
    unittest::Barrier barrier(2);

    AtomicWord<int> sessionsCreated{0};
    tf.sep().setOnStartSession([&](auto&&) { sessionsCreated.fetchAndAdd(1); });

    LOGV2(6109515, "connecting");
    auto& fp = transport::transportLayerASIOhangBeforeAccept;
    auto timesEntered = fp.setMode(FailPoint::alwaysOn);
    ConnectionThread connectThread(tf.tla().listenerPort(), [&](ConnectionThread& conn) {
        // Linger timeout = 0 causes a RST packet on close.
        struct linger sl = {1, 0};
        if (setsockopt(conn.socket().rawFD(),
                       SOL_SOCKET,
                       SO_LINGER,
                       reinterpret_cast<SetsockoptPtr>(&sl),
                       sizeof(sl)) != 0) {
            auto err = make_error_code(std::errc{errno});
            LOGV2_ERROR(6109517, "setsockopt", "error"_attr = err.message());
        }
        barrier.countDownAndWait();
    });
    fp.waitForTimesEntered(timesEntered + 1);
    // Test case thread does not close socket until client thread has set options to ensure that an
    // RST packet will be set on close.
    barrier.countDownAndWait();

    LOGV2(6109516, "closing");
    connectThread.close();
    fp.setMode(FailPoint::off);

    ASSERT_EQ(sessionsCreated.load(), 0);
}

/* check that timeouts actually time out */
TEST(TransportLayerASIO, SourceSyncTimeoutTimesOut) {
    TestFixture tf;
    Notification<StatusWith<Message>> received;
    tf.sep().setOnStartSession([&](SessionThread& st) {
        st.session().setTimeout(Milliseconds{500});
        st.schedule([&](auto& session) { received.set(session.sourceMessage()); });
    });
    SyncClient conn(tf.tla().listenerPort());
    ASSERT_EQ(received.get().getStatus(), ErrorCodes::NetworkTimeout);
}

/* check that timeouts don't time out unless there's an actual timeout */
TEST(TransportLayerASIO, SourceSyncTimeoutSucceeds) {
    TestFixture tf;
    Notification<StatusWith<Message>> received;
    tf.sep().setOnStartSession([&](SessionThread& st) {
        st.session().setTimeout(Milliseconds{500});
        st.schedule([&](auto& session) { received.set(session.sourceMessage()); });
    });
    SyncClient conn(tf.tla().listenerPort());
    ping(conn);  // This time we send a message
    ASSERT_OK(received.get().getStatus());
}

/** Switching from timeouts to no timeouts must reset the timeout to unlimited. */
TEST(TransportLayerASIO, SwitchTimeoutModes) {
    TestFixture tf;
    Notification<SessionThread*> mockSessionCreated;
    tf.sep().setOnStartSession([&](SessionThread& st) { mockSessionCreated.set(&st); });

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
        asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::loopback(), 0);
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
                LOGV2(6101605, "Accept", "error"_attr = transport::errorCodeToStatus(ec));
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
 * Have `TransportLayerASIO` make a egress connection and observe behavior when
 * that connection is immediately reset by the peer. Test that if this happens
 * during the `ASIOSession` constructor, that the thrown `asio::system_error`
 * is handled safely (translated to a Status holding a SocketException).
 */
TEST(TransportLayerASIO, EgressConnectionResetByPeerDuringSessionCtor) {
    // Under TFO, no SYN is sent until the client has data to send.  For this
    // test, we need the server to respond when the client hits the failpoint
    // in the ASIOSession ctor. So we have to disable TFO.
    auto savedTFOClient = std::exchange(transport::gTCPFastOpenClient, false);
    auto savedTFOClientRestore = makeGuard([&] { transport::gTCPFastOpenClient = savedTFOClient; });
    // The `server` accepts connections, only to immediately reset them.
    TestFixture tf;
    asio::io_context ioContext;

    // `fp` pauses the `ASIOSession` constructor immediately prior to its
    // `setsockopt` sequence, to allow time for the peer reset to propagate.
    FailPoint& fp = transport::transportLayerASIOSessionPauseBeforeSetSocketOption;

    Acceptor server(ioContext);
    server.setOnAccept([&](std::shared_ptr<Acceptor::Connection> conn) {
        LOGV2(6101604, "handling a connection by resetting it");
        conn->socket.set_option(asio::socket_base::linger(true, 0));
        conn->socket.close();
        sleepFor(Seconds{1});
        fp.setMode(FailPoint::off);
    });
    JoinThread ioThread{[&] { ioContext.run(); }};
    auto ioContextStop = makeGuard([&] { ioContext.stop(); });

    fp.setMode(FailPoint::alwaysOn);
    LOGV2(6101602, "Connecting", "port"_attr = server.port());
    using namespace unittest::match;
    // On MacOS, calling `setsockopt` on a peer-reset connection yields an
    // `EINVAL`. On Linux and Windows, the `setsockopt` completes successfully.
    // Either is okay, but the `ASIOSession` ctor caller is expected to handle
    // `asio::system_error` and convert it to `SocketException`.
    ASSERT_THAT(tf.tla()
                    .connect({"localhost", server.port()},
                             transport::ConnectSSLMode::kDisableSSL,
                             Seconds{10},
                             {})
                    .getStatus(),
                StatusIs(AnyOf(Eq(ErrorCodes::SocketException), Eq(ErrorCodes::OK)), Any()));
}

/**
 * With no regard to mongo code, just check what the ASIO socket
 * implementation does in the reset connection scenario.
 */
TEST(TransportLayerASIO, ConfirmSocketSetOptionOnResetConnections) {
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
    JoinThread ioThread{[&] { ioContext.run(); }};
    auto ioContextStop = makeGuard([&] { ioContext.stop(); });
    JoinThread client{[&] {
        asio::ip::tcp::socket client{ioContext};
        client.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server.port()));
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

}  // namespace
}  // namespace mongo
