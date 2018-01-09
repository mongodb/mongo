/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/transport/service_entry_point.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"

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

    bool shutdown(Milliseconds timeout) override {
        return true;
    }

    Stats sessionStats() const override {
        return {};
    }

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

}  // namespace
}  // namespace mongo
