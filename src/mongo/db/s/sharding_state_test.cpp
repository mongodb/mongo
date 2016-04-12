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
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager_mock.h"
#include "mongo/s/client/shard_factory_mock.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"


namespace mongo {
namespace {

/**
 * Initializes the grid object with the bare minimum and is not intended to be functional.
 */
void initGrid(OperationContext* txn, const ConnectionString& configConnString) {
    auto shardFactory(stdx::make_unique<ShardFactoryMock>());

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

    auto shardRegistry(stdx::make_unique<ShardRegistry>(
        std::move(shardFactory), std::move(executorPool), mockNetwork, configConnString));
    shardRegistry->startup();

    grid.init(
        stdx::make_unique<CatalogManagerMock>(),
        stdx::make_unique<CatalogCache>(),
        std::move(shardRegistry),
        stdx::make_unique<ClusterCursorManager>(txn->getServiceContext()->getPreciseClockSource()));
}

class ShardingStateTest : public mongo::unittest::Test {
public:
    void setUp() override {
        _service.setPreciseClockSource(stdx::make_unique<ClockSourceMock>());

        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        _client = _service.makeClient("ShardingStateTest");
        _opCtx = _client->makeOperationContext();

        _shardingState.setGlobalInitMethodForTest(
            [](OperationContext* txn, const ConnectionString& connStr) {
                initGrid(txn, connStr);
                return Status::OK();
            });
    }

    void tearDown() override {
        // ShardingState initialize can modify ReplicaSetMonitor state.
        ReplicaSetMonitor::cleanup();

        // Cleanup only if shard registry was initialized
        if (grid.shardRegistry()) {
            grid.shardRegistry()->shutdown();
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
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity));
    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InvalidConfigServerConnStringDoesNotParse) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("invalid:x");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    auto status = shardingState()->initializeFromShardIdentity(txn(), shardIdentity);
    ASSERT_EQ(ErrorCodes::FailedToParse, status);
    ASSERT_FALSE(shardingState()->enabled());
}

TEST_F(ShardingStateTest, CannotHaveNonReplConfigServerConnString) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("a:1");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    auto status = shardingState()->initializeFromShardIdentity(txn(), shardIdentity);
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, status);
    ASSERT_FALSE(shardingState()->enabled());
}

TEST_F(ShardingStateTest, InitWhilePreviouslyInErrorStateWillStayInErrorState) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) {
            return Status{ErrorCodes::ShutdownInProgress, "shutting down"};
        });

    {
        auto status = shardingState()->initializeFromShardIdentity(txn(), shardIdentity);
        ASSERT_EQ(ErrorCodes::ShutdownInProgress, status);
    }

    // ShardingState is now in error state, attempting to call it again will still result in error.

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) { return Status::OK(); });

    {
        auto status = shardingState()->initializeFromShardIdentity(txn(), shardIdentity);
        ASSERT_EQ(ErrorCodes::ManualInterventionRequired, status);
    }

    ASSERT_FALSE(shardingState()->enabled());
}

TEST_F(ShardingStateTest, InitializeAgainWithMatchingShardIdentitySucceeds) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithSameReplSetNameSucceeds) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString("config/b:2,c:3");
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentReplSetNameFails) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString("configRS/a:1,b:2");
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(txn(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentShardNameFails) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity2.setShardName("b");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(txn(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithPreviouslyUnsetClusterIdSucceeds) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentClusterIdFails) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity.setShardName("a");
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(txn(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString("config/a:1,b:2");
    shardIdentity2.setShardName("a");
    shardIdentity2.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(txn(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ("a", shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(txn()).toString());
}

}  // unnamed namespace
}  // namespace mongo
