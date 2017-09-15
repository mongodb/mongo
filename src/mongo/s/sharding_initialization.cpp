/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_initialization.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/client/remote_command_targeter_factory_impl.h"
#include "mongo/client/syncclusterconnection.h"
#include "mongo/db/audit.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/catalog/forwarding_catalog_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"

namespace mongo {

using executor::ConnectionPool;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolHostTimeoutMS, int, -1);
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxSize, int, -1);
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxConnecting, int, -1);
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMinSize,
                                      int,
                                      static_cast<int>(ConnectionPool::kDefaultMinConns));
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolRefreshRequirementMS, int, -1);
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolRefreshTimeoutMS, int, -1);

namespace {

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutorPool;
using executor::ThreadPoolTaskExecutor;

// Same logic as sharding_connection_hook.cpp.
class ShardingEgressMetadataHook final : public rpc::EgressMetadataHook {
public:
    Status writeRequestMetadata(const HostAndPort& target, BSONObjBuilder* metadataBob) override {
        try {
            audit::writeImpersonatedUsersToMetadata(metadataBob);

            // Add config server optime to metadata sent to shards.
            auto shard = grid.shardRegistry()->getShardForHostNoReload(target);
            if (!shard) {
                return Status(ErrorCodes::ShardNotFound,
                              str::stream() << "Shard not found for server: " << target.toString());
            }
            if (shard->isConfig()) {
                return Status::OK();
            }
            rpc::ConfigServerMetadata(grid.shardRegistry()->getConfigOpTime())
                .writeToMetadata(metadataBob);

            return Status::OK();
        } catch (...) {
            return exceptionToStatus();
        }
    }

    Status readReplyMetadata(const HostAndPort& replySource, const BSONObj& metadataObj) override {
        try {
            saveGLEStats(metadataObj, replySource.toString());

            auto shard = grid.shardRegistry()->getShardForHostNoReload(replySource);
            if (!shard) {
                return Status::OK();
            }
            // If this host is a known shard of ours, look for a config server optime in the
            // response metadata to use to update our notion of the current config server optime.
            auto responseStatus = rpc::ConfigServerMetadata::readFromMetadata(metadataObj);
            if (!responseStatus.isOK()) {
                return responseStatus.getStatus();
            }
            auto opTime = responseStatus.getValue().getOpTime();
            if (opTime.is_initialized()) {
                grid.shardRegistry()->advanceConfigOpTime(opTime.get());
            }
            return Status::OK();
        } catch (...) {
            return exceptionToStatus();
        }
    }
};

std::unique_ptr<ThreadPoolTaskExecutor> makeTaskExecutor(std::unique_ptr<NetworkInterface> net) {
    auto netPtr = net.get();
    return stdx::make_unique<ThreadPoolTaskExecutor>(
        stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));
}

std::unique_ptr<TaskExecutorPool> makeTaskExecutorPool(std::unique_ptr<NetworkInterface> fixedNet,
                                                       ConnectionPool::Options connPoolOptions) {
    std::vector<std::unique_ptr<executor::TaskExecutor>> executors;
    for (size_t i = 0; i < TaskExecutorPool::getSuggestedPoolSize(); ++i) {
        auto net = executor::makeNetworkInterface(
            "NetworkInterfaceASIO-TaskExecutorPool-" + std::to_string(i),
            stdx::make_unique<ShardingNetworkConnectionHook>(),
            stdx::make_unique<ShardingEgressMetadataHook>(),
            connPoolOptions);
        auto netPtr = net.get();
        auto exec = stdx::make_unique<ThreadPoolTaskExecutor>(
            stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));

        executors.emplace_back(std::move(exec));
    }

    // Add executor used to perform non-performance critical work.
    auto fixedNetPtr = fixedNet.get();
    auto fixedExec = stdx::make_unique<ThreadPoolTaskExecutor>(
        stdx::make_unique<NetworkInterfaceThreadPool>(fixedNetPtr), std::move(fixedNet));

    auto executorPool = stdx::make_unique<TaskExecutorPool>();
    executorPool->addExecutors(std::move(executors), std::move(fixedExec));
    return executorPool;
}

}  // namespace

Status initializeGlobalShardingState(OperationContext* txn,
                                     const ConnectionString& configCS,
                                     bool allowNetworking) {
    SyncClusterConnection::setConnectionValidationHook(
        [](const HostAndPort& target, const executor::RemoteCommandResponse& isMasterReply) {
            return ShardingNetworkConnectionHook::validateHostImpl(target, isMasterReply, true);
        });

    // We don't set the ConnectionPool's static const variables to be the default value in
    // MONGO_EXPORT_STARTUP_SERVER_PARAMETER because it's not guaranteed to be initialized.
    // The following code is a workaround.
    ConnectionPool::Options connPoolOptions;
    connPoolOptions.hostTimeout = (ShardingTaskExecutorPoolHostTimeoutMS != -1)
        ? Milliseconds(ShardingTaskExecutorPoolHostTimeoutMS)
        : ConnectionPool::kDefaultHostTimeout;
    connPoolOptions.maxConnections = (ShardingTaskExecutorPoolMaxSize != -1)
        ? ShardingTaskExecutorPoolMaxSize
        : ConnectionPool::kDefaultMaxConns;
    connPoolOptions.maxConnecting = (ShardingTaskExecutorPoolMaxConnecting != -1)
        ? ShardingTaskExecutorPoolMaxConnecting
        : ConnectionPool::kDefaultMaxConnecting;
    connPoolOptions.minConnections = ShardingTaskExecutorPoolMinSize;
    connPoolOptions.refreshRequirement = (ShardingTaskExecutorPoolRefreshRequirementMS != -1)
        ? Milliseconds(ShardingTaskExecutorPoolRefreshRequirementMS)
        : ConnectionPool::kDefaultRefreshRequirement;
    connPoolOptions.refreshTimeout = (ShardingTaskExecutorPoolRefreshTimeoutMS != -1)
        ? Milliseconds(ShardingTaskExecutorPoolRefreshTimeoutMS)
        : ConnectionPool::kDefaultRefreshTimeout;

    if (connPoolOptions.refreshRequirement <= connPoolOptions.refreshTimeout) {
        auto newRefreshTimeout = connPoolOptions.refreshRequirement - Milliseconds(1);
        warning() << "ShardingTaskExecutorPoolRefreshRequirementMS ("
                  << connPoolOptions.refreshRequirement
                  << ") set below ShardingTaskExecutorPoolRefreshTimeoutMS ("
                  << connPoolOptions.refreshTimeout
                  << "). Adjusting ShardingTaskExecutorPoolRefreshTimeoutMS to "
                  << newRefreshTimeout;
        connPoolOptions.refreshTimeout = newRefreshTimeout;
    }

    if (connPoolOptions.hostTimeout <=
        connPoolOptions.refreshRequirement + connPoolOptions.refreshTimeout) {
        auto newHostTimeout =
            connPoolOptions.refreshRequirement + connPoolOptions.refreshTimeout + Milliseconds(1);
        warning() << "ShardingTaskExecutorPoolHostTimeoutMS (" << connPoolOptions.hostTimeout
                  << ") set below ShardingTaskExecutorPoolRefreshRequirementMS ("
                  << connPoolOptions.refreshRequirement
                  << ") + ShardingTaskExecutorPoolRefreshTimeoutMS ("
                  << connPoolOptions.refreshTimeout
                  << "). Adjusting ShardingTaskExecutorPoolHostTimeoutMS to " << newHostTimeout;
        connPoolOptions.hostTimeout = newHostTimeout;
    }

    auto network =
        executor::makeNetworkInterface("NetworkInterfaceASIO-ShardRegistry",
                                       stdx::make_unique<ShardingNetworkConnectionHook>(),
                                       stdx::make_unique<ShardingEgressMetadataHook>(),
                                       connPoolOptions);
    auto networkPtr = network.get();
    auto shardRegistry(
        stdx::make_unique<ShardRegistry>(stdx::make_unique<RemoteCommandTargeterFactoryImpl>(),
                                         makeTaskExecutorPool(std::move(network), connPoolOptions),
                                         networkPtr,
                                         makeTaskExecutor(executor::makeNetworkInterface(
                                             "NetworkInterfaceASIO-ShardRegistry-TaskExecutor")),
                                         configCS));

    std::unique_ptr<ForwardingCatalogManager> catalogManager =
        stdx::make_unique<ForwardingCatalogManager>(
            getGlobalServiceContext(),
            configCS,
            shardRegistry.get(),
            HostAndPort(getHostName(), serverGlobalParams.port));

    shardRegistry->startup();
    grid.init(std::move(catalogManager),
              std::move(shardRegistry),
              stdx::make_unique<ClusterCursorManager>(getGlobalServiceContext()->getClockSource()));

    while (!inShutdown()) {
        try {
            Status status = grid.catalogManager(txn)->startup(txn, allowNetworking);
            uassertStatusOK(status);

            if (serverGlobalParams.configsvrMode == CatalogManager::ConfigServerMode::NONE) {
                grid.shardRegistry()->reload(txn);
            }
            return Status::OK();
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            if (status == ErrorCodes::ConfigServersInconsistent) {
                // Legacy catalog manager can return ConfigServersInconsistent.  When that happens
                // we should immediately fail initialization.  For all other failures we should
                // retry.
                return status;
            }

            if (status == ErrorCodes::MustUpgrade) {
                return status;
            }

            if (status == ErrorCodes::ReplicaSetNotFound) {
                // ReplicaSetNotFound most likely means we've been waiting for the config replica
                // set to come up for so long that the ReplicaSetMonitor stopped monitoring the set.
                // Rebuild the config shard to force the monitor to resume monitoring the config
                // servers.
                grid.shardRegistry()->rebuildConfigShard();
            }
            log() << "Error initializing sharding state, sleeping for 2 seconds and trying again"
                  << causedBy(status);
            sleepmillis(2000);
            continue;
        }
    }

    return Status::OK();
}

}  // namespace mongo
