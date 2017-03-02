/**
 *    Copyright (C) 2016 BongoDB Inc.
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

#include "bongo/platform/basic.h"

#include "bongo/s/sharding_bongod_test_fixture.h"

#include <algorithm>
#include <vector>

#include "bongo/base/status_with.h"
#include "bongo/client/remote_command_targeter_factory_mock.h"
#include "bongo/client/remote_command_targeter_mock.h"
#include "bongo/db/client.h"
#include "bongo/db/commands.h"
#include "bongo/db/db_raii.h"
#include "bongo/db/namespace_string.h"
#include "bongo/db/op_observer_impl.h"
#include "bongo/db/query/cursor_response.h"
#include "bongo/db/query/query_request.h"
#include "bongo/db/repl/oplog.h"
#include "bongo/db/repl/read_concern_args.h"
#include "bongo/db/repl/repl_settings.h"
#include "bongo/db/repl/replication_coordinator.h"
#include "bongo/db/repl/replication_coordinator_mock.h"
#include "bongo/db/service_context_noop.h"
#include "bongo/executor/network_interface_mock.h"
#include "bongo/executor/task_executor_pool.h"
#include "bongo/executor/thread_pool_task_executor_test_fixture.h"
#include "bongo/rpc/metadata/repl_set_metadata.h"
#include "bongo/rpc/metadata/server_selection_metadata.h"
#include "bongo/s/balancer_configuration.h"
#include "bongo/s/catalog/dist_lock_catalog.h"
#include "bongo/s/catalog/dist_lock_manager.h"
#include "bongo/s/catalog/sharding_catalog_client.h"
#include "bongo/s/catalog/sharding_catalog_manager.h"
#include "bongo/s/catalog/type_changelog.h"
#include "bongo/s/catalog/type_collection.h"
#include "bongo/s/catalog/type_shard.h"
#include "bongo/s/catalog_cache.h"
#include "bongo/s/client/shard_factory.h"
#include "bongo/s/client/shard_local.h"
#include "bongo/s/client/shard_registry.h"
#include "bongo/s/client/shard_remote.h"
#include "bongo/s/grid.h"
#include "bongo/s/query/cluster_cursor_manager.h"
#include "bongo/s/set_shard_version_request.h"
#include "bongo/s/write_ops/batched_command_request.h"
#include "bongo/s/write_ops/batched_command_response.h"
#include "bongo/stdx/memory.h"
#include "bongo/util/clock_source_mock.h"
#include "bongo/util/tick_source_mock.h"

namespace bongo {

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

ShardingBongodTestFixture::ShardingBongodTestFixture() = default;

ShardingBongodTestFixture::~ShardingBongodTestFixture() = default;

const Seconds ShardingBongodTestFixture::kFutureTimeout{5};

void ShardingBongodTestFixture::setUp() {
    ServiceContextBongoDTest::setUp();

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

std::unique_ptr<ReplicationCoordinatorMock> ShardingBongodTestFixture::makeReplicationCoordinator(
    ReplSettings replSettings) {
    return stdx::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(), replSettings);
}

std::unique_ptr<executor::TaskExecutorPool> ShardingBongodTestFixture::makeTaskExecutorPool() {
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

std::unique_ptr<ShardRegistry> ShardingBongodTestFixture::makeShardRegistry(
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

std::unique_ptr<DistLockCatalog> ShardingBongodTestFixture::makeDistLockCatalog(
    ShardRegistry* shardRegistry) {
    return nullptr;
}

std::unique_ptr<DistLockManager> ShardingBongodTestFixture::makeDistLockManager(
    std::unique_ptr<DistLockCatalog> distLockCatalog) {
    return nullptr;
}

std::unique_ptr<ShardingCatalogClient> ShardingBongodTestFixture::makeShardingCatalogClient(
    std::unique_ptr<DistLockManager> distLockManager) {
    return nullptr;
}

std::unique_ptr<ShardingCatalogManager> ShardingBongodTestFixture::makeShardingCatalogManager(
    ShardingCatalogClient* catalogClient) {
    return nullptr;
}

std::unique_ptr<CatalogCache> ShardingBongodTestFixture::makeCatalogCache() {
    return nullptr;
}

std::unique_ptr<ClusterCursorManager> ShardingBongodTestFixture::makeClusterCursorManager() {
    return nullptr;
}

std::unique_ptr<BalancerConfiguration> ShardingBongodTestFixture::makeBalancerConfiguration() {
    return nullptr;
}

Status ShardingBongodTestFixture::initializeGlobalShardingStateForBongodForTest(
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

void ShardingBongodTestFixture::tearDown() {
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

    ServiceContextBongoDTest::tearDown();
}

ShardingCatalogClient* ShardingBongodTestFixture::catalogClient() const {
    invariant(Grid::get(operationContext())->catalogClient(operationContext()));
    return Grid::get(operationContext())->catalogClient(operationContext());
}

ShardingCatalogManager* ShardingBongodTestFixture::catalogManager() const {
    invariant(Grid::get(operationContext())->catalogManager());
    return Grid::get(operationContext())->catalogManager();
}

CatalogCache* ShardingBongodTestFixture::catalogCache() const {
    invariant(Grid::get(operationContext())->catalogCache());
    return Grid::get(operationContext())->catalogCache();
}

ShardRegistry* ShardingBongodTestFixture::shardRegistry() const {
    invariant(Grid::get(operationContext())->shardRegistry());
    return Grid::get(operationContext())->shardRegistry();
}

ClusterCursorManager* ShardingBongodTestFixture::clusterCursorManager() const {
    invariant(Grid::get(operationContext())->getCursorManager());
    return Grid::get(operationContext())->getCursorManager();
}

executor::TaskExecutorPool* ShardingBongodTestFixture::executorPool() const {
    invariant(Grid::get(operationContext())->getExecutorPool());
    return Grid::get(operationContext())->getExecutorPool();
}

void ShardingBongodTestFixture::shutdownExecutorPool() {
    invariant(!_executorPoolShutDown);
    executorPool()->shutdownAndJoin();
    _executorPoolShutDown = true;
}

executor::NetworkInterfaceMock* ShardingBongodTestFixture::network() const {
    invariant(_mockNetwork);
    return _mockNetwork;
}

executor::TaskExecutor* ShardingBongodTestFixture::executor() const {
    invariant(Grid::get(operationContext())->getExecutorPool());
    return Grid::get(operationContext())->getExecutorPool()->getFixedExecutor();
}

repl::ReplicationCoordinatorMock* ShardingBongodTestFixture::replicationCoordinator() const {
    invariant(_replCoord);
    return _replCoord;
}

DistLockCatalog* ShardingBongodTestFixture::distLockCatalog() const {
    invariant(_distLockCatalog);
    return _distLockCatalog;
}

DistLockManager* ShardingBongodTestFixture::distLock() const {
    invariant(_distLockManager);
    return _distLockManager;
}

RemoteCommandTargeterFactoryMock* ShardingBongodTestFixture::targeterFactory() const {
    invariant(_targeterFactory);
    return _targeterFactory;
}

OperationContext* ShardingBongodTestFixture::operationContext() const {
    invariant(_opCtx);
    return _opCtx.get();
}

void ShardingBongodTestFixture::onCommand(NetworkTestEnv::OnCommandFunction func) {
    _networkTestEnv->onCommand(func);
}

void ShardingBongodTestFixture::onCommandWithMetadata(
    NetworkTestEnv::OnCommandWithMetadataFunction func) {
    _networkTestEnv->onCommandWithMetadata(func);
}

void ShardingBongodTestFixture::onFindCommand(NetworkTestEnv::OnFindCommandFunction func) {
    _networkTestEnv->onFindCommand(func);
}

void ShardingBongodTestFixture::onFindWithMetadataCommand(
    NetworkTestEnv::OnFindCommandWithMetadataFunction func) {
    _networkTestEnv->onFindWithMetadataCommand(func);
}

}  // namespace bongo
