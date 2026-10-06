// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard_factory.h"
#include "mongo/db/sharding_environment/client/shard_remote.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_shared_state_cache.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/net/hostandport.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

class ShardingCoordinatorTestCatalogClient final : public ShardingCatalogClientMock {
public:
    explicit ShardingCoordinatorTestCatalogClient(std::vector<ShardType> shards)
        : _shards(std::move(shards)) {}

    repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext*,
                                                          repl::ReadConcernArgs,
                                                          BSONObj) override {
        return repl::OpTimeWith<std::vector<ShardType>>{_shards, repl::OpTime()};
    }

private:
    std::vector<ShardType> _shards;
};

enum class IndexBuildProgress { kNone, kInProgress };

struct IndexBuildCommand {
    std::string host;
    std::string name;
    std::string db;
    std::string coll;
};

class IndexBuildCommandLog {
public:
    void record(IndexBuildCommand cmd) {
        std::lock_guard<std::mutex> lk(_mutex);
        _commands.push_back(std::move(cmd));
    }

    std::vector<IndexBuildCommand> commands() const {
        std::lock_guard<std::mutex> lk(_mutex);
        return _commands;
    }

private:
    mutable std::mutex _mutex;
    std::vector<IndexBuildCommand> _commands;
};

struct IndexBuildCheckOptions {
    IndexBuildProgress indexBuilds = IndexBuildProgress::kNone;
    std::vector<std::string> shardNames{"shard0"};
    std::map<std::string, std::vector<std::string>> collectionsByHost;
    std::map<std::string, std::vector<std::string>> viewsByHost;
    std::set<std::string> namespacesWithIndexBuild;
    std::shared_ptr<IndexBuildCommandLog> commandLog;
};

class IndexBuildCheckAsyncRPCRunner final : public async_rpc::detail::AsyncRPCRunner {
public:
    explicit IndexBuildCheckAsyncRPCRunner(IndexBuildCheckOptions options)
        : _options(std::move(options)) {}

    ExecutorFuture<async_rpc::detail::AsyncRPCInternalResponse> _sendCommand(
        std::shared_ptr<executor::TaskExecutor> exec,
        CancellationToken token,
        OperationContext*,
        async_rpc::Targeter* targeter,
        const TargetingMetadata& targetingMetadata,
        const DatabaseName& dbName,
        BSONObj cmdBSON,
        BatonHandle,
        boost::optional<UUID>) final {
        auto dbFromCommand = std::string{cmdBSON.getStringField("$db")};
        if (dbFromCommand.empty()) {
            dbFromCommand = dbName.serializeWithoutTenantPrefix_UNSAFE();
        }
        return targeter->resolve(token, targetingMetadata)
            .thenRunOn(exec)
            .then([cmdBSON = cmdBSON.getOwned(), options = _options, db = std::move(dbFromCommand)](
                      HostAndPort target) {
                const auto cmdName = std::string{cmdBSON.firstElementFieldName()};
                const auto coll = cmdName == "listIndexes"
                    ? std::string{cmdBSON.getStringField("listIndexes")}
                    : std::string{};
                const auto host = target.host();
                if (options.commandLog) {
                    options.commandLog->record(IndexBuildCommand{host, cmdName, db, coll});
                }

                BSONObjBuilder bob;
                bob.append("ok", 1.0);
                if (cmdName == "listIndexes" || cmdName == "listCollections") {
                    BSONObjBuilder cursor(bob.subobjStart("cursor"));
                    cursor.append("id", 0LL);
                    cursor.append("ns", coll.empty() ? db : db + "." + coll);
                    BSONArrayBuilder batch(cursor.subarrayStart("firstBatch"));
                    if (cmdName == "listIndexes") {
                        batch.append(
                            BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << "_id_"));
                        const auto fullNs = db + "." + coll;
                        const bool inProgress = !options.namespacesWithIndexBuild.empty()
                            ? options.namespacesWithIndexBuild.contains(fullNs)
                            : options.indexBuilds == IndexBuildProgress::kInProgress;
                        if (inProgress) {
                            batch.append(BSON("v" << 2 << "key" << BSON("a" << 1) << "name"
                                                  << "a_1" << "indexBuildInfo"
                                                  << BSON("method" << "Hybrid")));
                        }
                    } else {
                        if (auto it = options.collectionsByHost.find(host);
                            it != options.collectionsByHost.end()) {
                            for (const auto& name : it->second) {
                                batch.append(BSON("name" << name << "type" << "collection"));
                            }
                        }
                        if (auto it = options.viewsByHost.find(host);
                            it != options.viewsByHost.end()) {
                            for (const auto& name : it->second) {
                                batch.append(BSON("name" << name << "type" << "view"));
                            }
                        }
                    }
                }
                async_rpc::detail::AsyncRPCInternalResponse response;
                response.response = bob.obj();
                response.targetUsed = target;
                response.elapsed = Microseconds{1};
                return response;
            });
    }

private:
    IndexBuildCheckOptions _options;
};

class ShardingCoordinatorTestGrid {
public:
    static constexpr auto kShardName = "shard0";

    explicit ShardingCoordinatorTestGrid(OperationContext* opCtx,
                                         IndexBuildProgress indexBuilds = IndexBuildProgress::kNone)
        : ShardingCoordinatorTestGrid(opCtx, IndexBuildCheckOptions{.indexBuilds = indexBuilds}) {}

    explicit ShardingCoordinatorTestGrid(OperationContext* opCtx, IndexBuildCheckOptions options) {
        _serviceContext = opCtx->getServiceContext();

        async_rpc::detail::AsyncRPCRunner::set(
            _serviceContext, std::make_unique<IndexBuildCheckAsyncRPCRunner>(options));

        auto targeterFactory = std::make_unique<RemoteCommandTargeterFactoryMock>();
        std::vector<ShardType> shardTypes;
        shardTypes.reserve(options.shardNames.size());
        for (const auto& shardName : options.shardNames) {
            const auto shardHost = HostAndPort(shardName, 27017);
            const auto shardConnStr = ConnectionString::forReplicaSet(shardName, {shardHost});

            auto targeter = std::make_unique<RemoteCommandTargeterMock>();
            targeter->setConnectionStringReturnValue(shardConnStr);
            targeter->setFindHostReturnValue(shardHost);
            targeterFactory->addTargeterToReturn(shardConnStr, std::move(targeter));

            ShardType shard{shardName, shardConnStr.toString()};
            shard.setTopologyTime(Timestamp(1, 1));
            shardTypes.push_back(std::move(shard));
        }

        const auto configHost = HostAndPort("config", 27019);
        const auto configConnStr = ConnectionString::forReplicaSet("configRS", {configHost});
        auto configTargeter = std::make_unique<RemoteCommandTargeterMock>();
        configTargeter->setConnectionStringReturnValue(configConnStr);
        configTargeter->setFindHostReturnValue(configHost);
        targeterFactory->addTargeterToReturn(configConnStr, std::move(configTargeter));

        auto shardRegistry =
            std::make_shared<ShardRegistry>(_serviceContext,
                                            _makeShardFactory(_serviceContext, targeterFactory),
                                            configConnStr,
                                            std::vector<ShardRegistry::ShardRemovalHook>{});

        auto executorPool = _makeExecutorPool();
        executorPool->startup();

        Grid::get(_serviceContext)
            ->init(std::make_unique<ShardingCoordinatorTestCatalogClient>(std::move(shardTypes)),
                   nullptr,
                   std::move(shardRegistry),
                   nullptr,
                   nullptr,
                   std::move(executorPool),
                   nullptr);

        const auto shardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        invariant(shardIds.size() == options.shardNames.size());
    }

    ~ShardingCoordinatorTestGrid() {
        if (!_serviceContext) {
            return;
        }
        auto* grid = Grid::get(_serviceContext);
        if (grid->isInitialized()) {
            if (auto* pool = grid->getExecutorPool()) {
                pool->shutdownAndJoin();
            }
            if (auto* registry = grid->shardRegistry()) {
                registry->shutdown();
            }
            grid->clearForUnitTests();
        }
    }

    ShardingCoordinatorTestGrid(const ShardingCoordinatorTestGrid&) = delete;
    ShardingCoordinatorTestGrid& operator=(const ShardingCoordinatorTestGrid&) = delete;

private:
    static std::unique_ptr<executor::TaskExecutorPool> _makeExecutorPool() {
        auto makeExecutor = [](std::string name) {
            return executor::ThreadPoolTaskExecutor::create(
                ThreadPool::make({
                    .poolName = name + "Pool",
                    .threadNamePrefix = name + "-",
                    .minThreads = 0,
                    .maxThreads = 2,
                    .onCreateThread =
                        [](const std::string& threadName) {
                            Client::initThread(threadName, getGlobalServiceContext()->getService());
                        },
                }),
                executor::makeNetworkInterface(name));
        };
        auto pool = std::make_unique<executor::TaskExecutorPool>();
        pool->addExecutors({makeExecutor("ShardingCoordinatorTestGrid")},
                           makeExecutor("ShardingCoordinatorTestGridFixed"));
        return pool;
    }

    static std::unique_ptr<ShardFactory> _makeShardFactory(
        ServiceContext* serviceContext,
        std::unique_ptr<RemoteCommandTargeterFactoryMock>& targeterFactory) {
        auto* targeterFactoryPtr = targeterFactory.get();
        ShardFactory::BuilderCallable builder = [targeterFactoryPtr,
                                                 serviceContext](const ShardId& shardId,
                                                                 const ConnectionString& connStr) {
            auto& sharedState = ShardSharedStateCache::get(serviceContext);
            return std::make_unique<ShardRemote>(shardId,
                                                 connStr,
                                                 targeterFactoryPtr->create(connStr),
                                                 sharedState.getShardState(shardId));
        };
        ShardFactory::BuildersMap builders{
            {ConnectionString::ConnectionType::kReplicaSet, builder},
            {ConnectionString::ConnectionType::kStandalone, builder},
        };
        return std::make_unique<ShardFactory>(std::move(builders), std::move(targeterFactory));
    }

    ServiceContext* _serviceContext = nullptr;
};

}  // namespace mongo
