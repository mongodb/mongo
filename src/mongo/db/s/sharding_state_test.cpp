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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/shard_server_catalog_cache_loader.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/sharding_mongod_test_fixture.h"

namespace mongo {

using executor::RemoteCommandRequest;

namespace {

class ShardingStateTest : public ShardingMongodTestFixture {
public:
    ShardingState* shardingState() {
        return &_shardingState;
    }

    std::string shardName() const {
        return _shardName.toString();
    }

protected:
    // Used to write to set up local collections before exercising server logic.
    std::unique_ptr<DBDirectClient> _dbDirectClient;

    void setUp() override {
        _shardName = ShardId("a");

        serverGlobalParams.clusterRole = ClusterRole::None;
        ShardingMongodTestFixture::setUp();

        // When sharding initialization is triggered, initialize sharding state as a shard server.
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        _shardingState.setGlobalInitMethodForTest([&](OperationContext* opCtx,
                                                      const ConnectionString& configConnStr,
                                                      StringData distLockProcessId) {
            auto status = initializeGlobalShardingStateForMongodForTest(configConnStr);
            if (!status.isOK()) {
                return status;
            }
            // Set the ConnectionString return value on the mock targeter so that later calls to
            // the targeter's getConnString() return the appropriate value.
            auto configTargeter =
                RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
            configTargeter->setConnectionStringReturnValue(configConnStr);
            configTargeter->setFindHostReturnValue(configConnStr.getServers()[0]);

            return Status::OK();
        });

        _dbDirectClient = stdx::make_unique<DBDirectClient>(operationContext());
    }

    void tearDown() override {
        _dbDirectClient.reset();

        // Some test cases modify the readOnly value, but the teardown calls below depend on
        // readOnly being false, so we reset the value here rather than in setUp().
        storageGlobalParams.readOnly = false;

        // ShardingState initialize can modify ReplicaSetMonitor state.
        ReplicaSetMonitor::cleanup();

        ShardingMongodTestFixture::tearDown();
    }

    std::unique_ptr<DistLockManager> makeDistLockManager(
        std::unique_ptr<DistLockCatalog> distLockCatalog) override {
        return stdx::make_unique<DistLockManagerMock>(nullptr);
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {
        invariant(distLockManager);
        return stdx::make_unique<ShardingCatalogClientImpl>(std::move(distLockManager));
    }

    std::unique_ptr<CatalogCacheLoader> makeCatalogCacheLoader() {
        return stdx::make_unique<ShardServerCatalogCacheLoader>(
            stdx::make_unique<ConfigServerCatalogCacheLoader>());
    }

    std::unique_ptr<CatalogCache> makeCatalogCache(
        std::unique_ptr<CatalogCacheLoader> catalogCacheLoader) {
        invariant(catalogCacheLoader);
        return stdx::make_unique<CatalogCache>(std::move(catalogCacheLoader));
    }

private:
    ShardingState _shardingState;
    ShardId _shardName;
};

TEST_F(ShardingStateTest, ValidShardIdentitySucceeds) {
    Lock::GlobalWrite lk(operationContext());

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));
    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ(shardName(), shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitWhilePreviouslyInErrorStateWillStayInErrorState) {
    // Must hold a lock to call initializeFromShardIdentity.
    Lock::GlobalWrite lk(operationContext());

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* opCtx, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::ShutdownInProgress, "shutting down"};
        });

    {
        auto status =
            shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity);
        ASSERT_EQ(ErrorCodes::ShutdownInProgress, status);
    }

    // ShardingState is now in error state, attempting to call it again will still result in error.

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* opCtx, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status::OK();
        });

    {
        auto status =
            shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity);
        ASSERT_EQ(ErrorCodes::ManualInterventionRequired, status);
    }

    ASSERT_FALSE(shardingState()->enabled());
}

TEST_F(ShardingStateTest, InitializeAgainWithMatchingShardIdentitySucceeds) {
    // Must hold a lock to call initializeFromShardIdentity.
    Lock::GlobalWrite lk(operationContext());

    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName(shardName());
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* opCtx, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ(shardName(), shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithSameReplSetNameSucceeds) {
    // Must hold a lock to call initializeFromShardIdentity.
    Lock::GlobalWrite lk(operationContext());

    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "b:2,c:3", "config"));
    shardIdentity2.setShardName(shardName());
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* opCtx, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ(shardName(), shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

// The below tests check for compatible startup parameters for --shardsvr, --overrideShardIdentity,
// and queryableBackup (readOnly) mode.

// readOnly and --shardsvr

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndShardServerAndNoOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = BSONObj();

    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, swShardingInitialized.getStatus().code());
}

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndShardServerAndInvalidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = BSON("_id"
                                                    << "shardIdentity"
                                                    << "configsvrConnectionString"
                                                    << "invalid");
    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, swShardingInitialized.getStatus().code());
}

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndShardServerAndValidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());
    ASSERT_OK(shardIdentity.validate());
    serverGlobalParams.overrideShardIdentity = shardIdentity.toBSON();

    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_OK(swShardingInitialized);
    ASSERT_TRUE(swShardingInitialized.getValue());
}

// readOnly and not --shardsvr

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndNotShardServerAndNoOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::None;
    serverGlobalParams.overrideShardIdentity = BSONObj();

    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_OK(swShardingInitialized);
    ASSERT_FALSE(swShardingInitialized.getValue());
}

TEST_F(
    ShardingStateTest,
    InitializeShardingAwarenessIfNeededReadOnlyAndNotShardServerAndInvalidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::None;
    serverGlobalParams.overrideShardIdentity = BSON("_id"
                                                    << "shardIdentity"
                                                    << "configsvrConnectionString"
                                                    << "invalid");

    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, swShardingInitialized.getStatus().code());
}

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededReadOnlyAndNotShardServerAndValidOverrideShardIdentity) {
    storageGlobalParams.readOnly = true;
    serverGlobalParams.clusterRole = ClusterRole::None;

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());
    ASSERT_OK(shardIdentity.validate());
    serverGlobalParams.overrideShardIdentity = shardIdentity.toBSON();

    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, swShardingInitialized.getStatus().code());
}

// not readOnly and --overrideShardIdentity

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndInvalidOverrideShardIdentity) {
    storageGlobalParams.readOnly = false;
    serverGlobalParams.overrideShardIdentity = BSON("_id"
                                                    << "shardIdentity"
                                                    << "configsvrConnectionString"
                                                    << "invalid");

    // Should error regardless of cluster role.

    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, swShardingInitialized.getStatus().code());

    serverGlobalParams.clusterRole = ClusterRole::None;
    swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, swShardingInitialized.getStatus().code());
}

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndValidOverrideShardIdentity) {
    storageGlobalParams.readOnly = false;

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());
    ASSERT_OK(shardIdentity.validate());
    serverGlobalParams.overrideShardIdentity = shardIdentity.toBSON();

    // Should error regardless of cluster role.

    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, swShardingInitialized.getStatus().code());

    serverGlobalParams.clusterRole = ClusterRole::None;
    swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, swShardingInitialized.getStatus().code());
}

// not readOnly and --shardsvr

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndShardServerAndNoShardIdentity) {
    storageGlobalParams.readOnly = false;
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = BSONObj();
    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_OK(swShardingInitialized);
    ASSERT_FALSE(swShardingInitialized.getValue());
}

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndShardServerAndInvalidShardIdentity) {

    replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY);

    // Insert the shardIdentity doc to disk before setting the clusterRole, since if the clusterRole
    // is ShardServer, the OpObserver for inserts will prevent the insert from occurring, since the
    // shardIdentity doc is invalid.
    serverGlobalParams.clusterRole = ClusterRole::None;
    BSONObj invalidShardIdentity = BSON("_id"
                                        << "shardIdentity"
                                        << "configsvrConnectionString"
                                        << "invalid");
    _dbDirectClient->insert(NamespaceString::kConfigCollectionNamespace.toString(),
                            invalidShardIdentity);

    storageGlobalParams.readOnly = false;
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = BSONObj();

    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_EQUALS(ErrorCodes::UnsupportedFormat, swShardingInitialized.getStatus().code());
}


TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndShardServerAndValidShardIdentity) {

    replicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY);

    // Insert the shardIdentity doc to disk before setting the clusterRole, since if the clusterRole
    // is ShardServer, the OpObserver for inserts will trigger sharding initialization from the
    // inserted doc.
    serverGlobalParams.clusterRole = ClusterRole::None;

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());
    ASSERT_OK(shardIdentity.validate());
    BSONObj validShardIdentity = shardIdentity.toBSON();

    _dbDirectClient->insert(NamespaceString::kConfigCollectionNamespace.toString(),
                            validShardIdentity);

    storageGlobalParams.readOnly = false;
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    serverGlobalParams.overrideShardIdentity = BSONObj();

    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_OK(swShardingInitialized);
    ASSERT_TRUE(swShardingInitialized.getValue());
}

// not readOnly and not --shardsvr

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndNotShardServerAndNoShardIdentity) {
    storageGlobalParams.readOnly = false;
    serverGlobalParams.clusterRole = ClusterRole::None;
    serverGlobalParams.overrideShardIdentity = BSONObj();
    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_OK(swShardingInitialized);
    ASSERT_FALSE(swShardingInitialized.getValue());
}

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndNotShardServerAndInvalidShardIdentity) {
    _dbDirectClient->insert(NamespaceString::kConfigCollectionNamespace.toString(),
                            BSON("_id"
                                 << "shardIdentity"
                                 << "configsvrConnectionString"
                                 << "invalid"));

    storageGlobalParams.readOnly = false;
    serverGlobalParams.clusterRole = ClusterRole::None;
    serverGlobalParams.overrideShardIdentity = BSONObj();

    // The shardIdentity doc on disk, even if invalid, is ignored if ClusterRole is None.
    // This is to allow fixing the shardIdentity doc by starting without --shardsvr.
    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_OK(swShardingInitialized);
    ASSERT_FALSE(swShardingInitialized.getValue());
}

TEST_F(ShardingStateTest,
       InitializeShardingAwarenessIfNeededNotReadOnlyAndNotShardServerAndValidShardIdentity) {
    storageGlobalParams.readOnly = false;
    serverGlobalParams.clusterRole = ClusterRole::None;
    serverGlobalParams.overrideShardIdentity = BSONObj();

    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());
    ASSERT_OK(shardIdentity.validate());
    BSONObj validShardIdentity = shardIdentity.toBSON();

    _dbDirectClient->insert(NamespaceString::kConfigCollectionNamespace.toString(),
                            validShardIdentity);

    // The shardIdentity doc on disk is ignored if ClusterRole is None.
    auto swShardingInitialized =
        shardingState()->initializeShardingAwarenessIfNeeded(operationContext());
    ASSERT_OK(swShardingInitialized);
    ASSERT_FALSE(swShardingInitialized.getValue());
}

}  // namespace
}  // namespace mongo
