/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/config_server_test_fixture.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/s/write_ops/batched_update_document.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::vector;
using unittest::assertGet;

const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(rpc::ServerSelectionMetadata(true, boost::none).toBSON());
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}()};

class AddShardTest : public ConfigServerTestFixture {
protected:
    /**
     * Performs the test setup steps from the parent class and then configures the config shard and
     * the client name.
     */
    void setUp() override {
        ConfigServerTestFixture::setUp();

        // Make sure clusterID is written to the config.version collection.
        ASSERT_OK(catalogManager()->initializeConfigDatabaseIfNeeded(operationContext()));

        auto clusterIdLoader = ClusterIdentityLoader::get(operationContext());
        ASSERT_OK(clusterIdLoader->loadClusterId(operationContext(),
                                                 repl::ReadConcernLevel::kLocalReadConcern));
        _clusterId = clusterIdLoader->getClusterId();
    }

    /**
     * addShard validates the host as a shard. It calls "isMaster" on the host to determine what
     * kind of host it is -- mongos, regular mongod, config mongod -- and whether the replica set
     * details are correct. "isMasterResponse" defines the response of the "isMaster" request and
     * should be a command response BSONObj, or a failed Status.
     *
     * ShardingTestFixture::expectGetShards() should be called before this function, otherwise
     * addShard will never reach the isMaster command -- a find query is called first.
     */
    void expectIsMaster(const HostAndPort& target, StatusWith<BSONObj> isMasterResponse) {
        onCommandForAddShard([&, target, isMasterResponse](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("isMaster" << 1));
            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            return isMasterResponse;
        });
    }

    void expectListDatabases(const HostAndPort& target, const std::vector<BSONObj>& dbs) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("listDatabases" << 1));
            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            BSONArrayBuilder arr;
            for (const auto& db : dbs) {
                arr.append(db);
            }

            return BSON("ok" << 1 << "databases" << arr.obj());
        });
    }

    /**
     * Waits for a request for the shardIdentity document to be upserted into a shard from the
     * config server on addShard.
     */
    void expectShardIdentityUpsert(const HostAndPort& expectedHost,
                                   const std::string& expectedShardName) {
        // Create the expected upsert shardIdentity command for this shardType.
        auto upsertCmdObj = catalogManager()->createShardIdentityUpsertForAddShard(
            operationContext(), expectedShardName);

        // Get the BatchedUpdateRequest from the upsert command.
        BatchedCommandRequest request(BatchedCommandRequest::BatchType::BatchType_Update);
        std::string errMsg;
        invariant(request.parseBSON("admin", upsertCmdObj, &errMsg) || !request.isValid(&errMsg));

        expectUpdates(expectedHost,
                      NamespaceString(NamespaceString::kConfigCollectionNamespace),
                      request.getUpdateRequest());
    }

    /**
     * Waits for a set of batched updates and ensures that the host, namespace, and updates exactly
     * match what's expected. Responds with a success status.
     */
    void expectUpdates(const HostAndPort& expectedHost,
                       const NamespaceString& expectedNss,
                       BatchedUpdateRequest* expectedBatchedUpdates) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {

            ASSERT_EQUALS(expectedHost, request.target);

            // Check that the db name in the request matches the expected db name.
            ASSERT_EQUALS(expectedNss.db(), request.dbname);

            BatchedUpdateRequest actualBatchedUpdates;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedUpdates.parseBSON(request.dbname, request.cmdObj, &errmsg));

            // Check that the db and collection names in the BatchedUpdateRequest match the
            // expected.
            ASSERT_EQUALS(expectedNss, actualBatchedUpdates.getNS());

            auto expectedUpdates = expectedBatchedUpdates->getUpdates();
            auto actualUpdates = actualBatchedUpdates.getUpdates();

            ASSERT_EQUALS(expectedUpdates.size(), actualUpdates.size());

            auto itExpected = expectedUpdates.begin();
            auto itActual = actualUpdates.begin();

            for (; itActual != actualUpdates.end(); itActual++, itExpected++) {
                ASSERT_EQ((*itExpected)->getUpsert(), (*itActual)->getUpsert());
                ASSERT_EQ((*itExpected)->getMulti(), (*itActual)->getMulti());
                ASSERT_EQ((*itExpected)->getQuery(), (*itActual)->getQuery());
                ASSERT_EQ((*itExpected)->getUpdateExpr(), (*itActual)->getUpdateExpr());
            }

            BatchedCommandResponse response;
            response.setOk(true);
            response.setNModified(1);

            return response.toBSON();
        });
    }

    /**
     * Asserts that a document exists in the config server's config.shards collection corresponding
     * to 'expectedShard'.
     */
    void assertShardExists(const ShardType& expectedShard) {
        auto foundShard = assertGet(getShardDoc(operationContext(), expectedShard.getName()));

        ASSERT_EQUALS(expectedShard.getName(), foundShard.getName());
        ASSERT_EQUALS(expectedShard.getHost(), foundShard.getHost());
        ASSERT_EQUALS(expectedShard.getMaxSizeMB(), foundShard.getMaxSizeMB());
        ASSERT_EQUALS(expectedShard.getDraining(), foundShard.getDraining());
        ASSERT_EQUALS((int)expectedShard.getState(), (int)foundShard.getState());
        ASSERT_TRUE(foundShard.getTags().empty());
    }

    /**
     * Asserts that a document exists in the config server's config.databases collection
     * corresponding to 'expectedDB'.
     */
    void assertDatabaseExists(const DatabaseType& expectedDB) {
        auto foundDB =
            assertGet(catalogClient()->getDatabase(operationContext(), expectedDB.getName())).value;

        ASSERT_EQUALS(expectedDB.getName(), foundDB.getName());
        ASSERT_EQUALS(expectedDB.getPrimary(), foundDB.getPrimary());
        ASSERT_EQUALS(expectedDB.getSharded(), foundDB.getSharded());
    }

    /**
     * Asserts that a document exists in the config server's config.changelog collection
     * describing the addShard request for 'addedShard'.
     */
    void assertChangeWasLogged(const ShardType& addedShard) {
        auto response = assertGet(
            getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                     ReadPreferenceSetting{
                                                         ReadPreference::PrimaryOnly},
                                                     repl::ReadConcernLevel::kLocalReadConcern,
                                                     NamespaceString("config.changelog"),
                                                     BSON("what"
                                                          << "addShard"
                                                          << "details.name"
                                                          << addedShard.getName()),
                                                     BSONObj(),
                                                     1));
        ASSERT_EQ(1U, response.docs.size());
        auto logEntryBSON = response.docs.front();
        auto logEntry = assertGet(ChangeLogType::fromBSON(logEntryBSON));

        ASSERT_EQUALS(addedShard.getName(), logEntry.getDetails()["name"].String());
        ASSERT_EQUALS(addedShard.getHost(), logEntry.getDetails()["host"].String());
    }

    OID _clusterId;
};

TEST_F(AddShardTest, CreateShardIdentityUpsertForAddShard) {
    std::string shardName = "shardName";

    BSONObj expectedBSON = BSON("update"
                                << "system.version"
                                << "updates"
                                << BSON_ARRAY(BSON(
                                       "q"
                                       << BSON("_id"
                                               << "shardIdentity"
                                               << "shardName"
                                               << shardName
                                               << "clusterId"
                                               << _clusterId)
                                       << "u"
                                       << BSON("$set" << BSON(
                                                   "configsvrConnectionString"
                                                   << getConfigShard()->getConnString().toString()))
                                       << "upsert"
                                       << true))
                                << "writeConcern"
                                << BSON("w"
                                        << "majority"
                                        << "wtimeout"
                                        << 15000));
    ASSERT_EQUALS(
        expectedBSON,
        catalogManager()->createShardIdentityUpsertForAddShard(operationContext(), shardName));
}

TEST_F(AddShardTest, StandaloneBasicSuccess) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));


    std::string expectedShardName = "StandaloneShard";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setMaxSizeMB(100);
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1;
    discoveredDB1.setName("TestDB1");
    discoveredDB1.setPrimary(ShardId("StandaloneShard"));
    discoveredDB1.setSharded(false);

    DatabaseType discoveredDB2;
    discoveredDB2.setName("TestDB2");
    discoveredDB2.setPrimary(ShardId("StandaloneShard"));
    discoveredDB2.setSharded(false);

    auto future = launchAsync([this, expectedShardName] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk"
                                  << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kFutureTimeout);

    // Ensure that the shard document was properly added to config.shards
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB1);
    assertDatabaseExists(discoveredDB2);

    assertChangeWasLogged(expectedShard);
}

TEST_F(AddShardTest, StandaloneGenerateName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));

    ShardType existingShard;
    existingShard.setName("shard0005");
    existingShard.setHost("existingHost:12345");
    existingShard.setMaxSizeMB(100);
    existingShard.setState(ShardType::ShardState::kShardAware);

    // Add a pre-existing shard so when generating a name for the new shard it will have to go
    // higher than the existing one.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    ShardType::ConfigNS,
                                                    existingShard.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertShardExists(existingShard);

    std::string expectedShardName = "shard0006";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(shardTarget.toString());
    expectedShard.setMaxSizeMB(100);
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1;
    discoveredDB1.setName("TestDB1");
    discoveredDB1.setPrimary(ShardId(expectedShardName));
    discoveredDB1.setSharded(false);

    DatabaseType discoveredDB2;
    discoveredDB2.setName("TestDB2");
    discoveredDB2.setPrimary(ShardId(expectedShardName));
    discoveredDB2.setSharded(false);

    auto future = launchAsync([this, &expectedShardName, &shardTarget] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(catalogManager()->addShard(
            operationContext(), nullptr, ConnectionString(shardTarget), 100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk"
                                  << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kFutureTimeout);

    // Ensure that the shard document was properly added to config.shards
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB1);
    assertDatabaseExists(discoveredDB2);

    assertChangeWasLogged(expectedShard);
}

TEST_F(AddShardTest, AddSCCCConnectionStringAsShard) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    auto invalidConn =
        ConnectionString("host1:12345,host2:12345,host3:12345", ConnectionString::INVALID);
    targeter->setConnectionStringReturnValue(invalidConn);

    auto future = launchAsync([this, invalidConn] {
        const std::string shardName("StandaloneShard");
        auto status = catalogManager()->addShard(operationContext(), &shardName, invalidConn, 100);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "Invalid connection string");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, EmptyShardName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    std::string expectedShardName = "";

    auto future = launchAsync([this, expectedShardName] {
        auto status =
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_EQUALS("shard name cannot be empty", status.getStatus().reason());
    });

    future.timed_get(kFutureTimeout);
}

// Host is unreachable, cannot verify host.
TEST_F(AddShardTest, UnreachableHost) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, &expectedShardName, &shardTarget] {
        Client::initThreadIfNotAlready();
        auto status = catalogManager()->addShard(
            operationContext(), &expectedShardName, ConnectionString(shardTarget), 100);
        ASSERT_EQUALS(ErrorCodes::HostUnreachable, status);
        ASSERT_EQUALS("host unreachable", status.getStatus().reason());
    });

    Status hostUnreachableStatus = Status(ErrorCodes::HostUnreachable, "host unreachable");
    expectIsMaster(shardTarget, hostUnreachableStatus);

    future.timed_get(kFutureTimeout);
}

// Cannot add mongos as a shard.
TEST_F(AddShardTest, AddMongosAsShard) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, &expectedShardName, &shardTarget] {
        Client::initThreadIfNotAlready();
        auto status = catalogManager()->addShard(
            operationContext(), &expectedShardName, ConnectionString(shardTarget), 100);
        ASSERT_EQUALS(ErrorCodes::RPCProtocolNegotiationFailed, status);
    });

    Status rpcProtocolNegFailedStatus =
        Status(ErrorCodes::RPCProtocolNegotiationFailed, "Unable to communicate");
    expectIsMaster(shardTarget, rpcProtocolNegFailedStatus);

    future.timed_get(kFutureTimeout);
}

// A replica set name was found for the host but no name was provided with the host.
TEST_F(AddShardTest, AddReplicaSetShardAsStandalone) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget = HostAndPort("host1:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "Standalone";

    auto future = launchAsync([this, expectedShardName, shardTarget] {
        Client::initThreadIfNotAlready();
        auto status = catalogManager()->addShard(
            operationContext(), &expectedShardName, ConnectionString(shardTarget), 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "use replica set url format");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "myOtherSet");
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kFutureTimeout);
}

// A replica set name was provided with the host but no name was found for the host.
TEST_F(AddShardTest, AddStandaloneHostShardAsReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setConnectionStringReturnValue(connString);
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        Client::initThreadIfNotAlready();
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host did not return a set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kFutureTimeout);
}

// Provided replica set name does not match found replica set name.
TEST_F(AddShardTest, ReplicaSetMistmatchedReplicaSetName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        Client::initThreadIfNotAlready();
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "does not match the actual set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "myOtherSet");
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kFutureTimeout);
}

// Cannot add config server as a shard.
TEST_F(AddShardTest, ShardIsCSRSConfigServer) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("config/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        Client::initThreadIfNotAlready();
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "as a shard since it is a config server");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "config"
                                        << "configsvr"
                                        << true);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kFutureTimeout);
}

// One of the hosts is not part of the found replica set.
TEST_F(AddShardTest, ReplicaSetMissingHostsProvidedInSeedList) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        Client::initThreadIfNotAlready();
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "host2:12345 does not belong to replica set");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kFutureTimeout);
}

// Cannot add a shard with the shard name "config".
TEST_F(AddShardTest, AddShardWithNameConfigFails) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "config";

    auto future = launchAsync([this, expectedShardName, connString] {
        Client::initThreadIfNotAlready();
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_EQUALS(status.getStatus().reason(),
                      "use of shard replica set with name 'config' is not allowed");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, ShardContainsExistingDatabase) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "mySet";

    DatabaseType existingDB;
    existingDB.setName("existing");
    existingDB.setPrimary(ShardId("existingShard"));
    existingDB.setSharded(false);

    // Add a pre-existing database.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    DatabaseType::ConfigNS,
                                                    existingDB.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertDatabaseExists(existingDB);


    auto future = launchAsync([this, expectedShardName, connString] {
        Client::initThreadIfNotAlready();
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(
            status.getStatus().reason(),
            "because a local database 'existing' exists in another existingShard");
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectIsMaster(shardTarget, commandResponse);

    expectListDatabases(shardTarget, {BSON("name" << existingDB.getName())});

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, SuccessfullyAddReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));

    std::string expectedShardName = "mySet";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(connString.toString());
    expectedShard.setMaxSizeMB(100);
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB;
    discoveredDB.setName("shardDB");
    discoveredDB.setPrimary(ShardId(expectedShardName));
    discoveredDB.setSharded(false);

    auto future = launchAsync([this, &expectedShardName, &connString] {
        Client::initThreadIfNotAlready();
        auto shardName =
            assertGet(catalogManager()->addShard(operationContext(), nullptr, connString, 100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget, std::vector<BSONObj>{BSON("name" << discoveredDB.getName())});

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kFutureTimeout);

    // Ensure that the shard document was properly added to config.shards
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB);

    assertChangeWasLogged(expectedShard);
}

TEST_F(AddShardTest, ReplicaSetExtraHostsDiscovered) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString seedString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    ConnectionString fullConnString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345,host3:12345"));
    targeter->setConnectionStringReturnValue(fullConnString);
    HostAndPort shardTarget = seedString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(seedString, std::move(targeter));

    std::string expectedShardName = "mySet";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(fullConnString.toString());
    expectedShard.setMaxSizeMB(100);
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB;
    discoveredDB.setName("shardDB");
    discoveredDB.setPrimary(ShardId(expectedShardName));
    discoveredDB.setSharded(false);

    auto future = launchAsync([this, &expectedShardName, &seedString] {
        Client::initThreadIfNotAlready();
        auto shardName =
            assertGet(catalogManager()->addShard(operationContext(), nullptr, seedString, 100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget, std::vector<BSONObj>{BSON("name" << discoveredDB.getName())});

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kFutureTimeout);

    // Ensure that the shard document was properly added to config.shards
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were properly added to config.database.
    assertDatabaseExists(discoveredDB);

    // The changelog entry uses whatever connection string is passed to addShard, even if addShard
    // discovered additional hosts.
    expectedShard.setHost(seedString.toString());
    assertChangeWasLogged(expectedShard);
}

TEST_F(AddShardTest, AddShardSucceedsEvenIfAddingDBsFromNewShardFails) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));


    std::string expectedShardName = "StandaloneShard";

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setMaxSizeMB(100);
    expectedShard.setState(ShardType::ShardState::kShardAware);

    DatabaseType discoveredDB1;
    discoveredDB1.setName("TestDB1");
    discoveredDB1.setPrimary(ShardId("StandaloneShard"));
    discoveredDB1.setSharded(false);

    DatabaseType discoveredDB2;
    discoveredDB2.setName("TestDB2");
    discoveredDB2.setPrimary(ShardId("StandaloneShard"));
    discoveredDB2.setSharded(false);

    // Enable fail point to cause all updates to fail.  Since we add the databases detected from
    // the shard being added with upserts, but we add the shard document itself via insert, this
    // will allow the shard to be added but prevent the databases from brought into the cluster.
    auto failPoint = getGlobalFailPointRegistry()->getFailPoint("failAllUpdates");
    ASSERT(failPoint);
    failPoint->setMode(FailPoint::alwaysOn);
    ON_BLOCK_EXIT([&] { failPoint->setMode(FailPoint::off); });

    auto future = launchAsync([this, &expectedShardName, &shardTarget] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(catalogManager()->addShard(
            operationContext(), &expectedShardName, ConnectionString(shardTarget), 100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(
        shardTarget,
        std::vector<BSONObj>{BSON("name"
                                  << "local"
                                  << "sizeOnDisk"
                                  << 1000),
                             BSON("name" << discoveredDB1.getName() << "sizeOnDisk" << 2000),
                             BSON("name" << discoveredDB2.getName() << "sizeOnDisk" << 5000)});

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kFutureTimeout);

    // Ensure that the shard document was properly added to config.shards
    assertShardExists(expectedShard);

    // Ensure that the databases detected from the shard were *not* added.
    ASSERT_EQUALS(
        ErrorCodes::NamespaceNotFound,
        catalogClient()->getDatabase(operationContext(), discoveredDB1.getName()).getStatus());
    ASSERT_EQUALS(
        ErrorCodes::NamespaceNotFound,
        catalogClient()->getDatabase(operationContext(), discoveredDB2.getName()).getStatus());

    assertChangeWasLogged(expectedShard);
}

/*
TODO(SERVER-24213): Add back tests around adding shard that already exists.
// Host is already part of an existing shard.
TEST_F(AddShardTest, AddExistingShardStandalone) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget = HostAndPort("host1:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, shardTarget] {
        auto status =
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse(shardTarget.toString())),
                                       100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "is already a member of the existing shard");
    });

    ShardType shard;
    shard.setName("shard0000");
    shard.setHost(shardTarget.toString());
    expectGetShards({shard});

    future.timed_get(kFutureTimeout);
}


// Host is already part of an existing replica set shard.
TEST_F(AddShardTest, AddExistingShardReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setConnectionStringReturnValue(connString);
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "is already a member of the existing shard");
    });

    ShardType shard;
    shard.setName("shard0000");
    shard.setHost(shardTarget.toString());
    expectGetShards({shard});

    future.timed_get(kFutureTimeout);
}

// TODO(SERVER-24213): Test adding a new shard with an existing shard name, but different
// shard membership
TEST_F(AddShardTest, ReAddExistingShard) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "mySet";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_OK(status);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectIsMaster(shardTarget, commandResponse);

    expectListDatabases(shardTarget,
                        {BSON("name"
                              << "shardDB")});

    expectGetDatabase("shardDB", boost::none);

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType newShard;
    newShard.setName(expectedShardName);
    newShard.setMaxSizeMB(100);
    newShard.setHost(connString.toString());
    newShard.setState(ShardType::ShardState::kShardAware);

    // When a shard with the same name already exists, the insert into config.shards will fail
    // with a duplicate key error on the shard name.
    onCommand([&newShard](const RemoteCommandRequest& request) {
        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));

        ASSERT_EQUALS(ShardType::ConfigNS, actualBatchedInsert.getNS().toString());

        auto inserted = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(1U, inserted.size());

        ASSERT_EQ(newShard.toBSON(), inserted.front());

        BatchedCommandResponse response;
        response.setOk(false);
        response.setErrCode(ErrorCodes::DuplicateKey);
        response.setErrMessage("E11000 duplicate key error collection: config.shards");

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}
*/

}  // namespace
}  // namespace mongo
