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

#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/server_context.h"
#include "mongo/transport/grpc/util.h"

#include <string>

#include <grpcpp/grpcpp.h>

namespace mongo::transport::grpc {

class GRPCServerContext : public ServerContext {
public:
    explicit GRPCServerContext(::grpc::ServerContext* ctx)
        : _ctx{ctx}, _remote{util::parseGRPCFormattedURI(_ctx->peer())} {
        for (auto& kvp : _ctx->client_metadata()) {
            _clientMetadata.insert({StringData{kvp.first.data(), kvp.first.length()},
                                    StringData{kvp.second.data(), kvp.second.length()}});
        }
    }

    ~GRPCServerContext() override = default;

    void addInitialMetadataEntry(const std::string& key, const std::string& value) override {
        _ctx->AddInitialMetadata(key, value);
    }

    const MetadataView& getClientMetadata() const override {
        return _clientMetadata;
    }

    Date_t getDeadline() const override {
        return Date_t{_ctx->deadline()};
    }

    HostAndPort getRemote() const override {
        return _remote;
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
    HostAndPort _remote;
};

}  // namespace mongo::transport::grpc
