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


#include "mongo/transport/grpc/grpc_transport_layer.h"

#include <memory>
#include <sys/stat.h>

#include "mongo/db/server_options.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_options.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

GRPCTransportLayer::Options::Options(const ServerGlobalParams& params) {
    bindIpList = params.bind_ips;
    bindPort = params.grpcPort;
    useUnixDomainSockets = !params.noUnixSocket;
    unixDomainSocketPermissions = params.unixSocketPermissions;
    maxServerThreads = params.grpcServerMaxThreads;
}

GRPCTransportLayer::GRPCTransportLayer(ServiceContext* svcCtx, Options options)
    : _svcCtx(svcCtx), _options(std::move(options)) {}

Status GRPCTransportLayer::registerService(std::unique_ptr<Service> svc) try {
    stdx::lock_guard lk(_mutex);
    invariant(!_server,
              "Cannot register gRPC services after GRPCTransportLayer::setup() has been invoked");
    iassert(TransportLayer::ShutdownStatus.code(),
            "Cannot register gRPC services after the GRPCTransportLayer has been shut down",
            !_isShutdown);
    _services.push_back(std::move(svc));
    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

Status GRPCTransportLayer::setup() try {
    stdx::lock_guard lk(_mutex);
    iassert(TransportLayer::ShutdownStatus.code(),
            "Cannot set up GRPCTransportLayer after it has been shut down",
            !_isShutdown);

    if (!_services.empty()) {
        std::vector<std::unique_ptr<Service>> services;
        services.swap(_services);
        Server::Options serverOptions;
        if (_options.bindIpList.empty()) {
            _options.bindIpList.push_back("127.0.0.1");
        }
        std::vector<HostAndPort> addresses;
        for (auto& ip : _options.bindIpList) {
            addresses.push_back(HostAndPort(ip, _options.bindPort));
        }
        if (_options.useUnixDomainSockets) {
            addresses.push_back(HostAndPort(makeUnixSockPath(_options.bindPort)));
        }
        serverOptions.addresses = std::move(addresses);
        serverOptions.maxThreads = _options.maxServerThreads;

        if (!sslGlobalParams.sslCAFile.empty()) {
            serverOptions.tlsCAFile = sslGlobalParams.sslCAFile;
        }
        if (!sslGlobalParams.sslPEMKeyFile.empty()) {
            serverOptions.tlsPEMKeyFile = sslGlobalParams.sslPEMKeyFile;
        }
        _server = std::make_unique<Server>(std::move(services), serverOptions);
    }
    if (_options.enableEgress) {
        GRPCClient::Options clientOptions;
        if (!sslGlobalParams.sslCAFile.empty()) {
            clientOptions.tlsCAFile = sslGlobalParams.sslCAFile;
        }
        if (!sslGlobalParams.sslPEMKeyFile.empty()) {
            clientOptions.tlsCertificateKeyFile = sslGlobalParams.sslPEMKeyFile;
        }
        iassert(ErrorCodes::InvalidOptions,
                "gRPC egress networking enabled but no client metadata document was provided",
                _options.clientMetadata.has_value());
        _client =
            std::make_shared<GRPCClient>(this, *_options.clientMetadata, std::move(clientOptions));
    }
    return Status::OK();
} catch (const DBException& e) {
    return e.toStatus();
}

Status GRPCTransportLayer::start() try {
    stdx::lock_guard lk(_mutex);
    iassert(TransportLayer::ShutdownStatus.code(),
            "Cannot start GRPCTransportLayer after it has been shut down",
            !_isShutdown);

    // We can't distinguish between the default list of disabled protocols and one specified by
    // via options, so we just log that the list is being ignored when using gRPC.
    if (!sslGlobalParams.sslDisabledProtocols.empty()) {
        LOGV2_DEBUG(8000811,
                    3,
                    "Ignoring tlsDisabledProtocols for gRPC-based connections",
                    "tlsDisabledProtocols"_attr = sslGlobalParams.sslDisabledProtocols);
    }
    uassert(ErrorCodes::InvalidSSLConfiguration,
            "Specifying a CRL file is not supported when gRPC mode is enabled",
            sslGlobalParams.sslCRLFile.empty());
    uassert(ErrorCodes::InvalidSSLConfiguration,
            "Certificate passwords are not supported when gRPC mode is enabled",
            sslGlobalParams.sslPEMKeyPassword.empty());
    uassert(ErrorCodes::InvalidSSLConfiguration,
            "tlsFIPSMode is not supported when gRPC mode is enabled",
            !sslGlobalParams.sslFIPSMode);

    if (_server) {
        _server->start();
        if (_options.useUnixDomainSockets) {
            setUnixDomainSocketPermissions(makeUnixSockPath(_options.bindPort),
                                           _options.unixDomainSocketPermissions);
        }
    }
    if (_client) {
        _client->start(_svcCtx);
    }
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

StatusWith<std::shared_ptr<Session>> GRPCTransportLayer::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<TransientSSLParams> transientSSLParams) try {

    iassert(ErrorCodes::InvalidSSLConfiguration,
            "SSL must be enabled when using gRPC",
            sslMode == ConnectSSLMode::kEnableSSL ||
                (sslMode == transport::ConnectSSLMode::kGlobalSSLMode &&
                 sslGlobalParams.sslMode.load() != SSLParams::SSLModes::SSLMode_disabled));
    iassert(ErrorCodes::InvalidSSLConfiguration,
            "Transient SSL parameters are not supported when using gRPC",
            !transientSSLParams);
    invariant(_client);

    return _client->connect(peer, timeout, {});
} catch (const DBException& e) {
    return e.toStatus();
}

#ifdef MONGO_CONFIG_SSL
Status GRPCTransportLayer::rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                              bool asyncOCSPStaple) {
    // gRPC's C++ library does not currently support rotating TLS certificates manually.
    LOGV2_INFO(7402001, "Ignoring request to rotate TLS certificates for the gRPC transport layer");
    return Status::OK();
}

StatusWith<std::shared_ptr<const transport::SSLConnectionContext>>
GRPCTransportLayer::createTransientSSLContext(const TransientSSLParams& transientSSLParams) {
    return Status(ErrorCodes::InvalidSSLConfiguration,
                  "Transient SSL contexts are not supported when using gRPC");
}
#endif

void GRPCTransportLayer::shutdown() {
    stdx::lock_guard lk(_mutex);
    if (std::exchange(_isShutdown, true)) {
        return;
    }

    if (_server) {
        _server->shutdown();
    }
    if (_client) {
        _client->shutdown();
    }
}

}  // namespace mongo::transport::grpc
