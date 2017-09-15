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
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/replset_dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/sharding_catalog_manager_impl.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/sharding_egress_metadata_hook.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using executor::ConnectionPool;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolHostTimeoutMS,
                                      int,
                                      ConnectionPool::kDefaultHostTimeout.count());
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxSize, int, -1);
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxConnecting, int, -1);
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMinSize,
                                      int,
                                      static_cast<int>(ConnectionPool::kDefaultMinConns));
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolRefreshRequirementMS,
                                      int,
                                      ConnectionPool::kDefaultRefreshRequirement.count());
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolRefreshTimeoutMS,
                                      int,
                                      ConnectionPool::kDefaultRefreshTimeout.count());

namespace {

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutorPool;
using executor::ThreadPoolTaskExecutor;

static constexpr auto kRetryInterval = Seconds{2};

std::unique_ptr<ThreadPoolTaskExecutor> makeTaskExecutor(std::unique_ptr<NetworkInterface> net) {
    auto netPtr = net.get();
    return stdx::make_unique<ThreadPoolTaskExecutor>(
        stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));
}

std::unique_ptr<ShardingCatalogClient> makeCatalogClient(ServiceContext* service,
                                                         ShardRegistry* shardRegistry,
                                                         StringData distLockProcessId) {
    auto distLockCatalog = stdx::make_unique<DistLockCatalogImpl>(shardRegistry);
    auto distLockManager =
        stdx::make_unique<ReplSetDistLockManager>(service,
                                                  distLockProcessId,
                                                  std::move(distLockCatalog),
                                                  ReplSetDistLockManager::kDistLockPingInterval,
                                                  ReplSetDistLockManager::kDistLockExpirationTime);

    return stdx::make_unique<ShardingCatalogClientImpl>(std::move(distLockManager));
}

std::unique_ptr<TaskExecutorPool> makeTaskExecutorPool(
    std::unique_ptr<NetworkInterface> fixedNet,
    rpc::ShardingEgressMetadataHookBuilder metadataHookBuilder,
    ConnectionPool::Options connPoolOptions) {
    std::vector<std::unique_ptr<executor::TaskExecutor>> executors;

    for (size_t i = 0; i < TaskExecutorPool::getSuggestedPoolSize(); ++i) {
        auto net = executor::makeNetworkInterface(
            "NetworkInterfaceASIO-TaskExecutorPool-" + std::to_string(i),
            stdx::make_unique<ShardingNetworkConnectionHook>(),
            metadataHookBuilder(),
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

const StringData kDistLockProcessIdForConfigServer("ConfigServer");

std::string generateDistLockProcessId(OperationContext* txn) {
    std::unique_ptr<SecureRandom> rng(SecureRandom::create());

    return str::stream()
        << HostAndPort(getHostName(), serverGlobalParams.port).toString() << ':'
        << durationCount<Seconds>(
               txn->getServiceContext()->getPreciseClockSource()->now().toDurationSinceEpoch())
        << ':' << rng->nextInt64();
}

Status initializeGlobalShardingState(OperationContext* txn,
                                     const ConnectionString& configCS,
                                     StringData distLockProcessId,
                                     std::unique_ptr<ShardFactory> shardFactory,
                                     rpc::ShardingEgressMetadataHookBuilder hookBuilder,
                                     ShardingCatalogManagerBuilder catalogManagerBuilder) {
    if (configCS.type() == ConnectionString::INVALID) {
        return {ErrorCodes::BadValue, "Unrecognized connection string."};
    }

    // We don't set the ConnectionPool's static const variables to be the default value in
    // MONGO_EXPORT_STARTUP_SERVER_PARAMETER because it's not guaranteed to be initialized.
    // The following code is a workaround.
    ConnectionPool::Options connPoolOptions;
    connPoolOptions.hostTimeout = Milliseconds(ShardingTaskExecutorPoolHostTimeoutMS);
    connPoolOptions.maxConnections = (ShardingTaskExecutorPoolMaxSize != -1)
        ? ShardingTaskExecutorPoolMaxSize
        : ConnectionPool::kDefaultMaxConns;
    connPoolOptions.maxConnecting = (ShardingTaskExecutorPoolMaxConnecting != -1)
        ? ShardingTaskExecutorPoolMaxConnecting
        : ConnectionPool::kDefaultMaxConnecting;
    connPoolOptions.minConnections = ShardingTaskExecutorPoolMinSize;
    connPoolOptions.refreshRequirement = Milliseconds(ShardingTaskExecutorPoolRefreshRequirementMS);
    connPoolOptions.refreshTimeout = Milliseconds(ShardingTaskExecutorPoolRefreshTimeoutMS);

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
                                       hookBuilder(),
                                       connPoolOptions);
    auto networkPtr = network.get();
    auto executorPool = makeTaskExecutorPool(std::move(network), hookBuilder, connPoolOptions);
    executorPool->startup();

    auto shardRegistry(stdx::make_unique<ShardRegistry>(std::move(shardFactory), configCS));

    auto catalogClient =
        makeCatalogClient(txn->getServiceContext(), shardRegistry.get(), distLockProcessId);

    auto rawCatalogClient = catalogClient.get();

    std::unique_ptr<ShardingCatalogManager> catalogManager = catalogManagerBuilder(
        rawCatalogClient,
        makeTaskExecutor(executor::makeNetworkInterface("AddShard-TaskExecutor")));
    auto rawCatalogManager = catalogManager.get();

    grid.init(
        std::move(catalogClient),
        std::move(catalogManager),
        stdx::make_unique<CatalogCache>(),
        std::move(shardRegistry),
        stdx::make_unique<ClusterCursorManager>(getGlobalServiceContext()->getPreciseClockSource()),
        stdx::make_unique<BalancerConfiguration>(),
        std::move(executorPool),
        networkPtr);

    // must be started once the grid is initialized
    grid.shardRegistry()->startup();

    auto status = rawCatalogClient->startup();
    if (!status.isOK()) {
        return status;
    }

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        // Only config servers get a ShardingCatalogManager.
        status = rawCatalogManager->startup();
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

Status reloadShardRegistryUntilSuccess(OperationContext* txn) {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return Status::OK();
    }

    while (!inShutdown()) {
        auto stopStatus = txn->checkForInterruptNoAssert();
        if (!stopStatus.isOK()) {
            return stopStatus;
        }

        try {
            uassertStatusOK(ClusterIdentityLoader::get(txn)->loadClusterId(
                txn, repl::ReadConcernLevel::kMajorityReadConcern));
            if (grid.shardRegistry()->isUp()) {
                return Status::OK();
            }
            sleepFor(kRetryInterval);
            continue;
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            warning()
                << "Error initializing sharding state, sleeping for 2 seconds and trying again"
                << causedBy(status);
            sleepFor(kRetryInterval);
            continue;
        }
    }

    return {ErrorCodes::ShutdownInProgress, "aborting shard loading attempt"};
}

}  // namespace mongo
