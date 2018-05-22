/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include <memory>

#include "mongo/client/async_client.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"

namespace mongo {
namespace executor {
namespace connection_pool_tl {

class TLTypeFactory final : public ConnectionPool::DependentTypeFactoryInterface {
public:
    TLTypeFactory(transport::ReactorHandle reactor,
                  transport::TransportLayer* tl,
                  std::unique_ptr<NetworkConnectionHook> onConnectHook)
        : _reactor(std::move(reactor)), _tl(tl), _onConnectHook(std::move(onConnectHook)) {}

    std::shared_ptr<ConnectionPool::ConnectionInterface> makeConnection(
        const HostAndPort& hostAndPort, size_t generation) override;
    std::unique_ptr<ConnectionPool::TimerInterface> makeTimer() override;

    Date_t now() override;

private:
    transport::ReactorHandle _reactor;
    transport::TransportLayer* _tl;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
};

class TLTimer final : public ConnectionPool::TimerInterface {
public:
    explicit TLTimer(const transport::ReactorHandle& reactor)
        : _reactor(reactor), _timer(_reactor->makeTimer()) {}

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;

private:
    transport::ReactorHandle _reactor;
    std::unique_ptr<transport::ReactorTimer> _timer;
};

class TLConnection final : public ConnectionPool::ConnectionInterface {
public:
    TLConnection(transport::ReactorHandle reactor,
                 ServiceContext* serviceContext,
                 HostAndPort peer,
                 size_t generation,
                 NetworkConnectionHook* onConnectHook)
        : _reactor(reactor),
          _serviceContext(serviceContext),
          _timer(_reactor),
          _peer(std::move(peer)),
          _generation(generation),
          _onConnectHook(onConnectHook) {}

    void indicateSuccess() override;
    void indicateFailure(Status status) override;
    void indicateUsed() override;
    const HostAndPort& getHostAndPort() const override;
    bool isHealthy() override;
    AsyncDBClient* client();

private:
    Date_t getLastUsed() const override;
    const Status& getStatus() const override;

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;
    void setup(Milliseconds timeout, SetupCallback cb) override;
    void resetToUnknown() override;
    void refresh(Milliseconds timeout, RefreshCallback cb) override;

    size_t getGeneration() const override;

private:
    transport::ReactorHandle _reactor;
    ServiceContext* const _serviceContext;
    TLTimer _timer;
    HostAndPort _peer;
    size_t _generation;
    NetworkConnectionHook* const _onConnectHook;
    AsyncDBClient::Handle _client;
    Date_t _lastUsed;
    Status _status = ConnectionPool::kConnectionStateUnknown;
};

}  // namespace connection_pool_asio
}  // namespace executor
}  // namespace mongo
