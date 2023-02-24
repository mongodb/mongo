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

#include "mongo/db/wire_version.h"

namespace mongo::transport::grpc {

class WireVersionProvider {
public:
    WireVersionProvider() = default;
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
    int getClusterMaxWireVersion() const;
};

}  // namespace mongo::transport::grpc
