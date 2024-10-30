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

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_local.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/version/releases.h"

namespace mongo {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using repl::ReplicationCoordinatorMock;
using repl::ReplSettings;

ShardingMongoDTestFixture::ShardingMongoDTestFixture(
    MongoDScopedGlobalServiceContextForTest::Options options, bool setUpMajorityReads)
    : ShardingTestFixtureCommon(
          std::make_unique<MongoDScopedGlobalServiceContextForTest>(std::move(options))),
      _setUpMajorityReads(setUpMajorityReads) {}

ShardingMongoDTestFixture::~ShardingMongoDTestFixture() = default;

std::unique_ptr<ReplicationCoordinatorMock> ShardingMongoDTestFixture::makeReplicationCoordinator(
    ReplSettings replSettings) {
    auto coordinator =
        std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(), replSettings);
    ASSERT_OK(coordinator->setFollowerMode(repl::MemberState::RS_PRIMARY));
    return coordinator;
}

std::unique_ptr<executor::TaskExecutorPool> ShardingMongoDTestFixture::_makeTaskExecutorPool() {
    // Set up a NetworkInterfaceMock. Note, unlike NetworkInterfaceASIO, which has its own pool of
    // threads, tasks in the NetworkInterfaceMock must be carried out synchronously by the (single)
    // thread the unit test is running on.
    auto netForFixedTaskExecutor = std::make_unique<executor::NetworkInterfaceMock>();
    _mockNetwork = netForFixedTaskExecutor.get();

    // Set up a ThreadPoolTaskExecutor. Note, for local tasks this TaskExecutor uses a
    // ThreadPoolMock, and for remote tasks it uses the NetworkInterfaceMock created above. However,
    // note that the ThreadPoolMock uses the NetworkInterfaceMock's threads to run tasks, which is
    // again just the (single) thread the unit test is running on. Therefore, all tasks, local and
    // remote, must be carried out synchronously by the test thread.
    auto fixedTaskExecutor = makeThreadPoolTestExecutor(std::move(netForFixedTaskExecutor));
    _networkTestEnv = std::make_unique<NetworkTestEnv>(fixedTaskExecutor.get(), _mockNetwork);

    // Set up (one) TaskExecutor for the set of arbitrary TaskExecutors.
    auto mockNetworkForPool = std::make_unique<executor::NetworkInterfaceMock>();
    _mockNetworkForPool = mockNetworkForPool.get();
    std::vector<std::shared_ptr<executor::TaskExecutor>> arbitraryExecutorsForExecutorPool;
    arbitraryExecutorsForExecutorPool.emplace_back(
        makeThreadPoolTestExecutor(std::move(mockNetworkForPool)));

    // Set up the TaskExecutorPool with the fixed TaskExecutor and set of arbitrary TaskExecutors.
    auto executorPool = std::make_unique<executor::TaskExecutorPool>();
    executorPool->addExecutors(std::move(arbitraryExecutorsForExecutorPool),
                               std::move(fixedTaskExecutor));

    return executorPool;
}

std::unique_ptr<ShardRegistry> ShardingMongoDTestFixture::makeShardRegistry(
    ConnectionString configConnStr) {
    auto targeterFactory(std::make_unique<RemoteCommandTargeterFactoryMock>());
    auto targeterFactoryPtr = targeterFactory.get();
    _targeterFactory = targeterFactoryPtr;

    ShardFactory::BuilderCallable setBuilder = [targeterFactoryPtr](
                                                   const ShardId& shardId,
                                                   const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuilderCallable standaloneBuilder = [targeterFactoryPtr](
                                                          const ShardId& shardId,
                                                          const ConnectionString& connStr) {
        return std::make_unique<ShardRemote>(shardId, connStr, targeterFactoryPtr->create(connStr));
    };

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::ConnectionType::kReplicaSet, std::move(setBuilder)},
        {ConnectionString::ConnectionType::kStandalone, std::move(standaloneBuilder)}};

    // Only config servers use ShardLocal for now.
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        ShardFactory::BuilderCallable localBuilder = [](const ShardId& shardId,
                                                        const ConnectionString& connStr) {
            return std::make_unique<ShardLocal>(shardId);
        };
        buildersMap.insert(
            std::pair<ConnectionString::ConnectionType, ShardFactory::BuilderCallable>(
                ConnectionString::ConnectionType::kLocal, std::move(localBuilder)));
    }

    auto shardFactory =
        std::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    return std::make_unique<ShardRegistry>(
        getServiceContext(), std::move(shardFactory), configConnStr);
}

std::unique_ptr<ClusterCursorManager> ShardingMongoDTestFixture::makeClusterCursorManager() {
    return nullptr;
}

std::unique_ptr<BalancerConfiguration> ShardingMongoDTestFixture::makeBalancerConfiguration() {
    return std::make_unique<BalancerConfiguration>();
}

Status ShardingMongoDTestFixture::initializeGlobalShardingStateForMongodForTest(
    const ConnectionString& configConnStr,
    std::unique_ptr<CatalogCache> catalogCache,
    std::shared_ptr<CatalogCacheLoader> catalogCacheLoader) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));

    // Create and initialize each sharding component individually before moving them to the Grid
    // in order to control the order of initialization, since some components depend on others.

    auto executorPoolPtr = _makeTaskExecutorPool();
    if (executorPoolPtr) {
        executorPoolPtr->startup();
    }

    auto const grid = Grid::get(operationContext());
    grid->init(makeShardingCatalogClient(),
               std::move(catalogCache),
               makeShardRegistry(configConnStr),
               makeClusterCursorManager(),
               makeBalancerConfiguration(),
               std::move(executorPoolPtr),
               _mockNetwork);

    FilteringMetadataCache::initForTesting(getServiceContext(), catalogCacheLoader);

    return Status::OK();
}

void ShardingMongoDTestFixture::setUp() {
    ShardingTestFixtureCommon::setUp();

    const auto service = getServiceContext();

    // Set up this node as shard node, which is part of a replica set

    repl::ReplSettings replSettings;
    replSettings.setOplogSizeBytes(512'000);
    replSettings.setReplSetString(ConnectionString::forReplicaSet(_setName, _servers).toString());
    auto replCoordPtr = makeReplicationCoordinator(replSettings);
    _replCoord = replCoordPtr.get();

    BSONArrayBuilder serversBob;
    for (size_t i = 0; i < _servers.size(); ++i) {
        serversBob.append(BSON("host" << _servers[i].toString() << "_id" << static_cast<int>(i)));
    }

    auto replSetConfig =
        repl::ReplSetConfig::parse(BSON("_id" << _setName << "protocolVersion" << 1 << "version"
                                              << 3 << "members" << serversBob.arr()));
    replCoordPtr->setGetConfigReturnValue(replSetConfig);

    repl::ReplicationCoordinator::set(service, std::move(replCoordPtr));

    auto storagePtr = std::make_unique<repl::StorageInterfaceImpl>();

    repl::ReplicationProcess::set(service,
                                  std::make_unique<repl::ReplicationProcess>(
                                      storagePtr.get(),
                                      std::make_unique<repl::ReplicationConsistencyMarkersMock>(),
                                      std::make_unique<repl::ReplicationRecoveryMock>()));

    ASSERT_OK(repl::ReplicationProcess::get(operationContext())
                  ->initializeRollbackID(operationContext()));

    repl::StorageInterface::set(service, std::move(storagePtr));

    setupOpObservers();

    repl::createOplog(operationContext());

    MongoDSessionCatalog::set(
        service,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    // Set the highest FCV because otherwise it defaults to the lower FCV. This way we default to
    // testing this release's code, not backwards compatibility code.
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

    if (_setUpMajorityReads && service->getStorageEngine()->getSnapshotManager()) {
        WriteUnitOfWork wuow{operationContext()};
        service->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            repl::getNextOpTime(operationContext()).getTimestamp());
        wuow.commit();
    }
}

void ShardingMongoDTestFixture::tearDown() {
    ReplicaSetMonitor::cleanup();

    if (Grid::get(operationContext())->isInitialized()) {
        shutdownExecutorPool();

        if (Grid::get(operationContext())->shardRegistry()) {
            Grid::get(operationContext())->shardRegistry()->shutdown();
        }
    }

    CollectionShardingStateFactory::clear(getServiceContext());
    Grid::get(operationContext())->clearForUnitTests();

    ShardingTestFixtureCommon::tearDown();
}

ShardingCatalogClient* ShardingMongoDTestFixture::catalogClient() const {
    invariant(Grid::get(operationContext())->catalogClient());
    return Grid::get(operationContext())->catalogClient();
}

CatalogCache* ShardingMongoDTestFixture::catalogCache() const {
    invariant(Grid::get(operationContext())->catalogCache());
    return Grid::get(operationContext())->catalogCache();
}

ShardRegistry* ShardingMongoDTestFixture::shardRegistry() const {
    invariant(Grid::get(operationContext())->shardRegistry());
    return Grid::get(operationContext())->shardRegistry();
}

ClusterCursorManager* ShardingMongoDTestFixture::clusterCursorManager() const {
    invariant(Grid::get(operationContext())->getCursorManager());
    return Grid::get(operationContext())->getCursorManager();
}

executor::TaskExecutorPool* ShardingMongoDTestFixture::executorPool() const {
    invariant(Grid::get(operationContext())->getExecutorPool());
    return Grid::get(operationContext())->getExecutorPool();
}

std::shared_ptr<executor::TaskExecutor> ShardingMongoDTestFixture::executor() const {
    invariant(Grid::get(operationContext())->getExecutorPool());
    return Grid::get(operationContext())->getExecutorPool()->getFixedExecutor();
}

repl::ReplicationCoordinatorMock* ShardingMongoDTestFixture::replicationCoordinator() const {
    invariant(_replCoord);
    return _replCoord;
}

void ShardingMongoDTestFixture::setupOpObservers() {
    auto opObserverRegistry =
        checked_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
    opObserverRegistry->addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserverRegistry->addObserver(std::make_unique<ShardServerOpObserver>());
}

}  // namespace mongo
