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

#include "mongo/transport/grpc/client_context.h"

#include <string>

#include <grpcpp/grpcpp.h>

#include "mongo/transport/grpc/metadata.h"
#include "mongo/transport/grpc/util.h"

namespace mongo::transport::grpc {

class GRPCClientContext : public ClientContext {
public:
    GRPCClientContext() = default;

    ~GRPCClientContext() = default;

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
            mv.insert({StringData{kvp.first.data(), kvp.first.length()},
                       StringData{kvp.second.data(), kvp.second.length()}});
        }
        return mv;
    }

    Date_t getDeadline() const override {
        return Date_t{_ctx.deadline()};
    }

    void setDeadline(Date_t deadline) override {
        _ctx.set_deadline(deadline.toSystemTimePoint());
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
