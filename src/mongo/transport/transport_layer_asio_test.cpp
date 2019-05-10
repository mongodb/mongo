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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_asio.h"

#include "mongo/db/server_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"

#include "asio.hpp"

namespace mongo {
namespace {

class ServiceEntryPointUtil : public ServiceEntryPoint {
public:
    void startSession(transport::SessionHandle session) override {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _sessions.push_back(std::move(session));
        log() << "started session";
        _cv.notify_one();
    }

    void endAllSessions(transport::Session::TagMask tags) override {
        log() << "end all sessions";
        std::vector<transport::SessionHandle> old_sessions;
        {
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            old_sessions.swap(_sessions);
        }
        old_sessions.clear();
    }

    Status start() override {
        return Status::OK();
    }

    bool shutdown(Milliseconds timeout) override {
        return true;
    }

    void appendStats(BSONObjBuilder*) const override {}

    size_t numOpenSessions() const override {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        return _sessions.size();
    }

    DbResponse handleRequest(OperationContext* opCtx, const Message& request) override {
        MONGO_UNREACHABLE;
    }

    void setTransportLayer(transport::TransportLayer* tl) {
        _transport = tl;
    }

    void waitForConnect() {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        _cv.wait(lock, [&] { return !_sessions.empty(); });
    }

private:
    mutable stdx::mutex _mutex;
    stdx::condition_variable _cv;
    std::vector<transport::SessionHandle> _sessions;
    transport::TransportLayer* _transport = nullptr;
};

class SimpleConnectionThread {
public:
    explicit SimpleConnectionThread(int port) : _port(port) {
        _thr = stdx::thread{[&] {
            Socket s;
            SockAddr sa{"localhost", _port, AF_INET};
            s.connect(sa);
            log() << "connection: port " << _port;
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            _cv.wait(lk, [&] { return _stop; });
            log() << "connection: Rx stop request";
        }};
    }

    void stop() {
        {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            _stop = true;
        }
        log() << "connection: Tx stop request";
        _cv.notify_one();
        _thr.join();
        log() << "connection: stopped";
    }

private:
    stdx::mutex _mutex;
    stdx::condition_variable _cv;
    stdx::thread _thr;
    bool _stop = false;
    int _port;
};

TEST(TransportLayerASIO, PortZeroConnect) {
    ServiceEntryPointUtil sepu;

    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::TransportLayerASIO::Options opts(&params);

        // TODO SERVER-30212 should clean this up and assign a port from the supplied port range
        // provided by resmoke.
        opts.port = 0;
        return opts;
    }();

    transport::TransportLayerASIO tla(options, &sepu);
    sepu.setTransportLayer(&tla);

    ASSERT_OK(tla.setup());
    ASSERT_OK(tla.start());
    int port = tla.listenerPort();
    ASSERT_GT(port, 0);
    log() << "TransportLayerASIO.listenerPort() is " << port;

    SimpleConnectionThread connect_thread(port);
    sepu.waitForConnect();
    connect_thread.stop();
    sepu.endAllSessions({});
    tla.shutdown();
}

class TimeoutSEP : public ServiceEntryPoint {
public:
    ~TimeoutSEP() override {
        // This should shutdown immediately, so give the maximum timeout
        shutdown(Milliseconds::max());
    }

    void endAllSessions(transport::Session::TagMask tags) override {
        MONGO_UNREACHABLE;
    }

    bool shutdown(Milliseconds timeout) override {
        log() << "Joining all worker threads";
        for (auto& thread : _workerThreads) {
            thread.join();
        }
        return true;
    }

    Status start() override {
        return Status::OK();
    }

    void appendStats(BSONObjBuilder*) const override {}

    size_t numOpenSessions() const override {
        return 0;
    }

    DbResponse handleRequest(OperationContext* opCtx, const Message& request) override {
        MONGO_UNREACHABLE;
    }

    bool waitForTimeout(boost::optional<Milliseconds> timeout = boost::none) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        bool ret = true;
        if (timeout) {
            ret = _cond.wait_for(lk, timeout->toSystemDuration(), [this] { return _finished; });
        } else {
            _cond.wait(lk, [this] { return _finished; });
        }

        _finished = false;
        return ret;
    }

protected:
    void notifyComplete() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _finished = true;
        _cond.notify_one();
    }

    template <typename FunT>
    void startWorkerThread(FunT&& fun) {
        _workerThreads.emplace_back(std::forward<FunT>(fun));
    }

private:
    stdx::mutex _mutex;

    stdx::condition_variable _cond;
    bool _finished = false;

    std::vector<stdx::thread> _workerThreads;
};

class TimeoutSyncSEP : public TimeoutSEP {
public:
    enum Mode { kShouldTimeout, kNoTimeout };
    TimeoutSyncSEP(Mode mode) : _mode(mode) {}

    void startSession(transport::SessionHandle session) override {
        log() << "Accepted connection from " << session->remote();
        startWorkerThread([ this, session = std::move(session) ]() mutable {
            log() << "waiting for message";
            session->setTimeout(Milliseconds{500});
            auto status = session->sourceMessage().getStatus();
            if (_mode == kShouldTimeout) {
                ASSERT_EQ(status, ErrorCodes::NetworkTimeout);
                log() << "message timed out";
            } else {
                ASSERT_OK(status);
                log() << "message received okay";
            }

            session.reset();
            notifyComplete();
        });
    }

private:
    Mode _mode;
};

class TimeoutConnector {
public:
    TimeoutConnector(int port, bool sendRequest)
        : _ctx(), _sock(_ctx), _endpoint(asio::ip::address_v4::loopback(), port) {
        std::error_code ec;
        _sock.connect(_endpoint, ec);
        ASSERT_EQ(ec, std::error_code());

        if (sendRequest) {
            sendMessage();
        }
    }

    void sendMessage() {
        OpMsgBuilder builder;
        builder.setBody(BSON("ping" << 1));
        Message msg = builder.finish();
        msg.header().setResponseToMsgId(0);
        msg.header().setId(0);
        OpMsg::appendChecksum(&msg);

        std::error_code ec;
        asio::write(_sock, asio::buffer(msg.buf(), msg.size()), ec);
        ASSERT_FALSE(ec);
    }

private:
    asio::io_context _ctx;
    asio::ip::tcp::socket _sock;
    asio::ip::tcp::endpoint _endpoint;
};

std::unique_ptr<transport::TransportLayerASIO> makeAndStartTL(ServiceEntryPoint* sep) {
    auto options = [] {
        ServerGlobalParams params;
        params.noUnixSocket = true;
        transport::TransportLayerASIO::Options opts(&params);
        opts.port = 0;
        return opts;
    }();

    auto tla = std::make_unique<transport::TransportLayerASIO>(options, sep);
    ASSERT_OK(tla->setup());
    ASSERT_OK(tla->start());

    return tla;
}

/* check that timeouts actually time out */
TEST(TransportLayerASIO, SourceSyncTimeoutTimesOut) {
    TimeoutSyncSEP sep(TimeoutSyncSEP::kShouldTimeout);
    auto tla = makeAndStartTL(&sep);

    TimeoutConnector connector(tla->listenerPort(), false);

    sep.waitForTimeout();
    tla->shutdown();
}

/* check that timeouts don't time out unless there's an actual timeout */
TEST(TransportLayerASIO, SourceSyncTimeoutSucceeds) {
    TimeoutSyncSEP sep(TimeoutSyncSEP::kNoTimeout);
    auto tla = makeAndStartTL(&sep);

    TimeoutConnector connector(tla->listenerPort(), true);

    sep.waitForTimeout();
    tla->shutdown();
}

/* check that switching from timeouts to no timeouts correctly resets the timeout to unlimited */
class TimeoutSwitchModesSEP : public TimeoutSEP {
public:
    void startSession(transport::SessionHandle session) override {
        log() << "Accepted connection from " << session->remote();
        startWorkerThread([ this, session = std::move(session) ]() mutable {
            log() << "waiting for message";
            auto sourceMessage = [&] { return session->sourceMessage().getStatus(); };

            // the first message we source should time out.
            session->setTimeout(Milliseconds{500});
            ASSERT_EQ(sourceMessage(), ErrorCodes::NetworkTimeout);
            notifyComplete();

            log() << "timed out successfully";

            // get the session back in a known state with the timeout still in place
            ASSERT_OK(sourceMessage());
            notifyComplete();

            log() << "waiting for message without a timeout";

            // this should block and timeout the waitForComplete mutex, and the session should wait
            // for a while to make sure this isn't timing out and then send a message to unblock
            // the this call to recv
            session->setTimeout(boost::none);
            ASSERT_OK(sourceMessage());

            session.reset();
            notifyComplete();
            log() << "ending test";
        });
    }
};

TEST(TransportLayerASIO, SwitchTimeoutModes) {
    TimeoutSwitchModesSEP sep;
    auto tla = makeAndStartTL(&sep);

    TimeoutConnector connector(tla->listenerPort(), false);

    ASSERT_TRUE(sep.waitForTimeout());

    connector.sendMessage();
    ASSERT_TRUE(sep.waitForTimeout());

    ASSERT_FALSE(sep.waitForTimeout(Milliseconds{1000}));
    connector.sendMessage();
    ASSERT_TRUE(sep.waitForTimeout());

    tla->shutdown();
}

}  // namespace
}  // namespace mongo
