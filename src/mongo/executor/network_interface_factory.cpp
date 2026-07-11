// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/network_interface_factory.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/config.h"     // IWYU pragma: keep
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/egress_connection_closer_manager.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_tl.h"
#include "mongo/executor/pooled_async_client_factory.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#ifdef MONGO_CONFIG_GRPC
#include "mongo/transport/grpc/async_client_factory.h"
#endif

namespace mongo {
namespace executor {

std::string makeInstanceName(std::string_view name) {
    return fmt::format("NetworkInterfaceTL-{}", name);
}

std::unique_ptr<NetworkInterface> makeNetworkInterface(std::string_view instanceName) {
    return makeNetworkInterface(instanceName, nullptr, nullptr);
}

std::unique_ptr<NetworkInterface> makeNetworkInterface(
    std::string_view instanceName,
    std::unique_ptr<NetworkConnectionHook> hook,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    ConnectionPool::Options connPoolOptions,
    transport::TransportProtocol protocol,
    bool trackRequestCounts) {
    // instanceName flows into PooledAsyncClientFactory::_name which is sent as the
    // applicationName in the hello handshake for every connection this interface opens.
    // An empty name would make those connections invisible to server-side policies that
    // identify internal clients by appName (e.g. ingress rate limiting exemptions).
    dassert(!instanceName.empty(), "makeNetworkInterface requires a non-empty instanceName");

    if (!connPoolOptions.egressConnectionCloserManager && hasGlobalServiceContext()) {
        connPoolOptions.egressConnectionCloserManager =
            &EgressConnectionCloserManager::get(getGlobalServiceContext());
    }

    return makeNetworkInterfaceWithClientFactory(
        instanceName,
        std::make_shared<PooledAsyncClientFactory>(
            makeInstanceName(instanceName), std::move(connPoolOptions), std::move(hook), protocol),
        std::move(metadataHook),
        trackRequestCounts);
}

#ifdef MONGO_CONFIG_GRPC
std::unique_ptr<NetworkInterface> makeNetworkInterfaceGRPC(
    std::string_view instanceName, std::unique_ptr<rpc::EgressMetadataHook> metadataHook) {
    return makeNetworkInterfaceWithClientFactory(
        instanceName,
        std::make_shared<transport::grpc::GRPCAsyncClientFactory>(makeInstanceName(instanceName)),
        std::move(metadataHook));
}
#endif

std::unique_ptr<NetworkInterface> makeNetworkInterfaceWithClientFactory(
    std::string_view instanceName,
    std::shared_ptr<AsyncClientFactory> clientFactory,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    bool trackRequestCounts) {
    return std::make_unique<NetworkInterfaceTL>(makeInstanceName(instanceName),
                                                std::move(clientFactory),
                                                std::move(metadataHook),
                                                trackRequestCounts);
}

}  // namespace executor
}  // namespace mongo
