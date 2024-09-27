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
#include <grpcpp/server_builder.h>

#include "src/core/lib/security/credentials/ssl/ssl_credentials.h"
#include "src/core/lib/security/security_connector/ssl_utils.h"
#include "src/core/tsi/ssl_transport_security.h"
#include "src/cpp/server/secure_server_credentials.h"

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

namespace {
StatusWith<Server::Certificates> _readCertificatesFromDisk(boost::optional<StringData> tlsCAFile,
                                                           StringData tlsCertificateKeyFile) {
    Server::Certificates certs;
    if (tlsCAFile) {
        StatusWith<std::string> swCAFileContents = ssl_util::readPEMFile(tlsCAFile.get());
        if (!swCAFileContents.isOK()) {
            return swCAFileContents.getStatus();
        }
        if (swCAFileContents.getValue().empty()) {
            return Status(ErrorCodes::InvalidSSLConfiguration, "Empty CA file.");
        }

        certs.caCert = swCAFileContents.getValue();
    }

    try {
        certs.keyCertPair = util::parsePEMKeyFile(tlsCertificateKeyFile);
    } catch (const DBException& e) {
        return e.toStatus();
    }

    return certs;
}

/**
 * Convert the Server::Options to a certificate request type understandable by gRPC.
 */
grpc_ssl_client_certificate_request_type _getClientCertPolicy(const Server::Options& options) {
    if (options.tlsAllowConnectionsWithoutCertificates && options.tlsAllowInvalidCertificates) {
        return ::grpc_ssl_client_certificate_request_type::GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;
    } else if (options.tlsAllowConnectionsWithoutCertificates) {
        return ::grpc_ssl_client_certificate_request_type::
            GRPC_SSL_REQUEST_CLIENT_CERTIFICATE_AND_VERIFY;
    } else if (options.tlsAllowInvalidCertificates) {
        return ::grpc_ssl_client_certificate_request_type::
            GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_BUT_DONT_VERIFY;
    } else {
        return ::grpc_ssl_client_certificate_request_type::
            GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    }
}

/**
 * Utilize gRPC's ssl_server_handshaker_factory to verify that the user has provided valid SSL
 * certificates.
 * Leaves encryption-specific options as their defaults.
 */
Status _verifySSLCredentialsWithGRPC(grpc_ssl_server_certificate_config* config,
                                     grpc_ssl_client_certificate_request_type clientCertOpts) {
    tsi_ssl_server_handshaker_factory* handshakerFactory = nullptr;
    tsi_ssl_server_handshaker_options options;
    options.pem_key_cert_pairs =
        grpc_convert_grpc_to_tsi_cert_pairs(config->pem_key_cert_pairs, config->num_key_cert_pairs);
    options.num_key_cert_pairs = config->num_key_cert_pairs;
    ON_BLOCK_EXIT([&]() {
        grpc_tsi_ssl_pem_key_cert_pairs_destroy(
            const_cast<tsi_ssl_pem_key_cert_pair*>(options.pem_key_cert_pairs),
            options.num_key_cert_pairs);
    });

    options.pem_client_root_certs = config->pem_root_certs;
    options.client_certificate_request =
        grpc_get_tsi_client_certificate_request_type(clientCertOpts);

    tsi_result result =
        tsi_create_ssl_server_handshaker_factory_with_options(&options, &handshakerFactory);
    if (result != TSI_OK) {
        return Status(ErrorCodes::InvalidSSLConfiguration, "Invalid certificates provided.");
    }
    ON_BLOCK_EXIT([&]() { tsi_ssl_server_handshaker_factory_unref(handshakerFactory); });

    return Status::OK();
}
}  // namespace

/**
 * Callback that gRPC invokes on each new connection to fetch the server's TLS certificates.
 */
grpc_ssl_certificate_config_reload_status Server::_certificateConfigCallback(
    void* certState, grpc_ssl_server_certificate_config** config) {
    CertificateState* certStatePtr = reinterpret_cast<CertificateState*>(certState);
    {
        stdx::lock_guard<stdx::mutex> lk(certStatePtr->_mutex);
        if (certStatePtr->shouldReload) {
            // gRPC library will take ownership of and free the certificate config.
            *config = certStatePtr->cache.toGRPCConfig().release();
            certStatePtr->shouldReload = false;
            return grpc_ssl_certificate_config_reload_status::
                GRPC_SSL_CERTIFICATE_CONFIG_RELOAD_NEW;
        } else {
            return GRPC_SSL_CERTIFICATE_CONFIG_RELOAD_UNCHANGED;
        }
    }
}

/**
 * Make the initial SSL credentials for the gRPC server with a callback attatched to refetch the
 * certificates on future connections. Additionally, create a CertificateState to pass to the
 * callback, initialize its cache, and add it to the maintained _certificateStates vector.
 */
std::shared_ptr<::grpc::ServerCredentials> Server::_makeServerCredentialsWithFetcher() {
    ::grpc::SslServerCredentialsOptions sslOps{_getClientCertPolicy(_options)};
    auto certState = std::make_unique<CertificateState>();

    // Load the initial certificates into the certificate state cache.
    auto newCertificates = uassertStatusOK(
        _readCertificatesFromDisk(*_options.tlsCAFile, _options.tlsCertificateKeyFile));
    certState->cache = newCertificates;

    grpc_ssl_server_credentials_options* opts =
        grpc_ssl_server_credentials_create_options_using_config_fetcher(
            sslOps.client_certificate_request, _certificateConfigCallback, certState.get());

    _certificateStates.push_back(std::move(certState));

    // Create the credentials to pass to the builder, for use on startup.
    grpc_server_credentials* creds = grpc_ssl_server_credentials_create_with_options(opts);
    invariant(creds != nullptr);

    return std::shared_ptr<::grpc::ServerCredentials>(new ::grpc::SecureServerCredentials(creds));
}

Status Server::rotateCertificates() {
    auto swNewCertificates =
        _readCertificatesFromDisk(*_options.tlsCAFile, _options.tlsCertificateKeyFile);
    if (!swNewCertificates.isOK()) {
        return swNewCertificates.getStatus();
    }

    if (auto res = _verifySSLCredentialsWithGRPC(swNewCertificates.getValue().toGRPCConfig().get(),
                                                 _getClientCertPolicy(_options));
        !res.isOK()) {
        return res;
    }

    // Set the cache to the new certs and update each port's certificate state to notify gRPC to
    // rotate them on the next new connection.
    for (auto& certState : _certificateStates) {
        stdx::lock_guard<stdx::mutex> lk(certState->_mutex);
        certState->cache = swNewCertificates.getValue();
        certState->shouldReload = true;
    }
    return Status::OK();
}

void Server::start() {
    stdx::lock_guard lk(_mutex);
    invariant(!_shutdown, "Cannot start the server once it's stopped");
    invariant(!_server, "The server is already started");

    ::grpc::ServerBuilder builder;

    std::vector<int> boundPorts(_options.addresses.size());
    for (size_t i = 0; i < _options.addresses.size(); i++) {
        auto grpcAddr = util::toGRPCFormattedURI(_options.addresses[i]);

        if (util::isUnixSchemeGRPCFormattedURI(grpcAddr)) {
            builder.AddListeningPort(grpcAddr, ::grpc::InsecureServerCredentials());
        } else {
            builder.AddListeningPort(grpcAddr, _makeServerCredentialsWithFetcher(), &boundPorts[i]);
        }
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
                            "services"_attr = _services);
    }

    for (size_t i = 0; i < boundPorts.size(); i++) {
        auto& hostAndPort = _options.addresses[i];
        if (isUnixDomainSocket(hostAndPort.host())) {
            continue;
        }
        hostAndPort = HostAndPort(hostAndPort.host(), boundPorts[i]);
    }

    LOGV2_INFO(7401305,
               "Started gRPC server",
               "addresses"_attr = _options.addresses,
               "services"_attr = _services);
}

bool Server::isRunning() const {
    stdx::lock_guard lk(_mutex);
    return _server && !_shutdown;
}

const std::vector<HostAndPort>& Server::getListeningAddresses() const {
    invariant(_server);
    return _options.addresses;
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

void Server::stopAcceptingRequests() {
    stdx::lock_guard lk(_mutex);

    for (auto& service : _services) {
        service->stopAcceptingRequests();
    }
}

}  // namespace mongo::transport::grpc
