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

#include <memory>

#include <grpc/grpc_security.h>
#include <grpcpp/security/server_credentials.h>

#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/service.h"

namespace mongo::transport::grpc {

class Server {
public:
    struct Options {
        /**
         * List of IP addresses, hostnames, and/or Unix domain socket paths to bind to.
         */
        std::vector<HostAndPort> addresses;
        size_t maxThreads;
        StringData tlsCertificateKeyFile;
        boost::optional<StringData> tlsCAFile;
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
        stdx::mutex _mutex;
    };

    static grpc_ssl_certificate_config_reload_status _certificateConfigCallback(
        void* certState, grpc_ssl_server_certificate_config** config);
    std::shared_ptr<::grpc::ServerCredentials> _makeServerCredentialsWithFetcher();

    Options _options;
    mutable stdx::mutex _mutex;
    std::vector<std::unique_ptr<Service>> _services;
    std::unique_ptr<::grpc::Server> _server;
    std::vector<std::unique_ptr<CertificateState>> _certificateStates;
    bool _shutdown;
};

}  // namespace mongo::transport::grpc
