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

#pragma once

#include <memory>

#include "mongo/client/async_client.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/util/future.h"

namespace mongo {
namespace executor {
namespace connection_pool_tl {

class TLTypeFactory final : public ConnectionPool::DependentTypeFactoryInterface,
                            public std::enable_shared_from_this<TLTypeFactory> {
public:
    class Type;

    TLTypeFactory(transport::ReactorHandle reactor,
                  transport::TransportLayer* tl,
                  std::unique_ptr<NetworkConnectionHook> onConnectHook,
                  const ConnectionPool::Options& connPoolOptions)
        : _executor(std::move(reactor)),
          _tl(tl),
          _onConnectHook(std::move(onConnectHook)),
          _connPoolOptions(connPoolOptions) {}

    std::shared_ptr<ConnectionPool::ConnectionInterface> makeConnection(
        const HostAndPort& hostAndPort,
        transport::ConnectSSLMode sslMode,
        size_t generation) override;
    std::shared_ptr<ConnectionPool::TimerInterface> makeTimer() override;
    const std::shared_ptr<OutOfLineExecutor>& getExecutor() override {
        return _executor;
    }

    Date_t now() override;

    void shutdown() override;
    bool inShutdown() const;
    void fasten(Type* type);
    void release(Type* type);

private:
    auto reactor();

    std::shared_ptr<OutOfLineExecutor> _executor;  // This is always a transport::Reactor
    transport::TransportLayer* _tl;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
    const ConnectionPool::Options _connPoolOptions;

    mutable stdx::mutex _mutex;
    AtomicWord<bool> _inShutdown{false};
    stdx::unordered_set<Type*> _collars;
};

class TLTypeFactory::Type : public std::enable_shared_from_this<TLTypeFactory::Type> {
    friend class TLTypeFactory;

    Type(const Type&) = delete;
    Type& operator=(const Type&) = delete;

public:
    explicit Type(const std::shared_ptr<TLTypeFactory>& factory);
    ~Type();

    void release();
    bool inShutdown() const {
        return _factory->inShutdown();
    }

    virtual void kill() = 0;

private:
    std::shared_ptr<TLTypeFactory> _factory;
    bool _wasReleased = false;
};

class TLTimer final : public ConnectionPool::TimerInterface, public TLTypeFactory::Type {
public:
    explicit TLTimer(const std::shared_ptr<TLTypeFactory>& factory,
                     const transport::ReactorHandle& reactor)
        : TLTypeFactory::Type(factory), _reactor(reactor), _timer(_reactor->makeTimer()) {}
    ~TLTimer() {
        // Release must be the first expression of this dtor
        release();
    }

    void kill() override {
        cancelTimeout();
    }

    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;
    Date_t now() override;

private:
    transport::ReactorHandle _reactor;
    std::shared_ptr<transport::ReactorTimer> _timer;
};

class TLConnection final : public ConnectionPool::ConnectionInterface, public TLTypeFactory::Type {
public:
    TLConnection(const std::shared_ptr<TLTypeFactory>& factory,
                 transport::ReactorHandle reactor,
                 ServiceContext* serviceContext,
                 HostAndPort peer,
                 transport::ConnectSSLMode sslMode,
                 size_t generation,
                 NetworkConnectionHook* onConnectHook,
                 bool skipAuth)
        : ConnectionInterface(generation),
          TLTypeFactory::Type(factory),
          _reactor(reactor),
          _serviceContext(serviceContext),
          _timer(factory->makeTimer()),
          _skipAuth(skipAuth),
          _peer(std::move(peer)),
          _sslMode(sslMode),
          _onConnectHook(onConnectHook) {}
    ~TLConnection() {
        // Release must be the first expression of this dtor
        release();
    }

    void kill() override {
        cancelAsync();
    }

    const HostAndPort& getHostAndPort() const override;
    transport::ConnectSSLMode getSslMode() const override;
    bool isHealthy() override;
    AsyncDBClient* client();
    Date_t now() override;

private:
    void setTimeout(Milliseconds timeout, TimeoutCallback cb) override;
    void cancelTimeout() override;
    void setup(Milliseconds timeout, SetupCallback cb) override;
    void refresh(Milliseconds timeout, RefreshCallback cb) override;
    void cancelAsync();

private:
    transport::ReactorHandle _reactor;
    ServiceContext* const _serviceContext;
    std::shared_ptr<ConnectionPool::TimerInterface> _timer;
    const bool _skipAuth;

    HostAndPort _peer;
    transport::ConnectSSLMode _sslMode;
    NetworkConnectionHook* const _onConnectHook;
    AsyncDBClient::Handle _client;
};

}  // namespace connection_pool_tl
}  // namespace executor
}  // namespace mongo
