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

/** Options for a NetworkInterface that uses a connection pool. */
struct ConnectionPoolNetworkInterfaceOptions {
    std::unique_ptr<NetworkConnectionHook> connectionHook;
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook;
    ConnectionPool::Options connectionPoolOptions;
    transport::TransportProtocol protocol = transport::TransportProtocol::MongoRPC;
    bool trackRequestCounts = false;
};

/** Returns a new NetworkInterface that uses a connection pool. */
std::unique_ptr<NetworkInterface> makeNetworkInterface(
    std::string_view instanceName, ConnectionPoolNetworkInterfaceOptions options = {});

/** Options supported by generic NetworkInterfaces. */
struct NetworkInterfaceOptions {
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook;
    bool trackRequestCounts = false;
};

#ifdef MONGO_CONFIG_GRPC
/**
 * Returns a new NetworkInterface that uses gRPC as its transport layer.
 * Note that transport::Sessions established by this NetworkInterface do not perform the MongoDB
 * Handshake (e.g. hello/auth) during setup.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterfaceGRPC(std::string_view instanceName,
                                                           NetworkInterfaceOptions options = {});
#endif

/** Returns a new NetworkInterface that uses the provided AsyncClientFactory. Exposed for testing.
 */
std::unique_ptr<NetworkInterface> makeNetworkInterfaceWithClientFactory(
    std::string_view instanceName,
    std::shared_ptr<AsyncClientFactory> clientFactory,
    NetworkInterfaceOptions options = {});

}  // namespace executor
}  // namespace mongo
