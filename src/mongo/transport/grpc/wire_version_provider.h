// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/wire_version.h"
#include "mongo/util/modules.h"

namespace mongo::transport::grpc {

class [[MONGO_MOD_PARENT_PRIVATE]] WireVersionProvider {
public:
    WireVersionProvider() = default;
    virtual ~WireVersionProvider() = default;
    WireVersionProvider(const WireVersionProvider&) = delete;
    WireVersionProvider& operator=(const WireVersionProvider&) = delete;

    /**
     * Gets this server's understanding of the minimum maxWireVersion value for all servers in the
     * cluster. This is used to gossip the wire version gRPC clients should use when constructing
     * commands, as per the MongoDB gRPC Protocol.
     *
     * If this server is a mongos, "all servers in the cluster" refers to all the mongoses in the
     * sharded cluster. If this server is a mongod, "all servers in the cluster" refers to that
     * mongod alone, since gRPC clients only support direct connections to mongods and do not
     * support communicating with entire replica sets.
     *
     * Currently, this method just returns this server's maxWireVersion, since the current version
     * is the only server version that supports gRPC. In the future, the provider will be augmented
     * to account for the other servers in the cluster.
     */
    virtual int getClusterMaxWireVersion() const;
};

}  // namespace mongo::transport::grpc
