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

#include <memory>
#include <utility>

#include <fmt/format.h>

#ifdef MONGO_CONFIG_GRPC
#include "mongo/transport/grpc/async_client_factory.h"
#endif

namespace mongo {
namespace executor {

std::string makeInstanceName(StringData name) {
    return fmt::format("NetworkInterfaceTL-{}", name);
}

std::unique_ptr<NetworkInterface> makeNetworkInterface(StringData instanceName) {
    return makeNetworkInterface(instanceName, nullptr, nullptr);
}

std::unique_ptr<NetworkInterface> makeNetworkInterface(
    StringData instanceName,
    std::unique_ptr<NetworkConnectionHook> hook,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook,
    ConnectionPool::Options connPoolOptions,
    transport::TransportProtocol protocol) {

    if (!connPoolOptions.egressConnectionCloserManager && hasGlobalServiceContext()) {
        connPoolOptions.egressConnectionCloserManager =
            &EgressConnectionCloserManager::get(getGlobalServiceContext());
    }

    return makeNetworkInterfaceWithClientFactory(
        instanceName,
        std::make_shared<PooledAsyncClientFactory>(
            makeInstanceName(instanceName), std::move(connPoolOptions), std::move(hook), protocol),
        std::move(metadataHook));
}

#ifdef MONGO_CONFIG_GRPC
std::unique_ptr<NetworkInterface> makeNetworkInterfaceGRPC(
    StringData instanceName, std::unique_ptr<rpc::EgressMetadataHook> metadataHook) {
    return makeNetworkInterfaceWithClientFactory(
        instanceName,
        std::make_shared<transport::grpc::GRPCAsyncClientFactory>(makeInstanceName(instanceName)),
        std::move(metadataHook));
}
#endif

std::unique_ptr<NetworkInterface> makeNetworkInterfaceWithClientFactory(
    StringData instanceName,
    std::shared_ptr<AsyncClientFactory> clientFactory,
    std::unique_ptr<rpc::EgressMetadataHook> metadataHook) {
    return std::make_unique<NetworkInterfaceTL>(
        makeInstanceName(instanceName), std::move(clientFactory), std::move(metadataHook));
}

}  // namespace executor
}  // namespace mongo
