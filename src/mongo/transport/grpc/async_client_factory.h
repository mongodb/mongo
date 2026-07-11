// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/async_client.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo::transport::grpc {

/**
 * An AsyncClientFactory implementation that produces AsyncDBClient's backed by GRPCSessions.
 * This relies on the GRPCTransportLayer-global channel pool and does not own one of its own.
 * Sessions do not perform the MongoDB handshake nor are they pooled upon return.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] GRPCAsyncClientFactory : public executor::AsyncClientFactory {
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
    std::mutex _mutex;

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
