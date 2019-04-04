/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/s/sharding_merizod_test_fixture.h"

#include <algorithm>
#include <vector>

#include "merizo/base/checked_cast.h"
#include "merizo/base/status_with.h"
#include "merizo/client/remote_command_targeter_factory_mock.h"
#include "merizo/client/remote_command_targeter_mock.h"
#include "merizo/client/replica_set_monitor.h"
#include "merizo/db/catalog_raii.h"
#include "merizo/db/client.h"
#include "merizo/db/commands.h"
#include "merizo/db/namespace_string.h"
#include "merizo/db/op_observer_registry.h"
#include "merizo/db/query/cursor_response.h"
#include "merizo/db/query/query_request.h"
#include "merizo/db/repl/drop_pending_collection_reaper.h"
#include "merizo/db/repl/oplog.h"
#include "merizo/db/repl/read_concern_args.h"
#include "merizo/db/repl/repl_settings.h"
#include "merizo/db/repl/replication_consistency_markers_mock.h"
#include "merizo/db/repl/replication_process.h"
#include "merizo/db/repl/replication_recovery_mock.h"
#include "merizo/db/repl/storage_interface_mock.h"
#include "merizo/db/s/config_server_op_observer.h"
#include "merizo/db/s/op_observer_sharding_impl.h"
#include "merizo/db/s/shard_server_op_observer.h"
#include "merizo/executor/network_interface_mock.h"
#include "merizo/executor/task_executor_pool.h"
#include "merizo/executor/thread_pool_task_executor_test_fixture.h"
#include "merizo/rpc/metadata/repl_set_metadata.h"
#include "merizo/s/balancer_configuration.h"
#include "merizo/s/catalog/dist_lock_catalog.h"
#include "merizo/s/catalog/dist_lock_manager.h"
#include "merizo/s/catalog/sharding_catalog_client.h"
#include "merizo/s/catalog/type_changelog.h"
#include "merizo/s/catalog/type_collection.h"
#include "merizo/s/catalog/type_shard.h"
#include "merizo/s/catalog_cache.h"
#include "merizo/s/catalog_cache_loader.h"
#include "merizo/s/client/shard_factory.h"
#include "merizo/s/client/shard_local.h"
#include "merizo/s/client/shard_registry.h"
#include "merizo/s/client/shard_remote.h"
#include "merizo/s/grid.h"
#include "merizo/s/query/cluster_cursor_manager.h"
#include "merizo/s/request_types/set_shard_version_request.h"
#include "merizo/stdx/memory.h"
#include "merizo/util/clock_source_mock.h"
#include "merizo/util/tick_source_mock.h"

namespace merizo {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using repl::ReplicationCoordinator;
using repl::ReplicationCoordinatorMock;
using repl::ReplSettings;
using unittest::assertGet;

ShardingMongodTestFixture::ShardingMongodTestFixture() = default;

ShardingMongodTestFixture::~ShardingMongodTestFixture() = default;

void ShardingMongodTestFixture::setUp() {
    ServiceContextMongoDTest::setUp();

    const auto service = getServiceContext();
    _opCtx = makeOperationContext();

    // Set up this node as part of a replica set.

    repl::ReplSettings replSettings;
    replSettings.setOplogSizeBytes(512'000);
    replSettings.setReplSetString(ConnectionString::forReplicaSet(_setName, _servers).toString());
    auto replCoordPtr = makeReplicationCoordinator(replSettings);
    _replCoord = replCoordPtr.get();

    BSONArrayBuilder serversBob;
    for (size_t i = 0; i < _servers.size(); ++i) {
        serversBob.append(BSON("host" << _servers[i].toString() << "_id" << static_cast<int>(i)));
    }
    repl::ReplSetConfig replSetConfig;
    ASSERT_OK(replSetConfig.initialize(
        BSON("_id" << _setName << "protocolVersion" << 1 << "version" << 3 << "members"
                   << serversBob.arr())));
    replCoordPtr->setGetConfigReturnValue(replSetConfig);

    repl::ReplicationCoordinator::set(service, std::move(replCoordPtr));

    auto storagePtr = stdx::make_unique<repl::StorageInterfaceMock>();

    repl::DropPendingCollectionReaper::set(
        service, stdx::make_unique<repl::DropPendingCollectionReaper>(storagePtr.get()));

    repl::ReplicationProcess::set(service,
                                  stdx::make_unique<repl::ReplicationProcess>(
                                      storagePtr.get(),
                                      stdx::make_unique<repl::ReplicationConsistencyMarkersMock>(),
                                      stdx::make_unique<repl::ReplicationRecoveryMock>()));

    ASSERT_OK(repl::ReplicationProcess::get(_opCtx.get())->initializeRollbackID(_opCtx.get()));

    repl::StorageInterface::set(service, std::move(storagePtr));

    auto opObserver = checked_cast<OpObserverRegistry*>(service->getOpObserver());
    opObserver->addObserver(stdx::make_unique<OpObserverShardingImpl>());
    opObserver->addObserver(stdx::make_unique<ConfigServerOpObserver>());
    opObserver->addObserver(stdx::make_unique<ShardServerOpObserver>());

    repl::setOplogCollectionName(service);
    repl::createOplog(_opCtx.get());

    // Set the highest FCV because otherwise it defaults to the lower FCV. This way we default to
    // testing this release's code, not backwards compatibility code.
    serverGlobalParams.featureCompatibility.setVersion(
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42);
}

std::unique_ptr<ReplicationCoordinatorMock> ShardingMongodTestFixture::makeReplicationCoordinator(
    ReplSettings replSettings) {
    auto coordinator =
        stdx::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(), replSettings);
    ASSERT_OK(coordinator->setFollowerMode(repl::MemberState::RS_PRIMARY));
    return coordinator;
}

std::unique_ptr<executor::TaskExecutorPool> ShardingMongodTestFixture::makeTaskExecutorPool() {
    // Set up a NetworkInterfaceMock. Note, unlike NetworkInterfaceASIO, which has its own pool of
    // threads, tasks in the NetworkInterfaceMock must be carried out synchronously by the (single)
    // thread the unit test is running on.
    auto netForFixedTaskExecutor = stdx::make_unique<executor::NetworkInterfaceMock>();
    _mockNetwork = netForFixedTaskExecutor.get();

    // Set up a ThreadPoolTaskExecutor. Note, for local tasks this TaskExecutor uses a
    // ThreadPoolMock, and for remote tasks it uses the NetworkInterfaceMock created above. However,
    // note that the ThreadPoolMock uses the NetworkInterfaceMock's threads to run tasks, which is
    // again just the (single) thread the unit test is running on. Therefore, all tasks, local and
    // remote, must be carried out synchronously by the test thread.
    auto fixedTaskExecutor = makeThreadPoolTestExecutor(std::move(netForFixedTaskExecutor));
    _networkTestEnv = stdx::make_unique<NetworkTestEnv>(fixedTaskExecutor.get(), _mockNetwork);

    // Set up (one) TaskExecutor for the set of arbitrary TaskExecutors.
    std::vector<std::unique_ptr<executor::TaskExecutor>> arbitraryExecutorsForExecutorPool;
    arbitraryExecutorsForExecutorPool.emplace_back(
        makeThreadPoolTestExecutor(stdx::make_unique<executor::NetworkInterfaceMock>()));

    // Set up the TaskExecutorPool with the fixed TaskExecutor and set of arbitrary TaskExecutors.
    auto executorPool = stdx::make_unique<executor::TaskExecutorPool>();
    executorPool->addExecutors(std::move(arbitraryExecutorsForExecutorPool),
                               std::move(fixedTaskExecutor));

    return executorPool;
}

std::unique_ptr<ShardRegistry> ShardingMongodTestFixture::makeShardRegistry(
    ConnectionString configConnStr) {
    auto targeterFactory(stdx::make_unique<RemoteCommandTargeterFactoryMock>());
    auto targeterFactoryPtr = targeterFactory.get();
    _targeterFactory = targeterFactoryPtr;

    ShardFactory::BuilderCallable setBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuilderCallable masterBuilder =
        [targeterFactoryPtr](const ShardId& shardId, const ConnectionString& connStr) {
            return stdx::make_unique<ShardRemote>(
                shardId, connStr, targeterFactoryPtr->create(connStr));
        };

    ShardFactory::BuildersMap buildersMap{{ConnectionString::SET, std::move(setBuilder)},
                                          {ConnectionString::MASTER, std::move(masterBuilder)}};

    // Only config servers use ShardLocal for now.
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        ShardFactory::BuilderCallable localBuilder = [](const ShardId& shardId,
                                                        const ConnectionString& connStr) {
            return stdx::make_unique<ShardLocal>(shardId);
        };
        buildersMap.insert(
            std::pair<ConnectionString::ConnectionType, ShardFactory::BuilderCallable>(
                ConnectionString::LOCAL, std::move(localBuilder)));
    }

    auto shardFactory =
        stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    return stdx::make_unique<ShardRegistry>(std::move(shardFactory), configConnStr);
}

std::unique_ptr<DistLockCatalog> ShardingMongodTestFixture::makeDistLockCatalog() {
    return nullptr;
}

std::unique_ptr<DistLockManager> ShardingMongodTestFixture::makeDistLockManager(
    std::unique_ptr<DistLockCatalog> distLockCatalog) {
    return nullptr;
}

std::unique_ptr<ShardingCatalogClient> ShardingMongodTestFixture::makeShardingCatalogClient(
    std::unique_ptr<DistLockManager> distLockManager) {
    return nullptr;
}

std::unique_ptr<ClusterCursorManager> ShardingMongodTestFixture::makeClusterCursorManager() {
    return nullptr;
}

std::unique_ptr<BalancerConfiguration> ShardingMongodTestFixture::makeBalancerConfiguration() {
    return nullptr;
}

Status ShardingMongodTestFixture::initializeGlobalShardingStateForMongodForTest(
    const ConnectionString& configConnStr) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
              serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

    // Create and initialize each sharding component individually before moving them to the Grid
    // in order to control the order of initialization, since some components depend on others.

    auto executorPoolPtr = makeTaskExecutorPool();
    if (executorPoolPtr) {
        executorPoolPtr->startup();
    }

    auto distLockCatalogPtr = makeDistLockCatalog();
    _distLockCatalog = distLockCatalogPtr.get();

    auto distLockManagerPtr = makeDistLockManager(std::move(distLockCatalogPtr));
    _distLockManager = distLockManagerPtr.get();

    auto const grid = Grid::get(operationContext());
    grid->init(makeShardingCatalogClient(std::move(distLockManagerPtr)),
               stdx::make_unique<CatalogCache>(CatalogCacheLoader::get(getServiceContext())),
               makeShardRegistry(configConnStr),
               makeClusterCursorManager(),
               makeBalancerConfiguration(),
               std::move(executorPoolPtr),
               _mockNetwork);

    // NOTE: ShardRegistry::startup() is not called because it starts a task executor with a
    // self-rescheduling task to reload the ShardRegistry over the network.
    // grid->shardRegistry()->startup();

    if (grid->catalogClient()) {
        grid->catalogClient()->startup();
    }

    return Status::OK();
}

void ShardingMongodTestFixture::tearDown() {
    ReplicaSetMonitor::cleanup();

    if (Grid::get(operationContext())->getExecutorPool() && !_executorPoolShutDown) {
        Grid::get(operationContext())->getExecutorPool()->shutdownAndJoin();
    }

    if (Grid::get(operationContext())->catalogClient()) {
        Grid::get(operationContext())->catalogClient()->shutDown(operationContext());
    }

    if (Grid::get(operationContext())->shardRegistry()) {
        Grid::get(operationContext())->shardRegistry()->shutdown();
    }

    Grid::get(operationContext())->clearForUnitTests();

    _opCtx.reset();
    ServiceContextMongoDTest::tearDown();
}

ShardingCatalogClient* ShardingMongodTestFixture::catalogClient() const {
    invariant(Grid::get(operationContext())->catalogClient());
    return Grid::get(operationContext())->catalogClient();
}

CatalogCache* ShardingMongodTestFixture::catalogCache() const {
    invariant(Grid::get(operationContext())->catalogCache());
    return Grid::get(operationContext())->catalogCache();
}

ShardRegistry* ShardingMongodTestFixture::shardRegistry() const {
    invariant(Grid::get(operationContext())->shardRegistry());
    return Grid::get(operationContext())->shardRegistry();
}

ClusterCursorManager* ShardingMongodTestFixture::clusterCursorManager() const {
    invariant(Grid::get(operationContext())->getCursorManager());
    return Grid::get(operationContext())->getCursorManager();
}

executor::TaskExecutorPool* ShardingMongodTestFixture::executorPool() const {
    invariant(Grid::get(operationContext())->getExecutorPool());
    return Grid::get(operationContext())->getExecutorPool();
}

void ShardingMongodTestFixture::shutdownExecutorPool() {
    invariant(!_executorPoolShutDown);
    executorPool()->shutdownAndJoin();
    _executorPoolShutDown = true;
}

executor::TaskExecutor* ShardingMongodTestFixture::executor() const {
    invariant(Grid::get(operationContext())->getExecutorPool());
    return Grid::get(operationContext())->getExecutorPool()->getFixedExecutor();
}

repl::ReplicationCoordinatorMock* ShardingMongodTestFixture::replicationCoordinator() const {
    invariant(_replCoord);
    return _replCoord;
}

DistLockCatalog* ShardingMongodTestFixture::distLockCatalog() const {
    invariant(_distLockCatalog);
    return _distLockCatalog;
}

DistLockManager* ShardingMongodTestFixture::distLock() const {
    invariant(_distLockManager);
    return _distLockManager;
}

RemoteCommandTargeterFactoryMock* ShardingMongodTestFixture::targeterFactory() const {
    invariant(_targeterFactory);
    return _targeterFactory;
}

}  // namespace merizo
