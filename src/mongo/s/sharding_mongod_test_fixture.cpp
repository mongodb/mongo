/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/sharding_mongod_test_fixture.h"

#include <algorithm>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_local.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/set_shard_version_request.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/stdx/memory.h"
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

using std::string;
using std::vector;
using unittest::assertGet;

ShardingMongodTestFixture::ShardingMongodTestFixture() = default;

ShardingMongodTestFixture::~ShardingMongodTestFixture() = default;

const Seconds ShardingMongodTestFixture::kFutureTimeout{5};

void ShardingMongodTestFixture::setUp() {
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    // Set up this node as part of a replica set.

    repl::ReplSettings replSettings;
    replSettings.setReplSetString(ConnectionString::forReplicaSet(_setName, _servers).toString());
    replSettings.setMaster(true);
    auto replCoordPtr = makeReplicationCoordinator(replSettings);
    _replCoord = replCoordPtr.get();

    BSONArrayBuilder serversBob;
    for (size_t i = 0; i < _servers.size(); ++i) {
        serversBob.append(BSON("host" << _servers[i].toString() << "_id" << static_cast<int>(i)));
    }
    repl::ReplicaSetConfig replSetConfig;
    replSetConfig.initialize(BSON("_id" << _setName << "protocolVersion" << 1 << "version" << 3
                                        << "members"
                                        << serversBob.arr()));
    replCoordPtr->setGetConfigReturnValue(replSetConfig);

    repl::ReplicationCoordinator::set(service, std::move(replCoordPtr));

    service->setOpObserver(stdx::make_unique<OpObserverImpl>());
    repl::setOplogCollectionName();
    repl::createOplog(_opCtx.get());
}

std::unique_ptr<ReplicationCoordinatorMock> ShardingMongodTestFixture::makeReplicationCoordinator(
    ReplSettings replSettings) {
    return stdx::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(), replSettings);
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

    // Set up a NetworkInterfaceMock for the (one) arbitrary TaskExecutor that will go in the set
    // of arbitrary TaskExecutors.
    auto netForArbitraryExecutor = stdx::make_unique<executor::NetworkInterfaceMock>();

    // Set up (one) TaskExecutor for the set of arbitrary TaskExecutors.
    auto arbitraryExecutorForExecutorPool =
        makeThreadPoolTestExecutor(std::move(netForArbitraryExecutor));
    std::vector<std::unique_ptr<executor::TaskExecutor>> arbitraryExecutorsForExecutorPool;
    arbitraryExecutorsForExecutorPool.emplace_back(std::move(arbitraryExecutorForExecutorPool));

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

std::unique_ptr<DistLockCatalog> ShardingMongodTestFixture::makeDistLockCatalog(
    ShardRegistry* shardRegistry) {
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

std::unique_ptr<ShardingCatalogManager> ShardingMongodTestFixture::makeShardingCatalogManager(
    ShardingCatalogClient* catalogClient) {
    return nullptr;
}

std::unique_ptr<CatalogCache> ShardingMongodTestFixture::makeCatalogCache() {
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

    auto shardRegistryPtr = makeShardRegistry(configConnStr);

    auto distLockCatalogPtr = makeDistLockCatalog(shardRegistryPtr.get());
    _distLockCatalog = distLockCatalogPtr.get();

    auto distLockManagerPtr = makeDistLockManager(std::move(distLockCatalogPtr));
    _distLockManager = distLockManagerPtr.get();

    auto catalogClientPtr = makeShardingCatalogClient(std::move(distLockManagerPtr));
    auto catalogManagerPtr = makeShardingCatalogManager(catalogClientPtr.get());
    auto catalogCachePtr = makeCatalogCache();

    auto clusterCursorManagerPtr = makeClusterCursorManager();

    auto balancerConfigurationPtr = makeBalancerConfiguration();

    Grid::get(operationContext())
        ->init(std::move(catalogClientPtr),
               std::move(catalogManagerPtr),
               std::move(catalogCachePtr),
               std::move(shardRegistryPtr),
               std::move(clusterCursorManagerPtr),
               std::move(balancerConfigurationPtr),
               std::move(executorPoolPtr),
               _mockNetwork);

    // Note: ShardRegistry::startup() is not called because it starts a task executor with a self-
    // rescheduling task to reload the ShardRegistry over the network.
    if (Grid::get(operationContext())->catalogClient(operationContext())) {
        auto status = Grid::get(operationContext())->catalogClient(operationContext())->startup();
        if (!status.isOK()) {
            return status;
        }
    }

    if (Grid::get(operationContext())->catalogManager()) {
        auto status = Grid::get(operationContext())->catalogManager()->startup();
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void ShardingMongodTestFixture::tearDown() {
    // Only shut down components that were actually initialized and not already shut down.

    if (Grid::get(operationContext())->getExecutorPool() && !_executorPoolShutDown) {
        Grid::get(operationContext())->getExecutorPool()->shutdownAndJoin();
    }

    if (Grid::get(operationContext())->catalogManager()) {
        Grid::get(operationContext())->catalogManager()->shutDown(operationContext());
    }

    if (Grid::get(operationContext())->catalogClient(operationContext())) {
        Grid::get(operationContext())
            ->catalogClient(operationContext())
            ->shutDown(operationContext());
    }

    Grid::get(operationContext())->clearForUnitTests();

    _opCtx.reset();
    _client.reset();

    ServiceContextMongoDTest::tearDown();
}

ShardingCatalogClient* ShardingMongodTestFixture::catalogClient() const {
    invariant(Grid::get(operationContext())->catalogClient(operationContext()));
    return Grid::get(operationContext())->catalogClient(operationContext());
}

ShardingCatalogManager* ShardingMongodTestFixture::catalogManager() const {
    invariant(Grid::get(operationContext())->catalogManager());
    return Grid::get(operationContext())->catalogManager();
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

executor::NetworkInterfaceMock* ShardingMongodTestFixture::network() const {
    invariant(_mockNetwork);
    return _mockNetwork;
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

OperationContext* ShardingMongodTestFixture::operationContext() const {
    invariant(_opCtx);
    return _opCtx.get();
}

void ShardingMongodTestFixture::onCommand(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnv->onCommand(func);
}

void ShardingMongodTestFixture::onCommandWithMetadata(
    NetworkTestEnv::OnCommandWithMetadataFunction func) {
    _networkTestEnv->onCommandWithMetadata(func);
}

void ShardingMongodTestFixture::onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
    _networkTestEnv->onFindCommand(func);
}

void ShardingMongodTestFixture::onFindWithMetadataCommand(
    NetworkTestEnv::OnFindCommandWithMetadataFunction func) {
    _networkTestEnv->onFindWithMetadataCommand(func);
}

}  // namespace mongo
