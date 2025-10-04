/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/transport/transport_layer.h"

#include <memory>
#include <string>

namespace mongo {
namespace executor {

/**
 * Returns a new NetworkInterface that uses a connection pool with the default options.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterface(StringData instanceName);

/**
 * Returns a new NetworkInterface with the given connection hook set.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterface(
    StringData instanceName,
    std::unique_ptr<NetworkConnectionHook> hook,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    ConnectionPool::Options options = ConnectionPool::Options(),
    transport::TransportProtocol protocol = transport::TransportProtocol::MongoRPC);

#ifdef MONGO_CONFIG_GRPC
/**
 * Returns a new NetworkInterface that uses gRPC as its transport layer.
 * Note that transport::Sessions established by this NetworkInterface do not perform the MongoDB
 * Handshake (e.g. hello/auth) during setup.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterfaceGRPC(
    StringData instanceName, std::unique_ptr<rpc::EgressMetadataHook> metadataHook = nullptr);
#endif

/**
 * Returns a new NetworkInterface that uses the provided AsyncClientFactory.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterfaceWithClientFactory(
    StringData instanceName,
    std::shared_ptr<AsyncClientFactory> clientFactory,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook = nullptr);

}  // namespace executor
}  // namespace mongo
