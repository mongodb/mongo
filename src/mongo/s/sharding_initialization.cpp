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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_initialization.h"

#include <string>

#include "mongo/base/status.h"
#include "mongo/db/audit.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/executor/async_multicaster.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/dist_lock_catalog_impl.h"
#include "mongo/s/catalog/replset_dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/num_hosts_targeted_metrics.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/grid.h"
#include "mongo/s/pre_warm_connection_pool_impl.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/sharding_task_executor.h"
#include "mongo/s/sharding_task_executor_pool_controller.h"
#include "mongo/s/sharding_task_executor_pool_gen.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

using executor::ConnectionPool;
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

std::shared_ptr<executor::TaskExecutor> makeShardingFixedTaskExecutor(
    std::unique_ptr<NetworkInterface> net) {
    auto executor =
        stdx::make_unique<ThreadPoolTaskExecutor>(stdx::make_unique<ThreadPool>([] {
                                                      ThreadPool::Options opts;
                                                      opts.poolName = "Sharding-Fixed";
                                                      opts.maxThreads =
                                                          ThreadPool::Options::kUnlimited;
                                                      return opts;
                                                  }()),
                                                  std::move(net));

    return std::make_shared<executor::ShardingTaskExecutor>(std::move(executor));
}

std::unique_ptr<TaskExecutorPool> makeShardingTaskExecutorPool(
    std::unique_ptr<NetworkInterface> fixedNet,
    rpc::ShardingEgressMetadataHookBuilder metadataHookBuilder,
    ConnectionPool::Options connPoolOptions,
    boost::optional<size_t> taskExecutorPoolSize) {
    std::vector<std::shared_ptr<executor::TaskExecutor>> executors;

    const auto poolSize = taskExecutorPoolSize.value_or(TaskExecutorPool::getSuggestedPoolSize());

    for (size_t i = 0; i < poolSize; ++i) {
        auto exec = makeShardingTaskExecutor(
            executor::makeNetworkInterface("TaskExecutorPool-" + std::to_string(i),
                                           stdx::make_unique<ShardingNetworkConnectionHook>(),
                                           metadataHookBuilder(),
                                           connPoolOptions));

        executors.emplace_back(std::move(exec));
    }

    // Add executor used to perform non-performance critical work.
    auto fixedExec = makeShardingFixedTaskExecutor(std::move(fixedNet));

    auto executorPool = stdx::make_unique<TaskExecutorPool>();
    executorPool->addExecutors(std::move(executors), std::move(fixedExec));
    return executorPool;
}

/**
 * Uses an AsyncMulticaster to ping all of the hosts in order to establish
 * ShardingTaskExecutorPoolMinSize connections. This does not wait
 * for the connections to be established nor does it check how many were established.
 */
void preWarmConnections(OperationContext* opCtx, std::vector<HostAndPort> allHosts) {
    auto const grid = Grid::get(opCtx);
    auto arbi = grid->getExecutorPool()->getArbitraryExecutor();
    auto executor = executor::ScopedTaskExecutor(arbi);
    executor::AsyncMulticaster::Options options;

    auto results =
        executor::AsyncMulticaster(*executor, options)
            .multicast(allHosts,
                       "admin",
                       BSON("ping" << 1),
                       opCtx,
                       Milliseconds(gWarmMinConnectionsInShardingTaskExecutorPoolOnStartupWaitMS));
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
                                     StringData distLockProcessId,
                                     std::unique_ptr<CatalogCache> catalogCache,
                                     std::unique_ptr<ShardRegistry> shardRegistry,
                                     rpc::ShardingEgressMetadataHookBuilder hookBuilder,
                                     boost::optional<size_t> taskExecutorPoolSize) {
    ConnectionPool::Options connPoolOptions;
    connPoolOptions.controllerFactory = []() noexcept {
        return std::make_shared<ShardingTaskExecutorPoolController>();
    };

    auto network = executor::makeNetworkInterface(
        "ShardRegistry", std::make_unique<ShardingNetworkConnectionHook>(), hookBuilder());
    auto networkPtr = network.get();
    auto executorPool = makeShardingTaskExecutorPool(
        std::move(network), hookBuilder, connPoolOptions, taskExecutorPoolSize);
    executorPool->startup();

    auto& numHostsTargetedMetrics = NumHostsTargetedMetrics::get(opCtx);
    numHostsTargetedMetrics.startup();

    const auto service = opCtx->getServiceContext();
    auto const grid = Grid::get(service);

    grid->init(makeCatalogClient(service, distLockProcessId),
               std::move(catalogCache),
               std::move(shardRegistry),
               stdx::make_unique<ClusterCursorManager>(service->getPreciseClockSource()),
               stdx::make_unique<BalancerConfiguration>(),
               std::move(executorPool),
               networkPtr);

    // The shard registry must be started once the grid is initialized
    grid->shardRegistry()->startup(opCtx);

    // The catalog client must be started after the shard registry has been started up
    grid->catalogClient()->startup();

    auto keysCollectionClient =
        stdx::make_unique<KeysCollectionClientSharded>(grid->catalogClient());
    auto keyManager =
        std::make_shared<KeysCollectionManager>(KeysCollectionManager::kKeyManagerPurposeString,
                                                std::move(keysCollectionClient),
                                                Seconds(KeysRotationIntervalSec));
    keyManager->startMonitoring(service);

    LogicalTimeValidator::set(service, stdx::make_unique<LogicalTimeValidator>(keyManager));

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
            if (Grid::get(opCtx)->shardRegistry()->isUp()) {
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

Status preCacheMongosRoutingInfo(OperationContext* opCtx) {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return Status::OK();
    }
    auto grid = Grid::get(opCtx);

    auto shardingCatalogClient = grid->catalogClient();
    auto result =
        shardingCatalogClient->getAllDBs(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);

    if (!result.isOK()) {
        return result.getStatus();
    }

    auto cache = grid->catalogCache();
    for (auto& db : result.getValue().value) {
        for (auto& coll : shardingCatalogClient->getAllShardedCollectionsForDb(
                 opCtx, db.getName(), repl::ReadConcernLevel::kMajorityReadConcern)) {
            auto resp = cache->getShardedCollectionRoutingInfoWithRefresh(opCtx, coll);
            if (!resp.isOK()) {
                return resp.getStatus();
            }
        }
    }
    return Status::OK();
}

Status preWarmConnectionPool(OperationContext* opCtx) {
    if (!gWarmMinConnectionsInShardingTaskExecutorPoolOnStartup) {
        return Status::OK();
    }

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        log() << "Pre-warming pooled connections for config server is disabled";
        return Status::OK();
    }

    // Should not be called by mongod
    invariant(serverGlobalParams.clusterRole != ClusterRole::ShardServer);

    Timer timer;
    std::vector<HostAndPort> allHosts;
    auto const grid = Grid::get(opCtx);
    auto allShardsStatus =
        grid->catalogClient()->getAllShards(opCtx, repl::ReadConcernLevel::kMajorityReadConcern);
    if (!allShardsStatus.isOK()) {
        return allShardsStatus.getStatus();
    }
    auto allShards = allShardsStatus.getValue().value;

    for (auto& shard : allShards) {
        auto connStrStatus = ConnectionString::parse(shard.getHost());
        if (!connStrStatus.isOK()) {
            return connStrStatus.getStatus();
        }
        auto connStr = connStrStatus.getValue();
        for (const auto& hostEntry : connStr.getServers()) {
            allHosts.push_back(hostEntry);
        }
    }

    if (allHosts.empty()) {
        log() << "No hosts found to pre-warm connections to";
        return Status::OK();
    }
    log() << "Pre-warming connections to " << allHosts.size() << " hosts";

    try {
        opCtx->runWithDeadline(
            opCtx->getServiceContext()->getPreciseClockSource()->now() +
                Milliseconds(gWarmMinConnectionsInShardingTaskExecutorPoolOnStartupWaitMS),
            ErrorCodes::ExceededTimeLimit,
            [&] { preWarmConnections(opCtx, allHosts); });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        // if we've timed out, eat the exception and continue
    } catch (const DBException& ex) {
        warning() << "Connection pool pre-warm failure " << causedBy(ex.toStatus()) << ", timeMs "
                  << timer.millis();
        return ex.toStatus();
    }

    if (timer.millis() > gWarmMinConnectionsInShardingTaskExecutorPoolOnStartupWaitMS / 2) {
        log() << "Slow connection pool pre-warm, timeMs " << timer.millis();
    }
    return Status::OK();
}

}  // namespace mongo
