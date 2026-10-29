// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/service.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional.hpp>

namespace mongo::transport::grpc {
using namespace std::literals::string_view_literals;

/**
 * Wraps the gRPC Server and Client implementations. This abstraction layer aims to hide
 * gRPC-specific details from `SessionWorkflow`, `ServiceEntryPoint`, and the remainder of the
 * command execution path.
 *
 * The egress portion of this TransportLayer always communicates via the mongodb.CommandService,
 * but other arbitrary gRPC services can be used in the ingress portion. Such services can be
 * registered with this transport layer via registerService() before setup() is called.
 *
 * On shutdown, it cancels all outstanding RPCs (both ingress and egress) and blocks until they have
 * completed. If egress mode is enabled, this entails waiting for all sessions to be destructed. If
 * ingress mode is enabled, this entails waiting for all RPC handlers to return.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] GRPCTransportLayer : public TransportLayer {
protected:
    GRPCTransportLayer() = default;

public:
    struct Options {
        explicit Options(const ServerGlobalParams& params);
        Options() : Options(ServerGlobalParams()) {}

        bool enableEgress = false;
        bool enableIngress = true;
        std::vector<std::string> bindIpList;
        /**
         * If set to 0, the transport layer will bind to an arbitrary unused port.
         * This port can be accessed via getListeningAddresses() after the transport layer has been
         * started.
         */
        int bindPort;
        bool useUnixDomainSockets;
        int unixDomainSocketPermissions;
        int maxServerThreads;
        boost::optional<BSONObj> clientMetadata;
    };

    ~GRPCTransportLayer() override {}

    /**
     * Add the service to the list that will be served once this transport layer has been started.
     * If no services have been registered at the time when setup() is invoked, no server will be
     * created. All services must be registered before setup() is invoked.
     */
    virtual Status registerService(std::unique_ptr<Service> svc) = 0;

    /**
     * Acquire a unique gRPC client, which owns a unique associated ChannelPool.
     */
    virtual std::shared_ptr<Client> createGRPCClient(BSONObj clientMetadata) = 0;

    virtual StatusWith<std::shared_ptr<Session>> connectWithAuthToken(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        boost::optional<std::string> authToken = boost::none) = 0;

    virtual Future<std::shared_ptr<Session>> asyncConnectWithAuthToken(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        const CancellationToken& token = CancellationToken::uncancelable(),
        boost::optional<std::string> authToken = boost::none) = 0;

    std::string_view getNameForLogging() const override {
        return "gRPC"sv;
    }

    TransportProtocol getTransportProtocol() const override {
        return TransportProtocol::GRPC;
    }

    /**
     * The addresses the gRPC server is listening to, if any.
     *
     * This must only be invoked after the transport layer has been started.
     */
    virtual const std::vector<HostAndPort>& getListeningAddresses() const = 0;

#ifdef MONGO_CONFIG_SSL
    /**
     * The gRPC integration doesn't support the use of transient SSL contexts, so this always
     * returns an error.
     */
    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams& transientSSLParams) override {
        return Status(ErrorCodes::InvalidSSLConfiguration,
                      "Transient SSL contexts are not supported when using gRPC.");
    }
#endif
};

}  // namespace mongo::transport::grpc
