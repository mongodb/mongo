// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/service.h"

#include <memory>
#include <mutex>
#include <string_view>

#include <grpc/grpc_security.h>
#include <grpcpp/security/server_credentials.h>

namespace mongo::transport::grpc {

class Server {
public:
    struct Options {
        /**
         * List of IP addresses, hostnames, and/or Unix domain socket paths to bind to.
         */
        std::vector<HostAndPort> addresses;
        size_t maxThreads;
        std::string_view tlsCertificateKeyFile;
        boost::optional<std::string_view> tlsCAFile;
        bool tlsAllowConnectionsWithoutCertificates;
        bool tlsAllowInvalidCertificates;
    };

    struct Certificates {
        auto toGRPCConfig() {
            grpc_ssl_pem_key_cert_pair pemKeyCertPair = {keyCertPair.private_key.c_str(),
                                                         keyCertPair.cert_chain.c_str()};

            grpc_ssl_server_certificate_config* config = grpc_ssl_server_certificate_config_create(
                caCert.c_str(), &pemKeyCertPair, /* num_key_cert_pairs */ 1);
            auto deleter = [](grpc_ssl_server_certificate_config* config) {
                grpc_ssl_server_certificate_config_destroy(config);
            };
            return std::unique_ptr<grpc_ssl_server_certificate_config, decltype(deleter)>(config,
                                                                                          deleter);
        }

        std::string caCert;
        ::grpc::SslServerCredentialsOptions::PemKeyCertPair keyCertPair;
    };

    Server(std::vector<std::unique_ptr<Service>> services, Options options);

    ~Server();

    void start();

    /**
     * Return the list of addresses this server is bound to and listening on.
     * This must not be called before the server has been started.
     */
    const std::vector<HostAndPort>& getListeningAddresses() const;

    /**
     * Initiates shutting down of the gRPC server, blocking until all pending RPCs and their
     * associated handlers have been completed.
     */
    void shutdown();

    /**
     * Stop accepting new sessions, but allow existing ones to complete.
     */
    void stopAcceptingRequests();

    bool isRunning() const;

    Status rotateCertificates();

private:
    struct CertificateState {
        Certificates cache;
        bool shouldReload = true;
        std::mutex _mutex;
    };

    static grpc_ssl_certificate_config_reload_status _certificateConfigCallback(
        void* certState, grpc_ssl_server_certificate_config** config);
    std::shared_ptr<::grpc::ServerCredentials> _makeServerCredentialsWithFetcher();

    Options _options;
    mutable std::mutex _mutex;
    std::vector<std::unique_ptr<Service>> _services;
    std::unique_ptr<::grpc::Server> _server;
    std::vector<std::unique_ptr<CertificateState>> _certificateStates;
    bool _shutdown;
};

}  // namespace mongo::transport::grpc
