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
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/cluster_last_error_info.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_egress_metadata_hook_for_mongos.h"
#include "mongo/db/s/sharding_egress_metadata_hook_for_mongod.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutorPool;
using executor::ThreadPoolTaskExecutor;

std::unique_ptr<CatalogManager> makeCatalogManager(ServiceContext* service,
                                                   ShardRegistry* shardRegistry,
                                                   const HostAndPort& thisHost) {
    std::unique_ptr<SecureRandom> rng(SecureRandom::create());
    std::string distLockProcessId = str::stream()
        << thisHost.toString() << ':'
        << durationCount<Seconds>(service->getPreciseClockSource()->now().toDurationSinceEpoch())
        << ':' << static_cast<int32_t>(rng->nextInt64());

    auto distLockCatalog = stdx::make_unique<DistLockCatalogImpl>(shardRegistry);
    auto distLockManager =
        stdx::make_unique<ReplSetDistLockManager>(service,
                                                  distLockProcessId,
                                                  std::move(distLockCatalog),
                                                  ReplSetDistLockManager::kDistLockPingInterval,
                                                  ReplSetDistLockManager::kDistLockExpirationTime);

    return stdx::make_unique<CatalogManagerReplicaSet>(std::move(distLockManager));
}

std::unique_ptr<ThreadPoolTaskExecutor> makeTaskExecutor(std::unique_ptr<NetworkInterface> net) {
    auto netPtr = net.get();
    return stdx::make_unique<ThreadPoolTaskExecutor>(
        stdx::make_unique<NetworkInterfaceThreadPool>(netPtr), std::move(net));
}

std::unique_ptr<TaskExecutorPool> makeTaskExecutorPool(std::unique_ptr<NetworkInterface> fixedNet,
                                                       bool isMongos) {
    std::vector<std::unique_ptr<executor::TaskExecutor>> executors;
    for (size_t i = 0; i < TaskExecutorPool::getSuggestedPoolSize(); ++i) {
        std::unique_ptr<rpc::EgressMetadataHook> metadataHook;
        if (isMongos) {
            metadataHook = stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>();
        } else {
            metadataHook = stdx::make_unique<rpc::ShardingEgressMetadataHookForMongod>();
        };
        auto net = executor::makeNetworkInterface(
            "NetworkInterfaceASIO-TaskExecutorPool-" + std::to_string(i),
            stdx::make_unique<ShardingNetworkConnectionHook>(),
            std::move(metadataHook));
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

Status initializeGlobalShardingState(OperationContext* txn,
                                     const ConnectionString& configCS,
                                     bool isMongos) {
    if (configCS.type() == ConnectionString::INVALID) {
        return {ErrorCodes::BadValue, "Unrecognized connection string."};
    }

    std::unique_ptr<rpc::EgressMetadataHook> metadataHook;
    if (isMongos) {
        metadataHook = stdx::make_unique<rpc::ShardingEgressMetadataHookForMongos>();
    } else {
        metadataHook = stdx::make_unique<rpc::ShardingEgressMetadataHookForMongod>();
    }

    auto network =
        executor::makeNetworkInterface("NetworkInterfaceASIO-ShardRegistry",
                                       stdx::make_unique<ShardingNetworkConnectionHook>(),
                                       std::move(metadataHook));
    auto networkPtr = network.get();
    auto shardRegistry(
        stdx::make_unique<ShardRegistry>(stdx::make_unique<RemoteCommandTargeterFactoryImpl>(),
                                         makeTaskExecutorPool(std::move(network), isMongos),
                                         networkPtr,
                                         makeTaskExecutor(executor::makeNetworkInterface(
                                             "NetworkInterfaceASIO-ShardRegistry-TaskExecutor")),
                                         configCS));

    auto catalogManager = makeCatalogManager(getGlobalServiceContext(),
                                             shardRegistry.get(),
                                             HostAndPort(getHostName(), serverGlobalParams.port));

    shardRegistry->startup();
    grid.init(std::move(catalogManager),
              std::move(shardRegistry),
              stdx::make_unique<ClusterCursorManager>(
                  getGlobalServiceContext()->getPreciseClockSource()));

    while (!inShutdown()) {
        try {
            Status status = grid.catalogManager(txn)->startup(txn);
            uassertStatusOK(status);

            if (serverGlobalParams.configsvrMode == CatalogManager::ConfigServerMode::NONE) {
                grid.shardRegistry()->reload(txn);
            }
            return Status::OK();
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
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

}  // namespace

Status initializeGlobalShardingStateForMongos(OperationContext* txn,
                                              const ConnectionString& configCS) {
    return initializeGlobalShardingState(txn, configCS, true);
}

Status initializeGlobalShardingStateForMongod(OperationContext* txn,
                                              const ConnectionString& configCS) {
    return initializeGlobalShardingState(txn, configCS, false);
}

}  // namespace mongo
