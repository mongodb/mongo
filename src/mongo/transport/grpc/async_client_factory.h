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

#include "mongo/client/async_client.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

namespace mongo::transport::grpc {

/**
 * An AsyncClientFactory implementation that produces AsyncDBClient's backed by GRPCSessions.
 * This relies on the GRPCTransportLayer-global channel pool and does not own one of its own.
 * Sessions do not perform the MongoDB handshake nor are they pooled upon return.
 */
class GRPCAsyncClientFactory : public executor::AsyncClientFactory {
public:
    static constexpr auto kDiagnosticLogLevel = 4;
    static constexpr auto kDefaultStreamEstablishmentTimeout = Seconds(20);

    GRPCAsyncClientFactory(std::string instanceName);

    ~GRPCAsyncClientFactory() override;

    void startup(ServiceContext* svcCtx,
                 transport::TransportLayer* tl,
                 transport::ReactorHandle reactor) override;

    transport::TransportProtocol getTransportProtocol() const override {
        return transport::TransportProtocol::GRPC;
    }

    SemiFuture<std::shared_ptr<AsyncClientHandle>> get(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable()) override;

    SemiFuture<std::shared_ptr<AsyncClientHandle>> lease(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable()) override;

    void shutdown() override;

    void appendStats(BSONObjBuilder& bob) const override;
    GRPCConnectionStats getStats() const;

    // EgressConnectionCloser requirements.

    void dropConnections(const Status& status) override;
    void dropConnections(const HostAndPort& target, const Status& status) override;
    void setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) override;

private:
    class Handle : public AsyncClientFactory::AsyncClientHandle {
    public:
        explicit Handle(GRPCAsyncClientFactory* factory,
                        ServiceContext* svcCtx,
                        std::shared_ptr<AsyncDBClient> client,
                        bool lease)
            : _factory(factory),
              _client(std::move(client)),
              _acquiredTimer(std::make_shared<Timer>(svcCtx->getTickSource())),
              _lease(lease) {}

        ~Handle() override {
            _factory->_destroyHandle(*this);
        }

        const HostAndPort& getRemote() {
            return getClient().remote();
        }

        AsyncDBClient& getClient() override {
            return *_client;
        }

        void startAcquiredTimer() override {
            _acquiredTimer->reset();
        }

        std::shared_ptr<Timer> getAcquiredTimer() override {
            return _acquiredTimer;
        }

        bool isLeased() const {
            return _lease;
        }

        void indicateUsed() override {
            // We don't pool gRPC streams, so we don't care when it was last used.
        }

        void indicateSuccess() override {
            _outcome = Status::OK();
        }

        void indicateFailure(Status s) override {
            _outcome = std::move(s);
        }

    private:
        friend class GRPCAsyncClientFactory;

        GRPCAsyncClientFactory* _factory;
        std::shared_ptr<AsyncDBClient> _client;
        std::shared_ptr<Timer> _acquiredTimer;

        //  This is tracked for stats only. gRPC leased streams aren't any different from regular
        //  streams.
        bool _lease;

        // Iterator pointing to this handle's entry in one of the factory's active handles lists.
        boost::optional<std::list<AsyncDBClient*>::iterator> _it;

        boost::optional<Status> _outcome;
    };

    struct EndpointState {
        std::list<AsyncDBClient*> handles;
        bool keepOpen{false};
    };

    Future<std::shared_ptr<AsyncClientHandle>> _get(bool lease,
                                                    const HostAndPort& target,
                                                    transport::ConnectSSLMode sslMode,
                                                    Milliseconds timeout,
                                                    const CancellationToken& token);

    void _destroyHandle(Handle& handle);
    void _dropConnections(WithLock);
    void _dropConnections(WithLock, EndpointState& target);

    bool _shutdownComplete(WithLock lk) {
        return _state == State::kShutdown && _numActiveHandles == 0 &&
            _finishingClientList.size() == 0;
    }

    std::string _instanceName;

    stdx::condition_variable _cv;
    stdx::mutex _mutex;

    enum class State { kNew, kStarted, kShutdown };
    State _state{State::kNew};

    // All accesses of _numActiveHandles must be guarded by the mutex.
    std::uint64_t _numActiveHandles{0};

    Counter64 _numLeasedStreams;
    Counter64 _numStreamsCreated;
    Counter64 _totalStreamUsageTimeMs;
    stdx::unordered_map<HostAndPort, EndpointState> _endpoints;

    struct FinishingClientState {
        FinishingClientState(std::shared_ptr<AsyncDBClient> c) : client(std::move(c)) {}

        std::shared_ptr<AsyncDBClient> client;
        boost::optional<std::list<FinishingClientState>::iterator> it;
    };
    std::list<FinishingClientState> _finishingClientList;

    GRPCTransportLayer* _tl = nullptr;
    ServiceContext* _svcCtx = nullptr;
    transport::ReactorHandle _reactor;
    std::shared_ptr<Client> _client;
};
}  // namespace mongo::transport::grpc
