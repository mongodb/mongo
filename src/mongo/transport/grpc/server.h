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

#include <grpcpp/server.h>

#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/service.h"

namespace mongo::transport::grpc {

class Server {
public:
    struct Options {
        /**
         * List of IP addresses, hostnames, and/or Unix domain socket paths to bind to.
         */
        std::vector<std::string> addresses;

        int port;
        size_t maxThreads;
        StringData tlsPEMKeyFile;
        boost::optional<StringData> tlsCAFile;
        bool tlsAllowConnectionsWithoutCertificates;
        bool tlsAllowInvalidCertificates;
    };

    Server(std::vector<std::unique_ptr<Service>> services, Options options);

    ~Server();

    void start();

    /**
     * Initiates shutting down of the gRPC server, blocking until all pending RPCs and their
     * associated handlers have been completed.
     */
    void shutdown();

    bool isRunning() const;

private:
    Options _options;
    mutable Mutex _mutex = MONGO_MAKE_LATCH("grpc::Server::_mutex");
    std::vector<std::unique_ptr<Service>> _services;
    std::unique_ptr<::grpc::Server> _server;
    bool _shutdown;
};

}  // namespace mongo::transport::grpc
