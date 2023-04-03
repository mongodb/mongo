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

#include <string>

#include <grpcpp/grpcpp.h>

#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/server_context.h"

namespace mongo::transport::grpc {

class GRPCServerContext : public ServerContext {
public:
    /**
     * Parses a gRPC-formatted URI to a HostAndPort, throwing an exception on failure.
     * See: https://grpc.github.io/grpc/cpp/md_doc_naming.html
     */
    static HostAndPort parseURI(const std::string& uri) {
        StringData sd{uri};
        if (auto firstColon = sd.find(':'); firstColon != std::string::npos) {
            sd = sd.substr(firstColon + 1);
        }
        return HostAndPort::parseThrowing(sd);
    }

    explicit GRPCServerContext(::grpc::ServerContext* ctx)
        : _ctx{ctx}, _hostAndPort{parseURI(_ctx->peer())} {
        for (auto& kvp : _ctx->client_metadata()) {
            _clientMetadata.insert({std::string_view{kvp.first.data(), kvp.first.length()},
                                    std::string_view{kvp.second.data(), kvp.second.length()}});
        }
    }

    ~GRPCServerContext() = default;

    void addInitialMetadataEntry(const std::string& key, const std::string& value) override {
        _ctx->AddInitialMetadata(key, value);
    }

    const MetadataView& getClientMetadata() const override {
        return _clientMetadata;
    }

    Date_t getDeadline() const override {
        return Date_t{_ctx->deadline()};
    }

    HostAndPort getHostAndPort() const override {
        return _hostAndPort;
    }

    bool isCancelled() const override {
        return _ctx->IsCancelled();
    }

    void tryCancel() override {
        _ctx->TryCancel();
    }

private:
    ::grpc::ServerContext* _ctx;
    MetadataView _clientMetadata;
    HostAndPort _hostAndPort;
};

}  // namespace mongo::transport::grpc
