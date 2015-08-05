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

#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/async_stream_interface.h"

namespace mongo {
namespace executor {
namespace connection_pool_asio {

/**
 * Implements connection pool timers on top of asio
 */
class ASIOTimer final : public ConnectionPool::TimerInterface {
public:
    ASIOTimer(asio::io_service* service);

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;

private:
    TimeoutCallback _cb;
    asio::io_service* const _io_service;
    asio::steady_timer _impl;
};

/**
 * Implements connection pool connections on top of asio
 *
 * Owns an async op when it's out of the pool
 */
class ASIOConnection final : public ConnectionPool::ConnectionInterface {
public:
    ASIOConnection(const HostAndPort& hostAndPort, size_t generation, ASIOImpl* global);

    void indicateUsed() override;
    void indicateFailed() override;
    const HostAndPort& getHostAndPort() const override;

    std::unique_ptr<NetworkInterfaceASIO::AsyncOp> releaseAsyncOp();
    void bindAsyncOp(std::unique_ptr<NetworkInterfaceASIO::AsyncOp> op);

private:
    Date_t getLastUsed() const override;
    bool isFailed() const override;

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;

    void setup(Milliseconds timeout, SetupCallback cb) override;
    void refresh(Milliseconds timeout, RefreshCallback cb) override;

    size_t getGeneration() const override;

    static std::unique_ptr<NetworkInterfaceASIO::AsyncOp> makeAsyncOp(ASIOConnection* conn);
    static RemoteCommandRequest makeIsMasterRequest(ASIOConnection* conn);

private:
    SetupCallback _setupCallback;
    RefreshCallback _refreshCallback;
    ASIOImpl* const _global;
    ASIOTimer _timer;
    Date_t _lastUsed;
    bool _isFailed = false;
    HostAndPort _hostAndPort;
    size_t _generation;
    std::unique_ptr<NetworkInterfaceASIO::AsyncOp> _impl;
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
