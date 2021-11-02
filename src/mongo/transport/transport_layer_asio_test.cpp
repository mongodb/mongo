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
#include <utility>
#include <vector>

#include <asio.hpp>

#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

#ifdef _WIN32
using SetsockoptPtr = char*;
#else
using SetsockoptPtr = void*;
#endif

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
    explicit ConnectionThread(int port) : _port{port}, _thr{[this] { run(); }} {}

    ~ConnectionThread() {
        if (!_stop)
            stop();
    }

    void close() {
        _s.close();
    }

    void stop() {
        LOGV2(6109500, "connection: Tx stop request");
        _stop.set(true);
        _thr.join();
        LOGV2(6109501, "connection: joined");
    }

protected:
    Socket& socket() {
        return _s;
    }

private:
    virtual void onConnect() {}

    void run() {
        _s.connect(SockAddr::create("localhost", _port, AF_INET));
        LOGV2(6109502, "connection: port {port}", "port"_attr = _port);
        onConnect();
        _stop.get();
        LOGV2(6109503, "connection: Rx stop request");
    }

    int _port;
    stdx::thread _thr;
    Socket _s;
    Notification<bool> _stop;
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
    ASSERT_FALSE(client.write(msg.buf(), msg.size()));
}

class MockSEP : public ServiceEntryPoint {
public:
    struct StopException {};

    struct Session {
        using Task = std::function<void(Session&)>;

        ~Session() {
            stop();
        }

        void schedule(Task task) {
            LOGV2(6109505, "scheduling task");
            tasks.push(std::move(task));
        }

        void start() {
            thread = stdx::thread([this] { run(); });
        }

        void stop() {
            if (thread.joinable()) {
                schedule([](auto&&) { throw StopException{}; });
                thread.join();
            }
        }

        void run() {
            LOGV2(6109506, "doSession");
            while (true) {
                LOGV2(6109507, "polling for work");
                try {
                    LOGV2(6109508, "running a session task");
                    tasks.pop()(*this);
                } catch (const StopException&) {
                    LOGV2(6109509, "caught StopException");
                    return;
                }
            }
        }

        std::shared_ptr<transport::Session> session;
        stdx::thread thread;
        BlockingQueue<Task> tasks;
    };
    using Task = Session::Task;

    MockSEP() = default;
    explicit MockSEP(std::function<void(Session&)> onStartSession)
        : _onStartSession(std::move(onStartSession)) {}

    ~MockSEP() override {
        for (auto& s : **_sessions)
            s->stop();
        // This should shutdown immediately, so give the maximum timeout
        shutdown(Milliseconds::max());
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
        Session& newSession = *_sessions->emplace_back(new Session{std::move(session)});
        if (_onStartSession)
            _onStartSession(newSession);
        LOGV2(6109511, "started session");
    }

    void endAllSessions(transport::Session::TagMask tags) override {
        LOGV2(6109512, "end all sessions");
        _sessions->clear();
    }

    bool shutdown(Milliseconds timeout) override {
        LOGV2(6109513, "Joining all worker threads");
        std::exchange(**_sessions, {});
        return true;
    }

    size_t numOpenSessions() const override {
        return _sessions->size();
    }

    void mockOnStartSession(std::function<void(Session&)> cb) {
        _onStartSession = std::move(cb);
    }

private:
    std::function<void(Session&)> _onStartSession;
    synchronized_value<std::vector<std::unique_ptr<Session>>> _sessions;
};

std::unique_ptr<transport::TransportLayerASIO> makeAndStartTL(ServiceEntryPoint* sep) {
    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::TransportLayerASIO::Options opts(&params);
        // TODO SERVER-30212 should clean this up and assign a port from the supplied port range
        // provided by resmoke.
        opts.port = 0;
        return opts;
    }();
    auto tla = std::make_unique<transport::TransportLayerASIO>(options, sep);
    ASSERT_OK(tla->setup());
    ASSERT_OK(tla->start());
    return tla;
}

TEST(TransportLayerASIO, ListenerPortZeroTreatedAsEphemeral) {
    Notification<bool> connected;
    MockSEP sep;
    sep.mockOnStartSession([&](auto&&) { connected.set(true); });
    auto tla = makeAndStartTL(&sep);

    int port = tla->listenerPort();
    ASSERT_GT(port, 0);
    LOGV2(6109514, "TransportLayerASIO listening", "port"_attr = port);

    ConnectionThread connectThread(port);
    connected.get();

    ASSERT_EQ(sep.numOpenSessions(), 1);
    connectThread.stop();
    tla->shutdown();
}

TEST(TransportLayerASIO, TCPResetAfterConnectionIsSilentlySwallowed) {
    class ResettingConnectionThread : public ConnectionThread {
        using ConnectionThread::ConnectionThread;
        void onConnect() override {
            // Linger timeout = 0 causes a RST packet on close.
            struct linger sl = {1, 0};
            ASSERT(!setsockopt(socket().rawFD(),
                               SOL_SOCKET,
                               SO_LINGER,
                               reinterpret_cast<SetsockoptPtr>(&sl),
                               sizeof(sl)));
        }
    };

    MockSEP sep;
    auto tla = makeAndStartTL(&sep);

    auto& fp = transport::transportLayerASIOhangBeforeAccept;
    auto timesEntered = fp.setMode(FailPoint::alwaysOn);

    LOGV2(6109515, "connecting");

    ResettingConnectionThread connectThread(tla->listenerPort());
    fp.waitForTimesEntered(timesEntered + 1);

    LOGV2(6109516, "closing");
    connectThread.close();
    fp.setMode(FailPoint::off);

    LOGV2(6109517, "asserting");
    ASSERT_EQ(sep.numOpenSessions(), 0);
    LOGV2(6109518, "past assert");
    connectThread.stop();
    tla->shutdown();
}

/* check that timeouts actually time out */
TEST(TransportLayerASIO, SourceSyncTimeoutTimesOut) {
    Notification<StatusWith<Message>> received;
    MockSEP sep;
    sep.mockOnStartSession([&](MockSEP::Session& session) {
        LOGV2(6109519, "setting timeout");
        session.session->setTimeout(Milliseconds{500});
        session.start();
        LOGV2(6109520, "waiting for message");
        session.schedule([&](MockSEP::Session& session) {
            received.set(session.session->sourceMessage());
            LOGV2(6109521, "message receive op resolved");
        });
    });
    auto tla = makeAndStartTL(&sep);
    SyncClient conn(tla->listenerPort());

    LOGV2(6109522, "scheduled");

    ASSERT_EQ(received.get().getStatus(), ErrorCodes::NetworkTimeout);
    LOGV2(6109523, "received something");
    tla->shutdown();
}

/* check that timeouts don't time out unless there's an actual timeout */
TEST(TransportLayerASIO, SourceSyncTimeoutSucceeds) {
    MockSEP sep;
    Notification<StatusWith<Message>> received;
    sep.mockOnStartSession([&](MockSEP::Session& s) {
        s.session->setTimeout(Milliseconds{500});
        s.start();
        s.schedule([&](auto&) { received.set(s.session->sourceMessage()); });
    });
    auto tla = makeAndStartTL(&sep);
    SyncClient conn(tla->listenerPort());

    ping(conn);  // This time we send a message
    ASSERT_OK(received.get().getStatus());
    LOGV2(6109524, "received something");
    tla->shutdown();
}

/** Switching from timeouts to no timeouts must reset the timeout to unlimited. */
TEST(TransportLayerASIO, SwitchTimeoutModes) {
    MockSEP sep;
    Notification<MockSEP::Session*> mockSessionCreated;
    sep.mockOnStartSession([&](MockSEP::Session& s) {
        s.start();
        mockSessionCreated.set(&s);
    });
    auto tla = makeAndStartTL(&sep);

    SyncClient conn(tla->listenerPort());

    auto& session = *mockSessionCreated.get();

    {
        LOGV2(6109525, "The first message we source should time out");
        Notification<StatusWith<Message>> done;
        session.schedule([&](const auto&) {
            session.session->setTimeout(Milliseconds{500});
            done.set(session.session->sourceMessage());
        });
        ASSERT_EQ(done.get().getStatus(), ErrorCodes::NetworkTimeout);
        LOGV2(6109526, "timed out successfully");
    }
    {
        LOGV2(6109527, "Verify a message can be successfully received");
        Notification<StatusWith<Message>> done;
        session.schedule([&](const auto&) { done.set(session.session->sourceMessage()); });
        ping(conn);
        ASSERT_OK(done.get().getStatus());
    }
    {
        LOGV2(6109528, "Clear the timeout and verify reception of a late message.");
        Notification<StatusWith<Message>> done;
        session.schedule([&](const auto&) {
            LOGV2(6109529, "waiting for message without a timeout");
            session.session->setTimeout({});
            done.set(session.session->sourceMessage());
        });
        sleepFor(Seconds{1});
        ping(conn);
        ASSERT_OK(done.get().getStatus());
    }
    tla->shutdown();
}

}  // namespace
}  // namespace mongo
