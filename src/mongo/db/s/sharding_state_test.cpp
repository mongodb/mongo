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
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard_registry.h"
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

    void setupCollectionMetadata(const NamespaceString& nss,
                                 const OID& epoch,
                                 const std::vector<BSONObj>& initChunks) {
        auto future = launchAsync([this, &nss] {
            ChunkVersion latestShardVersion;
            Client::initThreadIfNotAlready();
            ASSERT_OK(
                shardingState()->refreshMetadataNow(operationContext(), nss, &latestShardVersion));
        });

        ChunkVersion initVersion(1, 0, epoch);
        onFindCommand([&nss, &initVersion](const RemoteCommandRequest&) {
            CollectionType coll;
            coll.setNs(nss);
            coll.setUpdatedAt(Date_t());
            coll.setEpoch(initVersion.epoch());
            coll.setKeyPattern(BSON("x" << 1));
            return std::vector<BSONObj>{coll.toBSON()};
        });

        onFindCommand([&initChunks](const RemoteCommandRequest&) { return initChunks; });

        future.timed_get(kFutureTimeout);
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
        _shardingState.setGlobalInitMethodForTest([&](OperationContext* txn,
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

private:
    ShardingState _shardingState;
    ShardId _shardName;
};

TEST_F(ShardingStateTest, ValidShardIdentitySucceeds) {
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
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::ShutdownInProgress, "shutting down"};
        });

    {
        auto status =
            shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity);
        ASSERT_EQ(ErrorCodes::ShutdownInProgress, status);
    }

    // ShardingState is now in error state, attempting to call it again will still result in error.

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
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
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ(shardName(), shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithSameReplSetNameSucceeds) {
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
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2));

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ(shardName(), shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentReplSetNameFails) {
    auto clusterID = OID::gen();
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(clusterID);

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "configRS"));
    shardIdentity2.setShardName(shardName());
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ(shardName(), shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentShardNameFails) {
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
    shardIdentity2.setShardName("b");
    shardIdentity2.setClusterId(clusterID);

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

    ASSERT_TRUE(shardingState()->enabled());
    ASSERT_EQ(shardName(), shardingState()->getShardName());
    ASSERT_EQ("config/a:1,b:2", shardingState()->getConfigServer(operationContext()).toString());
}

TEST_F(ShardingStateTest, InitializeAgainWithDifferentClusterIdFails) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    ShardIdentityType shardIdentity2;
    shardIdentity2.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity2.setShardName(shardName());
    shardIdentity2.setClusterId(OID::gen());

    shardingState()->setGlobalInitMethodForTest(
        [](OperationContext* txn, const ConnectionString& connStr, StringData distLockProcessId) {
            return Status{ErrorCodes::InternalError, "should not reach here"};
        });

    auto status = shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity2);
    ASSERT_EQ(ErrorCodes::InconsistentShardIdentity, status);

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

TEST_F(ShardingStateTest, MetadataRefreshShouldUseDiffQuery) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    const NamespaceString nss("test.user");
    const OID initEpoch(OID::gen());

    {
        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 0));
        chunk.setMax(BSON("x" << 10));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(2, 0, initEpoch));
        setupCollectionMetadata(nss, initEpoch, std::vector<BSONObj>{chunk.toBSON()});
    }

    const ChunkVersion newVersion(3, 0, initEpoch);
    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_OK(shardingState()->onStaleShardVersion(operationContext(), nss, newVersion));
    });

    onFindCommand([&nss, &initEpoch](const RemoteCommandRequest&) {
        CollectionType coll;
        coll.setNs(nss);
        coll.setUpdatedAt(Date_t());
        coll.setEpoch(initEpoch);
        coll.setKeyPattern(BSON("x" << 1));
        return std::vector<BSONObj>{coll.toBSON()};
    });

    onFindCommand([this, &nss, &initEpoch](const RemoteCommandRequest& request) {
        auto diffQueryStatus = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
        ASSERT_OK(diffQueryStatus.getStatus());

        auto diffQuery = std::move(diffQueryStatus.getValue());
        ASSERT_BSONOBJ_EQ(BSON("ns" << nss.ns() << "lastmod" << BSON("$gte" << Timestamp(2, 0))),
                          diffQuery->getFilter());

        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 10));
        chunk.setMax(BSON("x" << 20));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(3, 10, initEpoch));
        return std::vector<BSONObj>{chunk.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

/**
 * Test where the epoch changed right before the chunk diff query.
 */
TEST_F(ShardingStateTest, MetadataRefreshShouldUseFullQueryOnEpochMismatch) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    const NamespaceString nss("test.user");
    const OID initEpoch(OID::gen());

    {
        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 0));
        chunk.setMax(BSON("x" << 10));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(2, 0, initEpoch));
        setupCollectionMetadata(nss, initEpoch, std::vector<BSONObj>{chunk.toBSON()});
    }


    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_OK(shardingState()->onStaleShardVersion(
            operationContext(), nss, ChunkVersion(3, 0, initEpoch)));
    });

    onFindCommand([&nss, &initEpoch](const RemoteCommandRequest&) {
        CollectionType coll;
        coll.setNs(nss);
        coll.setUpdatedAt(Date_t());
        coll.setEpoch(initEpoch);
        coll.setKeyPattern(BSON("x" << 1));
        return std::vector<BSONObj>{coll.toBSON()};
    });

    // Now when the diff query is performed, it will get chunks with a different epoch.
    const ChunkVersion newVersion(3, 0, OID::gen());
    onFindCommand([this, &nss, &newVersion](const RemoteCommandRequest& request) {
        auto diffQueryStatus = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
        ASSERT_OK(diffQueryStatus.getStatus());

        auto diffQuery = std::move(diffQueryStatus.getValue());
        ASSERT_BSONOBJ_EQ(BSON("ns" << nss.ns() << "lastmod" << BSON("$gte" << Timestamp(2, 0))),
                          diffQuery->getFilter());

        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 10));
        chunk.setMax(BSON("x" << 20));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(3, 10, newVersion.epoch()));
        return std::vector<BSONObj>{chunk.toBSON()};
    });

    // Retry the refresh again. Now doing a full reload.

    onFindCommand([&nss, &newVersion](const RemoteCommandRequest&) {
        CollectionType coll;
        coll.setNs(nss);
        coll.setUpdatedAt(Date_t());
        coll.setEpoch(newVersion.epoch());
        coll.setKeyPattern(BSON("x" << 1));
        return std::vector<BSONObj>{coll.toBSON()};
    });

    onFindCommand([this, &nss, &newVersion](const RemoteCommandRequest& request) {
        auto diffQueryStatus = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
        ASSERT_OK(diffQueryStatus.getStatus());

        auto diffQuery = std::move(diffQueryStatus.getValue());
        ASSERT_BSONOBJ_EQ(BSON("ns" << nss.ns() << "lastmod" << BSON("$gte" << Timestamp(0, 0))),
                          diffQuery->getFilter());

        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 10));
        chunk.setMax(BSON("x" << 20));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(3, 10, newVersion.epoch()));
        return std::vector<BSONObj>{chunk.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingStateTest, FullMetadataOnEpochMismatchShouldStopAfterMaxRetries) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    const NamespaceString nss("test.user");
    const OID initEpoch(OID::gen());

    {
        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 0));
        chunk.setMax(BSON("x" << 10));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(2, 0, initEpoch));
        setupCollectionMetadata(nss, initEpoch, std::vector<BSONObj>{chunk.toBSON()});
    }


    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        auto status = shardingState()->onStaleShardVersion(
            operationContext(), nss, ChunkVersion(3, 0, initEpoch));
        ASSERT_EQ(ErrorCodes::RemoteChangeDetected, status);
    });

    OID lastEpoch(initEpoch);
    OID nextEpoch(OID::gen());
    for (int tries = 0; tries < 3; tries++) {
        onFindCommand([&nss, &lastEpoch](const RemoteCommandRequest&) {
            CollectionType coll;
            coll.setNs(nss);
            coll.setUpdatedAt(Date_t());
            coll.setEpoch(lastEpoch);
            coll.setKeyPattern(BSON("x" << 1));
            return std::vector<BSONObj>{coll.toBSON()};
        });

        onFindCommand([this, &nss, &nextEpoch, tries](const RemoteCommandRequest& request) {
            auto diffQueryStatus = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
            ASSERT_OK(diffQueryStatus.getStatus());

            auto diffQuery = std::move(diffQueryStatus.getValue());
            Timestamp expectedLastMod = (tries == 0) ? Timestamp(2, 0) : Timestamp(0, 0);
            ASSERT_BSONOBJ_EQ(
                BSON("ns" << nss.ns() << "lastmod" << BSON("$gte" << expectedLastMod)),
                diffQuery->getFilter());

            ChunkType chunk;
            chunk.setNS(nss.ns());
            chunk.setMin(BSON("x" << 10));
            chunk.setMax(BSON("x" << 20));
            chunk.setShard(ShardId(shardName()));
            chunk.setVersion(ChunkVersion(3, 10, nextEpoch));
            return std::vector<BSONObj>{chunk.toBSON()};
        });

        lastEpoch = nextEpoch;
        nextEpoch = OID::gen();
    }

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingStateTest, MetadataRefreshShouldBeOkWhenCollectionWasDropped) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    const NamespaceString nss("test.user");
    const OID initEpoch(OID::gen());

    {
        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 0));
        chunk.setMax(BSON("x" << 10));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(2, 0, initEpoch));
        setupCollectionMetadata(nss, initEpoch, std::vector<BSONObj>{chunk.toBSON()});
    }

    const ChunkVersion newVersion(3, 0, initEpoch);
    auto future = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_OK(shardingState()->onStaleShardVersion(operationContext(), nss, newVersion));
    });

    onFindCommand([&nss, &initEpoch](const RemoteCommandRequest&) {
        CollectionType coll;
        coll.setNs(nss);
        coll.setUpdatedAt(Date_t());
        coll.setEpoch(initEpoch);
        coll.setKeyPattern(BSON("x" << 1));
        coll.setDropped(true);
        return std::vector<BSONObj>{coll.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingStateTest, MetadataRefreshShouldNotRetryOtherTypesOfError) {
    ShardIdentityType shardIdentity;
    shardIdentity.setConfigsvrConnString(
        ConnectionString(ConnectionString::SET, "a:1,b:2", "config"));
    shardIdentity.setShardName(shardName());
    shardIdentity.setClusterId(OID::gen());

    ASSERT_OK(shardingState()->initializeFromShardIdentity(operationContext(), shardIdentity));

    const NamespaceString nss("test.user");
    const OID initEpoch(OID::gen());

    {
        ChunkType chunk;
        chunk.setNS(nss.ns());
        chunk.setMin(BSON("x" << 0));
        chunk.setMax(BSON("x" << 10));
        chunk.setShard(ShardId(shardName()));
        chunk.setVersion(ChunkVersion(2, 0, initEpoch));
        setupCollectionMetadata(nss, initEpoch, std::vector<BSONObj>{chunk.toBSON()});
    }

    auto configTargeter =
        RemoteCommandTargeterMock::get(shardRegistry()->getConfigShard()->getTargeter());
    configTargeter->setFindHostReturnValue({ErrorCodes::HostNotFound, "host erased by test"});

    auto status = shardingState()->onStaleShardVersion(
        operationContext(), nss, ChunkVersion(3, 0, initEpoch));
    ASSERT_EQ(ErrorCodes::HostNotFound, status);
}

}  // namespace
}  // namespace mongo
