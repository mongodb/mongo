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

#pragma once

#include <utility>

#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message_port_mock.h"

namespace mongo {

class BalancerConfiguration;
class CatalogCache;
class ConnectionString;
class ClusterCursorManager;
class DistLockCatalog;
class DistLockManager;
class NamespaceString;
class RemoteCommandTargeterFactoryMock;
class ShardingCatalogClient;
class ShardingCatalogManager;
class ShardRegistry;

namespace executor {
class NetworkInterfaceMock;
class NetworkTestEnv;
class TaskExecutor;
class TaskExecutorPool;
}  // namespace executor

namespace repl {
class ReplicationCoordinatorMock;
class ReplSettings;
}  // namespace repl

/**
 * Sets up this fixture as a mongod with a storage engine, OpObserver, and as a member of a replica
 * set.
 *
 * Additionally, provides an interface for initializing sharding components, mimicking the process
 * by which a real config or shard server does sharding initialization. Provides a set of default
 * components (including a NetworkInterface/TaskExecutor subsystem backed by the NetworkTestEnv),
 * but allows subclasses to replace any component with its real implementation, a mock, or nullptr.
 */
class ShardingMongodTestFixture : public ServiceContextMongoDTest {
public:
    ShardingMongodTestFixture();
    ~ShardingMongodTestFixture();

    static const Seconds kFutureTimeout;

    template <typename Lambda>
    executor::NetworkTestEnv::FutureHandle<typename std::result_of<Lambda()>::type> launchAsync(
        Lambda&& func) const {
        return _networkTestEnv->launchAsync(std::forward<Lambda>(func));
    }

    /**
     * Initializes sharding components according to the cluster role in
     * serverGlobalParams.clusterRole and puts the components on the Grid, mimicking the
     * initialization done by an actual config or shard mongod server.
     *
     * It is illegal to call this if serverGlobalParams.clusterRole is not ClusterRole::ShardServer
     * or ClusterRole::ConfigServer.
     */
    Status initializeGlobalShardingStateForMongodForTest(const ConnectionString& configConnStr);

    // Syntactic sugar for getting sharding components off the Grid, if they have been initialized.

    ShardingCatalogClient* catalogClient() const;
    ShardingCatalogManager* catalogManager() const;
    CatalogCache* catalogCache() const;
    ShardRegistry* shardRegistry() const;
    ClusterCursorManager* clusterCursorManager() const;
    executor::TaskExecutorPool* executorPool() const;

    /**
     * Shuts down the TaskExecutorPool and remembers that it has been shut down, so that it is not
     * shut down again on tearDown.
     *
     * Not safe to call from multiple threads.
     */
    void shutdownExecutorPool();

    // Syntactic sugar for getting executor and networking components off the Grid's executor pool,
    // if they have been initialized.

    executor::TaskExecutor* executor() const;
    executor::NetworkInterfaceMock* network() const;

    repl::ReplicationCoordinatorMock* replicationCoordinator() const;

    /**
     * Returns the stored raw pointer to the DistLockCatalog, if it has been initialized.
     */
    DistLockCatalog* distLockCatalog() const;

    /**
     * Returns the stored raw pointer to the DistLockManager, if it has been initialized.
     */
    DistLockManager* distLock() const;

    /**
     * Returns the stored raw pointer to the RemoteCommandTargeterFactoryMock, if it has been
     * initialized.
     */
    RemoteCommandTargeterFactoryMock* targeterFactory() const;

    /**
     * Returns the stored raw pointer to the OperationContext.
     */
    OperationContext* operationContext() const;

    /**
     * Blocking methods, which receive one message from the network and respond using the
     * responses returned from the input function. This is a syntactic sugar for simple,
     * single request + response or find tests.
     */
    void onCommand(executor::NetworkTestEnv::OnCommandFunction func);
    void onCommandWithMetadata(executor::NetworkTestEnv::OnCommandWithMetadataFunction func);
    void onFindCommand(executor::NetworkTestEnv::OnFindCommandFunction func);
    void onFindWithMetadataCommand(
        executor::NetworkTestEnv::OnFindCommandWithMetadataFunction func);

protected:
    /**
     * Sets up this fixture with a storage engine, OpObserver, and as a member of a replica set.
     */
    void setUp() override;

    /**
     * Resets the storage engine and operation context, and shuts down and resets any sharding
     * components that have been initialized but not yet shut down and reset.
     */
    void tearDown() override;

    // Allow subclasses to modify this node's hostname and port, set name, and replica set members.

    const HostAndPort _host{"node1:12345"};
    const std::string _setName = "mySet";
    const std::vector<HostAndPort> _servers{
        _host, HostAndPort("node2:12345"), HostAndPort("node3:12345")};

    // Methods for creating and returning sharding components. Some of these methods have been
    // implemented to return the real implementation of the component as the default, while others
    // return a mock or nullptr. Subclasses can override any of these methods to create and
    // initialize a real implementation, a mock, or nullptr, as needed.

    // Warning: If a component takes ownership of another component for which a real or mock is
    // being used, the component must also be real or mock implementation, so that it can actually
    // take the ownership.

    /**
     * Base class returns ReplicationCoordinatorMock.
     */
    virtual std::unique_ptr<repl::ReplicationCoordinatorMock> makeReplicationCoordinator(
        repl::ReplSettings replSettings);

    /**
     * Base class returns a TaskExecutorPool with a fixed TaskExecutor and a set of arbitrary
     * executors containing one TaskExecutor, each backed by a NetworkInterfaceMock/ThreadPoolMock
     * subsytem.
     */
    virtual std::unique_ptr<executor::TaskExecutorPool> makeTaskExecutorPool();

    /**
     * Base class returns a real implementation of ShardRegistry.
     */
    virtual std::unique_ptr<ShardRegistry> makeShardRegistry(ConnectionString configConnStr);

    /**
     * Base class returns nullptr.
     */
    virtual std::unique_ptr<DistLockCatalog> makeDistLockCatalog(ShardRegistry* shardRegistry);

    /**
     * Base class returns nullptr.
     *
     * Note: DistLockManager takes ownership of the DistLockCatalog, so if DistLockCatalog is not
     * nullptr, a real or mock DistLockManager must be supplied.
     */
    virtual std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog);

    /**
     * Base class returns nullptr.
     *
     * Note: ShardingCatalogClient takes ownership of DistLockManager, so if DistLockManager is not
     * nulllptr, a real or mock ShardingCatalogClient must be supplied.
     */
    virtual std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager);

    /**
     * Base class returns nullptr.
     */
    virtual std::unique_ptr<ShardingCatalogManager> makeShardingCatalogManager(
        ShardingCatalogClient* catalogClient);

    /**
     * Base class returns nullptr.
     */
    virtual std::unique_ptr<CatalogCache> makeCatalogCache();

    /**
     * Base class returns nullptr.
     */
    virtual std::unique_ptr<ClusterCursorManager> makeClusterCursorManager();

    /**
     * Base class returns nullptr.
     */
    virtual std::unique_ptr<BalancerConfiguration> makeBalancerConfiguration();

private:
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

    // Since a NetworkInterface is a private member of a TaskExecutor, we store a raw pointer to the
    // fixed TaskExecutor's NetworkInterface here.
    // TODO(esha): Currently, some fine-grained synchronization of the network and task executor is
    // is outside of NetworkTestEnv's capabilities. If all control of the network is done through
    // _networkTestEnv, storing this raw pointer is not necessary.
    executor::NetworkInterfaceMock* _mockNetwork = nullptr;

    // Since the RemoteCommandTargeterFactory is currently a private member of ShardFactory, we
    // store a raw pointer to it here.
    RemoteCommandTargeterFactoryMock* _targeterFactory = nullptr;

    // Since the DistLockCatalog is currently a private member of ReplSetDistLockManager, we store
    // a raw pointer to it here.
    DistLockCatalog* _distLockCatalog = nullptr;

    // Since the DistLockManager is currently a private member of ShardingCatalogClient, we
    // store a raw pointer to it here.
    DistLockManager* _distLockManager = nullptr;

    repl::ReplicationCoordinatorMock* _replCoord = nullptr;

    // Allows for processing tasks through the NetworkInterfaceMock/ThreadPoolMock subsystem.
    std::unique_ptr<executor::NetworkTestEnv> _networkTestEnv;

    // Records if a component has been shut down, so that it is only shut down once.
    bool _executorPoolShutDown = false;
};

}  // namespace mongo
