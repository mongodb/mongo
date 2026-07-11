// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/grpc/client_context.h"
#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/util.h"

#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

namespace mongo::transport::grpc {

class GRPCClientContext : public ClientContext {
public:
    GRPCClientContext() = default;

    ~GRPCClientContext() override = default;

    void addMetadataEntry(const std::string& key, const std::string& value) override {
        _ctx.AddMetadata(key, value);
    }

    // Because gRPC's GetServerInitialMetadata returns a reference to a map of gRPC reference types,
    // there's no easy way for us to return a map of our wrapper types without constructing such a
    // map ourselves. We construct one in each call here rather than doing it once and caching it to
    // avoid the need for locking or for making this method not thread-safe. This method only needs
    // to be invoked a single time anyways, so the perf concern is minimal.
    //
    // Note that the map cannot be created in the constructor, since the RPC might not have begun at
    // that point.
    MetadataView getServerInitialMetadata() const override {
        MetadataView mv;
        for (auto& kvp : _ctx.GetServerInitialMetadata()) {
            mv.insert({std::string_view{kvp.first.data(), kvp.first.length()},
                       std::string_view{kvp.second.data(), kvp.second.length()}});
        }
        return mv;
    }

    Date_t getDeadline() const override {
        return Date_t{_ctx.deadline()};
    }

    void setDeadline(Date_t deadline) override {
        _ctx.set_deadline(deadline.toSystemTimePoint());
    }

    /**
     * Sets whether stream establishment should wait (up to the deadline) for the underlying channel
     * to become connected or not. If set to false, stream establishment will
     * immediately return an error if the channel is not connected.
     *
     * The default is false.
     *
     * See: https://grpc.github.io/grpc/cpp/md_doc_wait-for-ready.html
     * See: https://github.com/grpc/grpc/blob/master/doc/wait-for-ready.md
     */
    void setWaitForReady(bool wait) {
        _ctx.set_wait_for_ready(wait);
    }

    // Similar to getServerInitialMetadata(), we parse the URI in each invocation of this method to
    // not violate const here or require any locking. This too should only need to be called once,
    // so again the performance impact should be negligible.
    HostAndPort getRemote() const override {
        return util::parseGRPCFormattedURI(_ctx.peer());
    }

    void tryCancel() override {
        _ctx.TryCancel();
    }

    ::grpc::ClientContext* getGRPCClientContext() {
        return &_ctx;
    }

private:
    ::grpc::ClientContext _ctx;
};

}  // namespace mongo::transport::grpc
