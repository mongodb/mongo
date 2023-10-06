/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/server.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

namespace mongo::transport::grpc {

/**
 * Wraps the gRPC Server and Client implementations. This abstraction layer aims to hide
 * gRPC-specific details from `SessionWorkflow`, `ServiceEntryPoint`, and the remainder of the
 * command execution path.
 *
 * The egress portion of this TransportLayer always communicates via the mongodb.CommandService,
 * but other arbitrary gRPC services can be used in the ingress portion. Such services can be
 * registered with this transport layer via registerService() before setup() is called.
 */
class GRPCTransportLayer : public TransportLayer {
public:
    struct Options {
        explicit Options(const ServerGlobalParams& params);
        Options() : Options(ServerGlobalParams()) {}

        bool enableEgress = false;
        std::vector<std::string> bindIpList;
        int bindPort;
        bool useUnixDomainSockets;
        int unixDomainSocketPermissions;
        int maxServerThreads;
        boost::optional<BSONObj> clientMetadata;
    };

    GRPCTransportLayer(ServiceContext* svcCtx, Options options);

    /**
     * Add the service to the list that will be served once this transport layer has been started.
     * If no services have been registered at the time when setup() is invoked, no server will be
     * created. All services must be registered before setup() is invoked.
     */
    Status registerService(std::unique_ptr<Service> svc);

    Status setup() override;

    Status start() override;

    /**
     * Cancels all outstanding RPCs (both ingress and egress) and blocks until they have completed.
     * If egress mode is enabled, this entails waiting for all sessions to be destructed.
     * If ingress mode is enabled, this entails waiting for all RPC handlers to return.
     */
    void shutdown() override;

    StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        boost::optional<TransientSSLParams> transientSSLParams = boost::none) override;

    /**
     * The server's current gRPC integration doesn't support async networking, so this is
     * left unimplemented.
     */
    Future<std::shared_ptr<Session>> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext) override {
        MONGO_UNIMPLEMENTED;
    }

    /**
     * Not applicable to gRPC networking.
     */
    ReactorHandle getReactor(WhichReactor) override {
        MONGO_UNIMPLEMENTED;
    }

#ifdef MONGO_CONFIG_SSL
    /**
     * gRPC's C++ library does not currently support rotating TLS certificates manually, so this
     * just does nothing and logs a message.
     */
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override;

    /**
     * The gRPC integration doesn't support the use of transient SSL contexts, so this always
     * returns an error.
     */
    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams& transientSSLParams) override;
#endif

private:
    mutable stdx::mutex _mutex;
    bool _isShutdown = false;

    std::shared_ptr<Client> _client;
    std::unique_ptr<Server> _server;
    ServiceContext* const _svcCtx;
    // Invalidated after setup().
    std::vector<std::unique_ptr<Service>> _services;
    Options _options;
};

}  // namespace mongo::transport::grpc
