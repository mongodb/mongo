/** *    Copyright (C) 2015 MongoDB Inc.
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

#pragma once

#include <asio/system_timer.hpp>

#include <memory>

#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/stdx/mutex.h"

namespace mongo {
namespace executor {
namespace connection_pool_asio {

/**
 * Implements connection pool timers on top of asio
 */
class ASIOTimer final : public ConnectionPool::TimerInterface {
public:
    using clock_type = asio::system_timer::clock_type;

    ASIOTimer(asio::io_service::strand* strand);
    ~ASIOTimer();

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;

private:
    struct CallbackSharedState {
        stdx::mutex mutex;
        std::size_t id = 0;
    };

    TimeoutCallback _cb;
    asio::io_service::strand* const _strand;
    asio::basic_waitable_timer<clock_type> _impl;
    std::shared_ptr<CallbackSharedState> _callbackSharedState;
};

/**
 * Implements connection pool connections on top of asio
 *
 * Owns an async op when it's out of the pool
 */
class ASIOConnection final : public ConnectionPool::ConnectionInterface {
public:
    ASIOConnection(const HostAndPort& hostAndPort, size_t generation, ASIOImpl* global);

    void indicateSuccess() override;
    void indicateUsed() override;
    void indicateFailure(Status status) override;
    const HostAndPort& getHostAndPort() const override;

    std::unique_ptr<NetworkInterfaceASIO::AsyncOp> releaseAsyncOp();
    void bindAsyncOp(std::unique_ptr<NetworkInterfaceASIO::AsyncOp> op);

    bool isHealthy() override;

private:
    Date_t getLastUsed() const override;
    const Status& getStatus() const override;

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;

    void setup(Milliseconds timeout, SetupCallback cb) override;
    void resetToUnknown() override;
    void refresh(Milliseconds timeout, RefreshCallback cb) override;

    size_t getGeneration() const override;

    static std::unique_ptr<NetworkInterfaceASIO::AsyncOp> makeAsyncOp(ASIOConnection* conn);
    static Message makeIsMasterRequest(ASIOConnection* conn);

private:
    SetupCallback _setupCallback;
    RefreshCallback _refreshCallback;
    ASIOImpl* const _global;
    Date_t _lastUsed;
    Status _status = ConnectionPool::kConnectionStateUnknown;
    HostAndPort _hostAndPort;
    size_t _generation;
    std::unique_ptr<NetworkInterfaceASIO::AsyncOp> _impl;
    ASIOTimer _timer;
};

/**
 * Implementions connection pool implementation for asio
 */
class ASIOImpl final : public ConnectionPool::DependentTypeFactoryInterface {
    friend class ASIOConnection;

public:
    ASIOImpl(NetworkInterfaceASIO* impl);

    std::unique_ptr<ConnectionPool::ConnectionInterface> makeConnection(
        const HostAndPort& hostAndPort, size_t generation) override;
    std::unique_ptr<ConnectionPool::TimerInterface> makeTimer() override;

    Date_t now() override;

private:
    NetworkInterfaceASIO* const _impl;
};

}  // namespace connection_pool_asio
}  // namespace executor
}  // namespace mongo
