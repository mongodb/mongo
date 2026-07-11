// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/client_transport_observer.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/grpc_transport_layer.h"
#include "mongo/transport/grpc/reactor.h"
#include "mongo/transport/grpc/server.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/session_manager.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo::transport::grpc {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] GRPCTransportLayerImpl : public GRPCTransportLayer {
public:
    // Note that passing `nullptr` for {sessionManager} will disallow ingress usage.
    GRPCTransportLayerImpl(ServiceContext* svcCtx,
                           Options options,
                           std::unique_ptr<SessionManager> sessionManager);

    /**
     * Create a GRPCTransportLayerImpl instance suitable for ingress (and optionally egress).
     * The instantiated TL will have CommandService pre-attached to route requests via
     * sessionManager->startSession().
     *
     * Note that this TransportLayer will throw during `setup()`
     * if no tlsCertificateKeyFile is available when ingress mode is set.
     */
    static std::unique_ptr<GRPCTransportLayerImpl> createWithConfig(
        ServiceContext*,
        Options options,
        std::vector<std::shared_ptr<ClientTransportObserver>> observers);

    Status registerService(std::unique_ptr<Service> svc) override;

    Status setup() override;

    Status start() override;

    void shutdown() override;

    void stopAcceptingSessions() override;

    std::shared_ptr<Client> createGRPCClient(BSONObj clientMetadata) override;

    StatusWith<std::shared_ptr<Session>> connectWithAuthToken(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        boost::optional<std::string> authToken = boost::none) override;

    StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        const boost::optional<TransientSSLParams>& transientSSLParams = boost::none) override;

    Future<std::shared_ptr<Session>> asyncConnectWithAuthToken(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        const CancellationToken& token = CancellationToken::uncancelable(),
        boost::optional<std::string> authToken = boost::none) override;

    Future<std::shared_ptr<Session>> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext) override;

    void appendStatsForServerStatus(BSONObjBuilder* bob) const override;

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override;
#endif

    ReactorHandle getReactor(WhichReactor which) override {
        switch (which) {
            case TransportLayer::kIngress:
                MONGO_UNIMPLEMENTED;
            case TransportLayer::kEgress:
                return _egressReactor;
            case TransportLayer::kNewReactor:
                return std::make_shared<GRPCReactor>();
        }

        MONGO_UNREACHABLE;
    }

    const std::vector<HostAndPort>& getListeningAddresses() const override;

    SessionManager* getSessionManager() const override {
        return _sessionManager.get();
    }

    std::shared_ptr<SessionManager> getSharedSessionManager() const override {
        return _sessionManager;
    }

    bool isIngress() const override {
        return _options.enableIngress;
    }

    bool isEgress() const override {
        return _options.enableEgress;
    }

private:
    mutable std::mutex _mutex;
    bool _isShutdown = false;

    // This default client is used in synchronous networking (DBClientGRPCStream). Asynchronous
    // networking uses the client returned by createGRPCClient.
    std::shared_ptr<Client> _defaultClient;
    std::unique_ptr<Server> _server;
    ServiceContext* const _svcCtx;

    // Callers that acquire a client through createGRPCClient should be responsible for all actions
    // related to the client, but we still need access to all clients to fit into some of the
    // existing TransportLayer abstractions (ie, rotateCertificates must operate on all clients).
    struct ClientEntry {
        std::weak_ptr<Client> client;
        std::list<ClientEntry>::iterator iter;
    };
    std::list<ClientEntry> _clients;

    /**
     * The GRPCTransportLayer starts an egress reactor on an _ioThread that is provided to
     * streams/sessions on calls to the synchronous connect() function. Providing this default
     * reactor allows use of the synchronous transport layer APIs with gRPC's async completion queue
     * API.
     */
    stdx::thread _ioThread;
    std::shared_ptr<GRPCReactor> _egressReactor;

    GRPCClient::Options _clientOptions;

    /**
     * The filepath for the grpc unix domain socket. This value gets populated after a call to
     * setup() only when _options.useUnixDomainSockets is true.
     */
    std::string _unixSockPath;

    // Invalidated after setup().
    std::vector<std::unique_ptr<Service>> _services;
    Options _options;
    std::shared_ptr<SessionManager> _sessionManager;
};

}  // namespace mongo::transport::grpc
