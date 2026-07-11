// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
