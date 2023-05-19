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

#include "mongo/transport/grpc/server.h"

#include <grpc/compression.h>
#include <grpc/grpc_security_constants.h>
#include <grpcpp/resource_quota.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_options.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

Server::Server(std::vector<std::unique_ptr<Service>> services, Options options)
    : _options{std::move(options)}, _services{std::move(services)}, _shutdown{false} {}


Server::~Server() {
    invariant(!isRunning());
}

std::shared_ptr<::grpc::ServerCredentials> _makeServerCredentials(const Server::Options& options) {
    auto clientCertPolicy = ::grpc_ssl_client_certificate_request_type::
        GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;

    if (options.tlsAllowConnectionsWithoutCertificates && options.tlsAllowInvalidCertificates) {
        clientCertPolicy =
            ::grpc_ssl_client_certificate_request_type::GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;
    } else if (options.tlsAllowConnectionsWithoutCertificates) {
        clientCertPolicy = ::grpc_ssl_client_certificate_request_type::
            GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY;
    } else if (options.tlsAllowInvalidCertificates) {
        clientCertPolicy = ::grpc_ssl_client_certificate_request_type::
            GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY;
    }

    ::grpc::SslServerCredentialsOptions sslOps{clientCertPolicy};
    ::grpc::SslServerCredentialsOptions::PemKeyCertPair certPair;
    sslOps.pem_key_cert_pairs = {parsePEMKeyFile(options.tlsPEMKeyFile)};
    if (options.tlsCAFile) {
        sslOps.pem_root_certs = uassertStatusOK(ssl_util::readPEMFile(*options.tlsCAFile));
    }
    return ::grpc::SslServerCredentials(sslOps);
}

void Server::start() {
    stdx::lock_guard lk(_mutex);
    invariant(!_shutdown, "Cannot start the server once it's stopped");
    invariant(!_server, "The server is already started");

    ::grpc::ServerBuilder builder;

    auto credentials = _makeServerCredentials(_options);

    for (auto& address : _options.addresses) {
        std::string fullAddress;
        if (!isUnixDomainSocket(address)) {
            fullAddress = fmt::format("{}:{}", address, _options.port);
        } else {
            fullAddress = fmt::format("unix://{}", address);
        }
        builder.AddListeningPort(fullAddress, credentials);
    }
    for (auto& service : _services) {
        builder.RegisterService(service.get());
    }
    builder.SetMaxReceiveMessageSize(MaxMessageSizeBytes);
    builder.SetMaxSendMessageSize(MaxMessageSizeBytes);
    builder.SetDefaultCompressionAlgorithm(::grpc_compression_algorithm::GRPC_COMPRESS_NONE);
    ::grpc::ResourceQuota quota;
    quota.SetMaxThreads(_options.maxThreads);
    builder.SetResourceQuota(quota);

    _server = builder.BuildAndStart();

    if (!_server) {
        LOGV2_ERROR_OPTIONS(7401309,
                            {logv2::UserAssertAfterLog(ErrorCodes::UnknownError)},
                            "Failed to start gRPC server",
                            "addresses"_attr = _options.addresses,
                            "port"_attr = _options.port,
                            "services"_attr = _services);
    }

    LOGV2_INFO(7401305,
               "Started gRPC server",
               "addresses"_attr = _options.addresses,
               "port"_attr = _options.port,
               "services"_attr = _services);
}

bool Server::isRunning() const {
    stdx::lock_guard lk(_mutex);
    return _server && !_shutdown;
}

void Server::shutdown() {
    stdx::lock_guard lk(_mutex);
    invariant(!_shutdown, "Cannot shutdown the server once it's stopped");
    _shutdown = true;
    if (!_server) {
        return;
    }

    LOGV2_INFO(7401306, "Shutting down gRPC server");

    for (auto& service : _services) {
        service->shutdown();
    }

    _server->Shutdown();
    LOGV2_DEBUG(7401307, 1, "gRPC server shutdown complete");
    _services.clear();
}

}  // namespace mongo::transport::grpc
