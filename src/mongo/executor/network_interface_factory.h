// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] executor {

/**
 * Returns a new NetworkInterface that uses a connection pool with the default options.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterface(std::string_view instanceName);

/**
 * Returns a new NetworkInterface with the given connection hook set.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterface(
    std::string_view instanceName,
    std::unique_ptr<NetworkConnectionHook> hook,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    ConnectionPool::Options options = ConnectionPool::Options(),
    transport::TransportProtocol protocol = transport::TransportProtocol::MongoRPC,
    bool trackRequestCounts = false);

#ifdef MONGO_CONFIG_GRPC
/**
 * Returns a new NetworkInterface that uses gRPC as its transport layer.
 * Note that transport::Sessions established by this NetworkInterface do not perform the MongoDB
 * Handshake (e.g. hello/auth) during setup.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterfaceGRPC(
    std::string_view instanceName, std::unique_ptr<rpc::EgressMetadataHook> metadataHook = nullptr);
#endif

/**
 * Returns a new NetworkInterface that uses the provided AsyncClientFactory.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterfaceWithClientFactory(
    std::string_view instanceName,
    std::shared_ptr<AsyncClientFactory> clientFactory,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    bool trackRequestCounts = false);

}  // namespace executor
}  // namespace mongo
