/**
 *    Copyright (C) 2016 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/s/balancer/balancer_configuration.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/sharding_catalog_manager_mock.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"


namespace mongo {
namespace {

/**
 * Initializes the grid object with the bare minimum and is not intended to be functional.
 */
void initGrid(OperationContext* txn, const ConnectionString& configConnString) {
    // Set up executor pool used for most operations.
    auto fixedNet = stdx::make_unique<executor::NetworkInterfaceMock>();
    auto mockNetwork = fixedNet.get();

    auto fixedExec = makeThreadPoolTestExecutor(std::move(fixedNet));


    auto netForPool = stdx::make_unique<executor::NetworkInterfaceMock>();
    auto execForPool = makeThreadPoolTestExecutor(std::move(netForPool));
    std::vector<std::unique_ptr<executor::TaskExecutor>> executorsForPool;
    executorsForPool.emplace_back(std::move(execForPool));

    auto executorPool = stdx::make_unique<executor::TaskExecutorPool>();
    executorPool->addExecutors(std::move(executorsForPool), std::move(fixedExec));
    executorPool->startup();

    auto targeterFactory = stdx::make_unique<RemoteCommandTargeterFactoryMock>();
    auto targeterFactoryPtr = targeterFactory.get();

    auto configTargeter(stdx::make_unique<RemoteCommandTargeterMock>());
    configTargeter->setConnectionStringReturnValue(configConnString);
    targeterFactory->addTargeterToReturn(configConnString, std::move(configTargeter));

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

    ShardFactory::BuildersMap buildersMap{
        {ConnectionString::SET, std::move(setBuilder)},
        {ConnectionString::MASTER, std::move(masterBuilder)},
    };

    auto shardFactory =
        stdx::make_unique<ShardFactory>(std::move(buildersMap), std::move(targeterFactory));

    auto shardRegistry(stdx::make_unique<ShardRegistry>(std::move(shardFactory), configConnString));

    grid.init(
        stdx::make_unique<ShardingCatalogClientMock>(),
        stdx::make_unique<ShardingCatalogManagerMock>(),
        stdx::make_unique<CatalogCache>(),
        std::move(shardRegistry),
        stdx::make_unique<ClusterCursorManager>(txn->getServiceContext()->getPreciseClockSource()),
        stdx::make_unique<BalancerConfiguration>(),
        std::move(executorPool),
        mockNetwork);
}

class ShardingStateTest : public mongo::unittest::Test {
public:
    void setUp() override {
        _service.setFastClockSource(stdx::make_unique<ClockSourceMock>());
        _service.setPreciseClockSource(stdx::make_unique<ClockSourceMock>());
        _service.setTickSource(stdx::make_unique<TickSourceMock>());

        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        _client = _service.makeClient("ShardingStateTest");
        _opCtx = _client->makeOperationContext();

        _shardingState.setGlobalInitMethodForTest([&](
            OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            initGrid(_opCtx.get(), connStr);
            return Status::OK();
        });
    }

    void tearDown() override {
        // ShardingState initialize can modify ReplicaSetMonitor state.
        ReplicaSetMonitor::cleanup();

        // Cleanup only if shard registry was initialized
        if (grid.shardRegistry()) {
            grid.getExecutorPool()->shutdownAndJoin();
            grid.clearForUnitTests();
        }
    }

    OperationContext* txn() {
        return _opCtx.get();
    }

    ShardingState* shardingState() {
        return &_shardingState;
    }

private:
    ServiceContextNoop _service;
    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;

    ShardingState _shardingState;
};

TEST_F(ShardingStateTest, ValidShardIdentitySucceeds) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max()));
    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitWhilePreviouslyInErrorStateWillStayInErrorState) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::ShutdownInProgress, "shutting down"};
        });

    {
        auto status =
            shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max());
        ASSERT_EQ(ErrorCodes::ShutdownInProgress, status);
    }

    // ShardingState is now in error state, attempting to call it again will still result in error.

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status::OK();
        });

    {
        auto status =
            shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max());
        ASSERT_EQ(ErrorCodes::ManualInterventionRequired, status);
    }

    ASSERT_FALSE(shardingState()->enabled());
}

TEST_F(ShardingStateTest, InitializeAgainWithMatchingShardIdentitySucceeds) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max()));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity2, Date_t::max()));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithSameReplSetNameSucceeds) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max()));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "b:2,c:3", "config"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity2, Date_t::max()));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentReplSetNameFails) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max()));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "configRS"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status =
        shardingState()->initializeFromShardIdentity(txn(), shardIdentity2, Date_t::max());
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentShardNameFails) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max()));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName("b");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status =
        shardingState()->initializeFromShardIdentity(txn(), shardIdentity2, Date_t::max());
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentClusterIdFails) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity, Date_t::max()));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status =
        shardingState()->initializeFromShardIdentity(txn(), shardIdentity2, Date_t::max());
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

}  // unnamed namespace
}  // namespace mongo
