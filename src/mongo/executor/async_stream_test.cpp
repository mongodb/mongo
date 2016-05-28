/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include <asio.hpp>
#include <system_error>
#include <vector>

#include "mongo/executor/async_stream.h"
#include "mongo/executor/network_interface_asio_test_utils.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using asio::ip::tcp;

class Server {
public:
    void startup() {
        _thread = stdx::thread([this] {
            try {
                tcp::acceptor acceptor{_io_service,
                                       // Let the OS choose the accepting port.
                                       tcp::endpoint{asio::ip::make_address_v4("127.0.0.1"), 0}};
                _endpoint.emplace(acceptor.local_endpoint());
                tcp::socket socket{_io_service};
                _started.emplace(true);
                acceptor.accept(socket);
                log() << "got incoming connection";
                _stopped.get();
                log() << "shutting down socket";
                socket.shutdown(tcp::socket::shutdown_both);
            } catch (...) {
                log() << exceptionToStatus();
            }
        });
        _started.get();
    }

    // idempotent
    void shutdown() {
        _stopped.emplace(true);
        _thread.join();
    }

    Server() {
        startup();
    }

    tcp::endpoint endpoint() {
        return _endpoint.get();
    }

    ~Server() {
        if (!_stopped.hasCompleted()) {
            shutdown();
        }
    }

private:
    stdx::thread _thread;
    executor::Deferred<bool> _started;
    executor::Deferred<bool> _stopped;
    executor::Deferred<tcp::endpoint> _endpoint;
    asio::io_service _io_service;
    std::vector<tcp::socket> _sockets;
};

TEST(AsyncStreamTest, IsOpen) {
    Server server;
    asio::io_service io_service;
    stdx::thread clientWorker([&io_service] {
        log() << "starting clientWorker";
        asio::io_service::work work(io_service);
        io_service.run();
    });
    auto guard = MakeGuard([&clientWorker, &io_service] {
        io_service.stop();
        clientWorker.join();
    });
    asio::io_service::strand strand{io_service};
    executor::AsyncStream stream{&strand};
    ASSERT_FALSE(stream.isOpen());

    tcp::resolver resolver{io_service};
    auto endpoints = resolver.resolve(server.endpoint());

    executor::Deferred<bool> opened;

    log() << "opening up outgoing connection";
    stream.connect(endpoints, [opened](std::error_code ec) mutable {
        log() << "opened outgoing connection";
        opened.emplace(!ec);
    });

    ASSERT_TRUE(opened.get());
    ASSERT_TRUE(stream.isOpen());

    server.shutdown();

    // There is nothing we can wait on to determinstically know when
    // the socket will transition to closed. Busy wait for that.
    while (stream.isOpen()) {
        stdx::this_thread::sleep_for(Milliseconds(1).toSystemDuration());
    }
}

}  // namespace
}  // namespace mongo
