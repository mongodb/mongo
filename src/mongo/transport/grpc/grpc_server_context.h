// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/server_context.h"
#include "mongo/transport/grpc/util.h"

#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

namespace mongo::transport::grpc {

class GRPCServerContext : public ServerContext {
public:
    explicit GRPCServerContext(::grpc::ServerContext* ctx)
        : _ctx{ctx}, _remote{util::parseGRPCFormattedURI(_ctx->peer())} {
        for (auto& kvp : _ctx->client_metadata()) {
            _clientMetadata.insert({std::string_view{kvp.first.data(), kvp.first.length()},
                                    std::string_view{kvp.second.data(), kvp.second.length()}});
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
