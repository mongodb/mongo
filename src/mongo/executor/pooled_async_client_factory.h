/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_tl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

#include <memory>

namespace mongo::executor {

/**
 * An AsyncClientFactory implementation based on executor::ConnectionPool.
 */
class PooledAsyncClientFactory : public executor::AsyncClientFactory {
public:
    explicit PooledAsyncClientFactory(
        std::string name,
        ConnectionPool::Options options,
        std::unique_ptr<NetworkConnectionHook> hook,
        transport::TransportProtocol protocol = transport::TransportProtocol::MongoRPC)
        : _name(std::move(name)),
          _poolOpts(std::move(options)),
          _onConnectHook(std::move(hook)),
          _protocol(protocol) {}

    void startup(ServiceContext* svcCtx,
                 transport::TransportLayer* tl,
                 transport::ReactorHandle reactor) override {
        invariant(tl->getTransportProtocol() == getTransportProtocol());

        std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext;
#ifdef MONGO_CONFIG_SSL
        if (_poolOpts.transientSSLParams) {
            transientSSLContext = uassertStatusOK(
                tl->createTransientSSLContext(_poolOpts.transientSSLParams.value()));
        }
#endif
        auto typeFactory = std::make_unique<connection_pool_tl::TLTypeFactory>(
            reactor, tl, std::move(_onConnectHook), _poolOpts, transientSSLContext, _name);
        _pool = std::make_shared<ConnectionPool>(std::move(typeFactory), _name, _poolOpts);
    }

    transport::TransportProtocol getTransportProtocol() const override {
        return _protocol;
    }

    SemiFuture<std::shared_ptr<AsyncClientHandle>> get(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable()) override {

        return _pool->get(target, sslMode, timeout, token)
            .unsafeToInlineFuture()
            .then([target](
                      ConnectionPool::ConnectionHandle conn) -> std::shared_ptr<AsyncClientHandle> {
                return std::make_shared<Handle>(std::move(conn));
            })
            .semi();
    }

    SemiFuture<std::shared_ptr<AsyncClientHandle>> lease(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable()) override {

        return _pool->lease(target, sslMode, timeout, token)
            .unsafeToInlineFuture()
            .then([](ConnectionPool::ConnectionHandle conn) -> std::shared_ptr<AsyncClientHandle> {
                return std::make_shared<Handle>(std::move(conn));
            })
            .semi();
    }

    void shutdown() override {
        _pool->shutdown();
    }

    void dropConnections(const Status& status) override {
        _pool->dropConnections(status);
    }

    void dropConnections(const HostAndPort& target, const Status& status) override {
        _pool->dropConnections(target, status);
    }

    void setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) override {
        _pool->setKeepOpen(hostAndPort, keepOpen);
    }

    void appendConnectionStats(ConnectionPoolStats* stats) const override {
        _pool->appendConnectionStats(stats);
    }

    void appendStats(BSONObjBuilder& bob) const override {}

private:
    class Handle : public AsyncClientFactory::AsyncClientHandle {
    public:
        Handle(ConnectionPool::ConnectionHandle handle) : _conn(std::move(handle)) {}

        AsyncDBClient& getClient() override {
            return *getTLConnection().client();
        }

        void startAcquiredTimer() override {
            getTLConnection().startConnAcquiredTimer();
        }

        std::shared_ptr<Timer> getAcquiredTimer() override {
            return getTLConnection().getConnAcquiredTimer();
        }

        void indicateUsed() override {
            getTLConnection().indicateUsed();
        }

        void indicateSuccess() override {
            getTLConnection().indicateSuccess();
        }

        void indicateFailure(Status s) override {
            getTLConnection().indicateFailure(std::move(s));
        }

    private:
        connection_pool_tl::TLConnection& getTLConnection() {
            return *checked_cast<connection_pool_tl::TLConnection*>(_conn.get());
        }

        ConnectionPool::ConnectionHandle _conn;
    };

    std::string _name;
    ConnectionPool::Options _poolOpts;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
    std::shared_ptr<ConnectionPool> _pool;
    transport::TransportProtocol _protocol;
};
}  // namespace mongo::executor
