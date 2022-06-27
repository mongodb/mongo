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

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_mongod_test_fixture.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/s/config_server_op_observer.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/s/shard_local.h"
#include "mongo/db/s/shard_server_op_observer.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

using executor::NetworkInterfaceMock;
using executor::NetworkTestEnv;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using repl::ReplicationCoordinator;
using repl::ReplicationCoordinatorMock;
using repl::ReplSettings;
using unittest::assertGet;

ShardingMongodTestFixture::ShardingMongodTestFixture(Options options, bool setUpMajorityReads)
    : ServiceContextMongoDTest(std::move(options)), _setUpMajorityReads(setUpMajorityReads) {}

ShardingMongodTestFixture::~ShardingMongodTestFixture() = default;

std::unique_ptr<ReplicationCoordinatorMock> ShardingMongodTestFixture::makeReplicationCoordinator(
    ReplSettings replSettings) {
    auto coordinator =
        std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(), replSettings);
    ASSERT_OK(coordinator->setFollowerMode(repl::MemberState::RS_PRIMARY));
    return coordinator;
}

std::unique_ptr<executor::TaskExecutorPool> ShardingMongodTestFixture::_makeTaskExecutorPool() {
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
    auto fixedTaskExecutor = makeSharedThreadPoolTestExecutor(std::move(netForFixedTaskExecutor));
    _networkTestEnv = std::make_unique<NetworkTestEnv>(fixedTaskExecutor.get(), _mockNetwork);

    // Set up (one) TaskExecutor for the set of arbitrary TaskExecutors.
    std::vector<std::shared_ptr<executor::TaskExecutor>> arbitraryExecutorsForExecutorPool;
    arbitraryExecutorsForExecutorPool.emplace_back(
        makeSharedThreadPoolTestExecutor(std::make_unique<executor::NetworkInterfaceMock>()));

    // Set up the TaskExecutorPool with the fixed TaskExecutor and set of arbitrary TaskExecutors.
    auto executorPool = std::make_unique<executor::TaskExecutorPool>();
    executorPool->addExecutors(std::move(arbitraryExecutorsForExecutorPool),
                               std::move(fixedTaskExecutor));

    return executorPool;
}

std::unique_ptr<ShardRegistry> ShardingMongodTestFixture::makeShardRegistry(
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
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
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

    return std::make_unique<ShardRegistry>(std::move(shardFactory), configConnStr);
}

std::unique_ptr<DistLockManager> ShardingMongodTestFixture::makeDistLockManager() {
    class DistLockManagerNoop : public DistLockManager {
    public:
        DistLockManagerNoop() : DistLockManager(OID::gen()) {}
        void startUp() override {}
        void shutDown(OperationContext* opCtx) {}
        std::string getProcessID() override {
            return "DistLockManagerNoop";
        }
        Status lockDirect(OperationContext* opCtx,
                          StringData name,
                          StringData whyMessage,
                          Milliseconds waitFor) override {
            return Status::OK();
        }
        Status tryLockDirectWithLocalWriteConcern(OperationContext* opCtx,
                                                  StringData name,
                                                  StringData whyMessage) override {
            return Status::OK();
        }
        void unlock(Interruptible* intr, StringData name) override {}
        void unlockAll(OperationContext* opCtx) override {}
    };
    return std::make_unique<DistLockManagerNoop>();
}

std::unique_ptr<ClusterCursorManager> ShardingMongodTestFixture::makeClusterCursorManager() {
    return nullptr;
}

std::unique_ptr<BalancerConfiguration> ShardingMongodTestFixture::makeBalancerConfiguration() {
    return std::make_unique<BalancerConfiguration>();
}

std::unique_ptr<CatalogCache> ShardingMongodTestFixture::makeCatalogCache() {
    return std::make_unique<CatalogCache>(getServiceContext(),
                                          CatalogCacheLoader::get(getServiceContext()));
}

Status ShardingMongodTestFixture::initializeGlobalShardingStateForMongodForTest(
    const ConnectionString& configConnStr) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
              serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

    // Create and initialize each sharding component individually before moving them to the Grid
    // in order to control the order of initialization, since some components depend on others.

    auto executorPoolPtr = _makeTaskExecutorPool();
    if (executorPoolPtr) {
        executorPoolPtr->startup();
    }

    auto const grid = Grid::get(operationContext());
    grid->init(makeShardingCatalogClient(),
               makeCatalogCache(),
               makeShardRegistry(configConnStr),
               makeClusterCursorManager(),
               makeBalancerConfiguration(),
               std::move(executorPoolPtr),
               _mockNetwork);

    DistLockManager::create(getServiceContext(), makeDistLockManager());
    if (DistLockManager::get(operationContext())) {
        DistLockManager::get(operationContext())->startUp();
    }

    return Status::OK();
}

void ShardingMongodTestFixture::setUp() {
    ServiceContextMongoDTest::setUp();
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

    auto storagePtr = std::make_unique<repl::StorageInterfaceMock>();

    repl::DropPendingCollectionReaper::set(
        service, std::make_unique<repl::DropPendingCollectionReaper>(storagePtr.get()));

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

    // Set the highest FCV because otherwise it defaults to the lower FCV. This way we default to
    // testing this release's code, not backwards compatibility code.
    // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
    serverGlobalParams.mutableFeatureCompatibility.setVersion(multiversion::GenericFCV::kLatest);

    if (_setUpMajorityReads && service->getStorageEngine()->getSnapshotManager()) {
        WriteUnitOfWork wuow{operationContext()};
        service->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            repl::getNextOpTime(operationContext()).getTimestamp());
        wuow.commit();
    }
}

void ShardingMongodTestFixture::tearDown() {
    ReplicaSetMonitor::cleanup();

    if (DistLockManager::get(operationContext())) {
        DistLockManager::get(operationContext())->shutDown(operationContext());
    }

    if (Grid::get(operationContext())->getExecutorPool() && !_executorPoolShutDown) {
        Grid::get(operationContext())->getExecutorPool()->shutdownAndJoin();
    }

    if (Grid::get(operationContext())->shardRegistry()) {
        Grid::get(operationContext())->shardRegistry()->shutdown();
    }

    CollectionShardingStateFactory::clear(getServiceContext());
    Grid::get(operationContext())->clearForUnitTests();

    ShardingTestFixtureCommon::tearDown();
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

std::shared_ptr<executor::TaskExecutor> ShardingMongodTestFixture::executor() const {
    invariant(Grid::get(operationContext())->getExecutorPool());
    return Grid::get(operationContext())->getExecutorPool()->getFixedExecutor();
}

repl::ReplicationCoordinatorMock* ShardingMongodTestFixture::replicationCoordinator() const {
    invariant(_replCoord);
    return _replCoord;
}

void ShardingMongodTestFixture::setupOpObservers() {
    auto opObserverRegistry =
        checked_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
    opObserverRegistry->addObserver(std::make_unique<OpObserverShardingImpl>());
    opObserverRegistry->addObserver(std::make_unique<ShardServerOpObserver>());
}

}  // namespace mongo
