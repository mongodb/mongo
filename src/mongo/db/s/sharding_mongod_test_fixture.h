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

#pragma once

#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/s/sharding_test_fixture_common.h"

namespace mongo {

/**
 * Sets up this fixture as a mongod with a storage engine, OpObserver, and as a member of a replica
 * set.
 *
 * Additionally, provides an interface for initializing sharding components, mimicking the process
 * by which a real config or shard server does sharding initialization. Provides a set of default
 * components (including a NetworkInterface/TaskExecutor subsystem backed by the NetworkTestEnv),
 * but allows subclasses to replace any component with its real implementation, a mock, or nullptr.
 *
 * The ordering of the base classes here matter because both ShardingTestFixtureCommon and
 * ServiceContextMongoDTest inherit from ServiceContextTest, which overrides the global service
 * context.
 */
class ShardingMongodTestFixture : public ShardingTestFixtureCommon,
                                  public ServiceContextMongoDTest {
protected:
    ShardingMongodTestFixture(Options options = {}, bool allowMajorityReads = true);
    ~ShardingMongodTestFixture();

    void setUp() override;
    void tearDown() override;

    // Set a catalog cache to be used when initializing the Grid. Must be called before
    // initializeGlobalShardingStateForMongodForTest() in order to take effect.
    void setCatalogCache(std::unique_ptr<CatalogCache> cache);

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
    CatalogCache* catalogCache() const;
    ShardRegistry* shardRegistry() const;
    std::shared_ptr<executor::TaskExecutor> executor() const;
    ClusterCursorManager* clusterCursorManager() const;
    executor::TaskExecutorPool* executorPool() const;

    /**
     * Shuts down the TaskExecutorPool and remembers that it has been shut down, so that it is not
     * shut down again on tearDown.
     *
     * Not safe to call from multiple threads.
     */
    void shutdownExecutorPool();

    repl::ReplicationCoordinatorMock* replicationCoordinator() const;

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
     * Base class returns a real implementation of ShardRegistry.
     */
    virtual std::unique_ptr<ShardRegistry> makeShardRegistry(ConnectionString configConnStr);

    /**
     * Allows tests to conditionally construct a DistLockManager
     */
    virtual std::unique_ptr<DistLockManager> makeDistLockManager();

    /**
     * Base class returns nullptr.
     */
    virtual std::unique_ptr<ClusterCursorManager> makeClusterCursorManager();

    /**
     * Base class returns nullptr.
     */
    virtual std::unique_ptr<BalancerConfiguration> makeBalancerConfiguration();

    /**
     * Base class returns CatalogCache created from the CatalogCacheLoader set on the
     * ServiceContext.
     */
    virtual std::unique_ptr<CatalogCache> makeCatalogCache();

    /**
     * Setups the op observer listeners depending on cluster role.
     */
    virtual void setupOpObservers();

private:
    /**
     * Base class returns a TaskExecutorPool with a fixed TaskExecutor and a set of arbitrary
     * executors containing one TaskExecutor, each backed by a NetworkInterfaceMock/ThreadPoolMock
     * subsytem.
     */
    std::unique_ptr<executor::TaskExecutorPool> _makeTaskExecutorPool();

    const std::string _setName = "mySet";
    const std::vector<HostAndPort> _servers{
        HostAndPort("node1:12345"), HostAndPort("node2:12345"), HostAndPort("node3:12345")};

    repl::ReplicationCoordinatorMock* _replCoord = nullptr;

    // Records if a component has been shut down, so that it is only shut down once.
    bool _executorPoolShutDown = false;

    // Whether the test fixture should set a committed snapshot during setup so that tests can
    // perform majority reads without doing any writes.
    bool _setUpMajorityReads;
};

}  // namespace mongo
