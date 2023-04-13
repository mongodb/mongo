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

#include <fstream>
#include <queue>
#include <system_error>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <fmt/format.h>

#include "mongo/client/dbclient_connection.h"
#include "mongo/config.h"
#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/death_test.h"
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


namespace mongo {
namespace {

using namespace fmt::literals;

#ifdef _WIN32
using SetsockoptPtr = char*;
#else
using SetsockoptPtr = void*;
#endif

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
        _ready.get();
    }

private:
    void _run() {
        _s.connect(SockAddr::create("localhost", _port, AF_INET));
        LOGV2(6109502, "connected", "port"_attr = _port);
        if (_onConnect)
            _onConnect(*this);
        _ready.set();
        _stopRequest.get();
        LOGV2(6109503, "connection: Rx stop request");
    }

    int _port;
    std::function<void(ConnectionThread&)> _onConnect;
    Notification<void> _ready;
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
        : _session{std::move(s)}, _thread{[this] {
              _run();
          }} {}

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

    logv2::LogSeverity slowSessionWorkflowLogSeverity() override {
        MONGO_UNIMPLEMENTED;
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

std::unique_ptr<transport::AsioTransportLayer> makeTLA(ServiceEntryPoint* sep) {
    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::AsioTransportLayer::Options opts(&params);
        // TODO SERVER-30312 should clean this up and assign a port from the supplied port range
        // provided by resmoke.
        opts.port = 0;
        return opts;
    }();
    auto tla = std::make_unique<transport::AsioTransportLayer>(options, sep);
    ASSERT_OK(tla->setup());
    ASSERT_OK(tla->start());
    return tla;
}

/**
 * Properly setting up and tearing down the MockSEP and AsioTransportLayer is
 * tricky. Most tests can delegate the details to this TestFixture.
 */
class TestFixture {
public:
    TestFixture() : _tla{makeTLA(&_sep)} {}

    ~TestFixture() {
        _sep.endAllSessions({});
        _tla->shutdown();
    }

    MockSEP& sep() {
        return _sep;
    }

    transport::AsioTransportLayer& tla() {
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
    RAIIServerParameterControllerForTest _featureFlagController{"featureFlagConnHealthMetrics",
                                                                true};
    std::unique_ptr<transport::AsioTransportLayer> _tla;
    MockSEP _sep;

    FailPoint& _hangBeforeAccept = transport::asioTransportLayerHangBeforeAcceptCallback;
    FailPoint& _hangDuringAccept = transport::asioTransportLayerHangDuringAcceptCallback;

    FailPoint::EntryCountT _hangBeforeAcceptTimesEntered{0};
    FailPoint::EntryCountT _hangDuringAcceptTimesEntered{0};
};

TEST(AsioTransportLayer, ListenerPortZeroTreatedAsEphemeral) {
    Notification<bool> connected;
    TestFixture tf;
    tf.sep().setOnStartSession([&](auto&&) { connected.set(true); });

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
    tf.sep().setOnStartSession([&](auto&&) { sessionsCreated.fetchAndAdd(1); });

    LOGV2(6109515, "connecting");
    auto& fp = transport::asioTransportLayerHangDuringAcceptCallback;
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

#ifdef __linux__
/**
 * Test that the server successfully captures the TCP socket queue depth, and places the value both
 * into the AsioTransportLayer class and FTDC output.
 */
TEST(AsioTransportLayer, TCPCheckQueueDepth) {
    // Set the listenBacklog to a parameter greater than the number of connection threads we intend
    // to create.
    serverGlobalParams.listenBacklog = 10;
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
    Notification<SessionThread*> mockSessionCreated;
    tf.sep().setOnStartSession([&](SessionThread& st) { mockSessionCreated.set(&st); });

    ConnectionThread connectThread(tf.tla().listenerPort(), &setNoLinger);

    // We set the timeout to ensure that the setsockopt calls are actually made in ensureSync()
    auto& st = *mockSessionCreated.get();
    st.session().setTimeout(Milliseconds{500});

    // Synchronize with the connection thread to ensure the connection is closed only after the
    // connection thread returns from calling `setsockopt`.
    connectThread.wait();
    connectThread.close();

    // On Mac, setsockopt will immediately throw a SocketException since the socket is closed.
    // On Linux, we will throw HostUnreachable once we try to actually read the socket.
    // We allow for either exception here.
    using namespace unittest::match;
    ASSERT_THAT(
        st.session().sourceMessage().getStatus(),
        StatusIs(AnyOf(Eq(ErrorCodes::HostUnreachable), Eq(ErrorCodes::SocketException)), Any()));
}

/* check that timeouts actually time out */
TEST(AsioTransportLayer, SourceSyncTimeoutTimesOut) {
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
TEST(AsioTransportLayer, SourceSyncTimeoutSucceeds) {
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
TEST(AsioTransportLayer, SwitchTimeoutModes) {
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
 * Have `AsioTransportLayer` make a egress connection and observe behavior when
 * that connection is immediately reset by the peer. Test that if this happens
 * during the `AsioSession` constructor, that the thrown `asio::system_error`
 * is handled safely (translated to a Status holding a SocketException).
 */
TEST(AsioTransportLayer, EgressConnectionResetByPeerDuringSessionCtor) {
    // Under TFO, no SYN is sent until the client has data to send.  For this
    // test, we need the server to respond when the client hits the failpoint
    // in the AsioSession ctor. So we have to disable TFO.
    auto savedTFOClient = std::exchange(transport::gTCPFastOpenClient, false);
    ScopeGuard savedTFOClientRestore = [&] {
        transport::gTCPFastOpenClient = savedTFOClient;
    };
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
    JoinThread ioThread{[&] {
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
    JoinThread ioThread{[&] {
        ioContext.run();
    }};
    ScopeGuard ioContextStop = [&] {
        ioContext.stop();
    };
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
        auto sep = std::make_unique<MockSEP>();
        auto tl = makeTLA(sep.get());
        getServiceContext()->setServiceEntryPoint(std::move(sep));
        getServiceContext()->setTransportLayer(std::move(tl));
    }

    void tearDown() override {
        getServiceContext()->getTransportLayer()->shutdown();
    }

    transport::AsioTransportLayer& tla() {
        auto tl = getServiceContext()->getTransportLayer();
        return *checked_cast<transport::AsioTransportLayer*>(tl);
    }
};

TEST_F(AsioTransportLayerWithServiceContextTest, TimerServiceDoesNotSpawnThreadsBeforeStart) {
    ThreadCounter counter;
    { transport::AsioTransportLayer::TimerService service{{counter.makeSpawnFunc()}}; }
    ASSERT_EQ(counter.created(), 0);
}

TEST_F(AsioTransportLayerWithServiceContextTest, TimerServiceOneShotStart) {
    ThreadCounter counter;
    transport::AsioTransportLayer::TimerService service{{counter.makeSpawnFunc()}};
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
    transport::AsioTransportLayer::TimerService service{{counter.makeSpawnFunc()}};
    service.stop();
    service.start();
    ASSERT_EQ(counter.created(), 0) << "Stop then start should not spawn";
}

TEST_F(AsioTransportLayerWithServiceContextTest, TimerServiceCanStopMoreThanOnce) {
    // Verifying that it is safe to have multiple calls to `stop()`.
    {
        transport::AsioTransportLayer::TimerService service;
        service.start();
        service.stop();
        service.stop();
    }
    {
        transport::AsioTransportLayer::TimerService service;
        service.stop();
        service.stop();
    }
}

TEST_F(AsioTransportLayerWithServiceContextTest, TransportStartAfterShutDown) {
    tla().shutdown();
    ASSERT_EQ(tla().start(), transport::TransportLayer::ShutdownStatus);
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

    auto status = conn.connectSocketOnly({"localhost", port}, std::move(params));
    ASSERT_EQ(status, ErrorCodes::HostUnreachable);
}
#endif  // _WIN32
#endif  // MONGO_CONFIG_SSL

#ifdef __linux__

/**
 * Helper constants to be used as template parameters for AsioNetworkingBatonTest. Indicates
 * whether the test is allowed to accept connections after establishing the first Session associated
 * with `_client`.
 */
enum class AllowMultipleSessions : bool {};

/**
 * Creates a connection between a client and a server, then runs tests against the
 * `AsioNetworkingBaton` associated with the server-side of the connection (i.e., `Client`). The
 * client-side of this connection is associated with `_connThread`, and the server-side is wrapped
 * inside `_client`.
 *
 * Instantiated with `shouldAllowMultipleSessions` which indicates whether the listener thread for
 * this test's transport layer should accept more than the connection initially established by
 * `_client`.
 */
template <AllowMultipleSessions shouldAllowMultipleSessions>
class AsioNetworkingBatonTest : public LockerNoopServiceContextTest {
    // A service entry point that emplaces a Future with the ingress session corresponding to the
    // first connection.
    class FirstSessionSEP : public ServiceEntryPoint {
    public:
        explicit FirstSessionSEP(Promise<std::shared_ptr<transport::Session>> promise)
            : _promise(std::move(promise)) {}

        Status start() override {
            return Status::OK();
        }

        void appendStats(BSONObjBuilder*) const override {}

        Future<DbResponse> handleRequest(OperationContext*, const Message&) noexcept override {
            MONGO_UNREACHABLE;
        }

        void startSession(std::shared_ptr<transport::Session> session) override {
            if (!_isEmplaced) {
                _promise.emplaceValue(std::move(session));
                _isEmplaced = true;
                return;
            }
            invariant(static_cast<bool>(shouldAllowMultipleSessions),
                      "Created a second ingress Session when only one was expected.");
        }

        void endAllSessions(transport::Session::TagMask) override {}

        bool shutdown(Milliseconds) override {
            return true;
        }

        size_t numOpenSessions() const override {
            MONGO_UNREACHABLE;
        }

        logv2::LogSeverity slowSessionWorkflowLogSeverity() override {
            MONGO_UNIMPLEMENTED;
        }

    private:
        Promise<std::shared_ptr<transport::Session>> _promise;
        bool _isEmplaced = false;
    };

    // Used for setting and canceling timers on the networking baton. Does not offer any timer
    // functionality, and is only used for its unique id.
    class DummyTimer final : public transport::ReactorTimer {
    public:
        void cancel(const BatonHandle& baton = nullptr) override {
            MONGO_UNREACHABLE;
        }

        Future<void> waitUntil(Date_t timeout, const BatonHandle& baton = nullptr) override {
            MONGO_UNREACHABLE;
        }
    };

public:
    void setUp() override {
        auto pf = makePromiseFuture<std::shared_ptr<transport::Session>>();
        auto servCtx = getServiceContext();
        servCtx->setServiceEntryPoint(std::make_unique<FirstSessionSEP>(std::move(pf.promise)));

        auto tla = makeTLA(servCtx->getServiceEntryPoint());
        const auto listenerPort = tla->listenerPort();
        servCtx->setTransportLayer(std::move(tla));

        _connThread = std::make_unique<ConnectionThread>(listenerPort);
        _client = servCtx->makeClient("NetworkBatonTest", pf.future.get());
    }

    void tearDown() override {
        _connThread.reset();
        getServiceContext()->getTransportLayer()->shutdown();
    }

    Client& client() {
        return *_client;
    }

    ConnectionThread& connection() {
        return *_connThread;
    }

    std::unique_ptr<transport::ReactorTimer> makeDummyTimer() const {
        return std::make_unique<DummyTimer>();
    }

private:
    ServiceContext::UniqueClient _client;
    std::unique_ptr<ConnectionThread> _connThread;
};

class IngressAsioNetworkingBatonTest
    : public AsioNetworkingBatonTest<AllowMultipleSessions{false}> {};

class EgressAsioNetworkingBatonTest : public AsioNetworkingBatonTest<AllowMultipleSessions{true}> {
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
    JoinThread _thread;
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
    auto future = baton->addSession(*session, transport::NetworkingBaton::Type::In);
    ASSERT_TRUE(baton->cancelSession(*session));
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(IngressAsioNetworkingBatonTest, AddAndRemoveSessionWhileInPoll) {
    // Attempts to add and remove a session while the baton is polling. This, for example, could
    // happen on `mongos` while an operation is blocked, waiting for `AsyncDBClient` to create an
    // egress connection, and then the connection has to be ended for some reason before the baton
    // returns from polling.
    auto opCtx = client().makeOperationContext();
    Notification<bool> cancelSessionResult;

    MilestoneThread thread([&](Notification<void>& isReady) {
        auto baton = opCtx->getBaton()->networking();
        auto session = client().session();

        FailPointEnableBlock fp("blockAsioNetworkingBatonBeforePoll");
        isReady.set();
        waitForTimesEntered(fp, 1);

        // This thread is an external observer to the baton, so the expected behavior is for
        // `cancelSession` to happen after `addSession`, and thus it must return `true`.
        baton->addSession(*session, transport::NetworkingBaton::Type::In).getAsync([](Status) {});
        cancelSessionResult.set(baton->cancelSession(*session));
    });

    // TODO SERVER-64174 Change the following to `ASSERT_TRUE` once the underlying issue is fixed.
    ASSERT_FALSE(cancelSessionResult.get(opCtx.get()));
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

    // TODO SERVER-64174 Change the following to `ASSERT_TRUE` once the underlying issue is fixed.
    ASSERT_FALSE(cancelTimerResult.get(opCtx.get()));
}

DEATH_TEST_F(IngressAsioNetworkingBatonTest, AddAnAlreadyAddedSession, "invariant") {
    auto opCtx = client().makeOperationContext();
    auto baton = opCtx->getBaton()->networking();
    auto session = client().session();

    baton->addSession(*session, transport::NetworkingBaton::Type::In).getAsync([](Status) {});
    baton->addSession(*session, transport::NetworkingBaton::Type::In).getAsync([](Status) {});
}

class EgressSessionWithScopedReactor {
public:
    EgressSessionWithScopedReactor(ServiceContext* sc)
        : _sc(sc),
          _reactor(sc->getTransportLayer()->getReactor(transport::TransportLayer::kNewReactor)),
          _reactorThread([&] { _reactor->run(); }),
          _session(_makeEgressSession()) {}

    ~EgressSessionWithScopedReactor() {
        _reactor->stop();
        _reactorThread.join();
    }

    std::shared_ptr<transport::Session>& session() {
        return _session;
    }

private:
    std::shared_ptr<transport::Session> _makeEgressSession() {
        auto tla = checked_cast<transport::AsioTransportLayer*>(_sc->getTransportLayer());

        HostAndPort localTarget("localhost", tla->listenerPort());

        return _sc->getTransportLayer()
            ->asyncConnect(localTarget,
                           transport::ConnectSSLMode::kGlobalSSLMode,
                           _reactor,
                           Milliseconds{500},
                           std::make_shared<ConnectionMetrics>(_sc->getFastClockSource()),
                           nullptr)
            .get();
    }

    ServiceContext* _sc;
    transport::ReactorHandle _reactor;
    stdx::thread _reactorThread;
    std::shared_ptr<transport::Session> _session;
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

    JoinThread thread([&] {
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
}  // namespace mongo
