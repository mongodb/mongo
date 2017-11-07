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
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/keys_collection_manager_sharding.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/sharding_task_executor.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/time_proof_service.h"
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
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/sock.h"

namespace mongo {

using executor::ConnectionPool;

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolHostTimeoutMS,
                                      int,
                                      ConnectionPool::kDefaultHostTimeout.count());
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxSize, int, -1);

// By default, limit us to two concurrent pending connection attempts
// in any one pool. Since pools are currently per-cpu, we still may
// have something like 64 concurrent total connection attempts on a
// modestly sized system. We could set it to one, but that seems too
// restrictive.
MONGO_EXPORT_STARTUP_SERVER_PARAMETER(ShardingTaskExecutorPoolMaxConnecting, int, 2);

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

std::unique_ptr<ShardingCatalogClient> makeCatalogClient(ServiceContext* service,
                                                         StringData distLockProcessId) {
    auto distLockCatalog = stdx::make_unique<DistLockCatalogImpl>();
    auto distLockManager =
        stdx::make_unique<ReplSetDistLockManager>(service,
                                                  distLockProcessId,
                                                  std::move(distLockCatalog),
                                                  ReplSetDistLockManager::kDistLockPingInterval,
                                                  ReplSetDistLockManager::kDistLockExpirationTime);

    return stdx::make_unique<ShardingCatalogClientImpl>(std::move(distLockManager));
}

std::unique_ptr<TaskExecutorPool> makeShardingTaskExecutorPool(
    std::unique_ptr<NetworkInterface> fixedNet,
    rpc::ShardingEgressMetadataHookBuilder metadataHookBuilder,
    ConnectionPool::Options connPoolOptions,
    boost::optional<size_t> taskExecutorPoolSize) {
    std::vector<std::unique_ptr<executor::TaskExecutor>> executors;

    const auto poolSize = taskExecutorPoolSize.value_or(TaskExecutorPool::getSuggestedPoolSize());

    for (size_t i = 0; i < poolSize; ++i) {
        auto exec = makeShardingTaskExecutor(executor::makeNetworkInterface(
            "NetworkInterfaceASIO-TaskExecutorPool-" + std::to_string(i),
            stdx::make_unique<ShardingNetworkConnectionHook>(),
            metadataHookBuilder(),
            connPoolOptions));

        executors.emplace_back(std::move(exec));
    }

    // Add executor used to perform non-performance critical work.
    auto fixedExec = makeShardingTaskExecutor(std::move(fixedNet));

    auto executorPool = stdx::make_unique<TaskExecutorPool>();
    executorPool->addExecutors(std::move(executors), std::move(fixedExec));
    return executorPool;
}

}  // namespace

std::unique_ptr<executor::TaskExecutor> makeShardingTaskExecutor(
    std::unique_ptr<NetworkInterface> net) {
    auto netPtr = net.get();
    auto executor = stdx::make_unique<ThreadPoolTaskExecutor>(
        stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));

    return stdx::make_unique<executor::ShardingTaskExecutor>(std::move(executor));
}

std::string generateDistLockProcessId(OperationContext* opCtx) {
    std::unique_ptr<SecureRandom> rng(SecureRandom::create());

    return str::stream()
        << HostAndPort(getHostName(), serverGlobalParams.port).toString() << ':'
        << durationCount<Seconds>(
               opCtx->getServiceContext()->getPreciseClockSource()->now().toDurationSinceEpoch())
        << ':' << rng->nextInt64();
}

Status initializeGlobalShardingState(OperationContext* opCtx,
                                     const ConnectionString& configCS,
                                     StringData distLockProcessId,
                                     std::unique_ptr<ShardFactory> shardFactory,
                                     std::unique_ptr<CatalogCache> catalogCache,
                                     rpc::ShardingEgressMetadataHookBuilder hookBuilder,
                                     boost::optional<size_t> taskExecutorPoolSize) {
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
    auto executorPool = makeShardingTaskExecutorPool(
        std::move(network), hookBuilder, connPoolOptions, taskExecutorPoolSize);
    executorPool->startup();

    auto const grid = Grid::get(opCtx);
    grid->init(
        makeCatalogClient(opCtx->getServiceContext(), distLockProcessId),
        std::move(catalogCache),
        stdx::make_unique<ShardRegistry>(std::move(shardFactory), configCS),
        stdx::make_unique<ClusterCursorManager>(getGlobalServiceContext()->getPreciseClockSource()),
        stdx::make_unique<BalancerConfiguration>(),
        std::move(executorPool),
        networkPtr);

    // The shard registry must be started once the grid is initialized
    grid->shardRegistry()->startup(opCtx);

    // The catalog client must be started after the shard registry has been started up
    grid->catalogClient()->startup();

    auto keysCollectionClient =
        stdx::make_unique<KeysCollectionClientSharded>(grid->catalogClient());
    auto keyManager = std::make_shared<KeysCollectionManagerSharding>(
        KeysCollectionManager::kKeyManagerPurposeString,
        std::move(keysCollectionClient),
        Seconds(KeysRotationIntervalSec));
    keyManager->startMonitoring(opCtx->getServiceContext());

    LogicalTimeValidator::set(opCtx->getServiceContext(),
                              stdx::make_unique<LogicalTimeValidator>(keyManager));

    auto replCoord = repl::ReplicationCoordinator::get(opCtx->getClient()->getServiceContext());
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer &&
        replCoord->getMemberState().primary()) {
        LogicalTimeValidator::get(opCtx)->enableKeyGenerator(opCtx, true);
    }

    return Status::OK();
}

Status waitForShardRegistryReload(OperationContext* opCtx) {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return Status::OK();
    }

    while (!globalInShutdownDeprecated()) {
        auto stopStatus = opCtx->checkForInterruptNoAssert();
        if (!stopStatus.isOK()) {
            return stopStatus;
        }

        try {
            uassertStatusOK(ClusterIdentityLoader::get(opCtx)->loadClusterId(
                opCtx, repl::ReadConcernLevel::kMajorityReadConcern));
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
