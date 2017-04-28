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
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
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
    o.appendElements(ReadPreferenceSetting::secondaryPreferredMetadata());
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}()};

// TODO (SERVER-27029): This value was chosen to be greater than the time it takes for the hang
// analyzer to kick in. Remove once the cause for the test failure has been figured out.
const Hours kLongFutureTimeout(8);

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
            ASSERT_BSONOBJ_EQ(request.cmdObj, BSON("isMaster" << 1));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

            return isMasterResponse;
        });
    }

    void expectListDatabases(const HostAndPort& target, const std::vector<BSONObj>& dbs) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_BSONOBJ_EQ(request.cmdObj, BSON("listDatabases" << 1 << "nameOnly" << true));
            ASSERT_BSONOBJ_EQ(rpc::makeEmptyMetadata(), request.metadata);

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
    void expectShardIdentityUpsertReturnSuccess(const HostAndPort& expectedHost,
                                                const std::string& expectedShardName) {
        // Create the expected upsert shardIdentity command for this shardType.
        auto upsertCmdObj = catalogManager()->createShardIdentityUpsertForAddShard(
            operationContext(), expectedShardName);

        // Get the BatchedUpdateRequest from the upsert command.
        BatchedCommandRequest request(BatchedCommandRequest::BatchType::BatchType_Update);
        std::string errMsg;
        invariant(request.parseBSON("admin", upsertCmdObj, &errMsg) || !request.isValid(&errMsg));

        expectUpdatesReturnSuccess(expectedHost,
                                   NamespaceString(NamespaceString::kConfigCollectionNamespace),
                                   request.getUpdateRequest());
    }

    void expectShardIdentityUpsertReturnFailure(const HostAndPort& expectedHost,
                                                const std::string& expectedShardName,
                                                const Status& statusToReturn) {
        // Create the expected upsert shardIdentity command for this shardType.
        auto upsertCmdObj = catalogManager()->createShardIdentityUpsertForAddShard(
            operationContext(), expectedShardName);

        // Get the BatchedUpdateRequest from the upsert command.
        BatchedCommandRequest request(BatchedCommandRequest::BatchType::BatchType_Update);
        std::string errMsg;
        invariant(request.parseBSON("admin", upsertCmdObj, &errMsg) || !request.isValid(&errMsg));

        expectUpdatesReturnFailure(expectedHost,
                                   NamespaceString(NamespaceString::kConfigCollectionNamespace),
                                   request.getUpdateRequest(),
                                   statusToReturn);
    }

    /**
     * Waits for a set of batched updates and ensures that the host, namespace, and updates exactly
     * match what's expected. Responds with a success status.
     */
    void expectUpdatesReturnSuccess(const HostAndPort& expectedHost,
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
                ASSERT_BSONOBJ_EQ((*itExpected)->getQuery(), (*itActual)->getQuery());
                ASSERT_BSONOBJ_EQ((*itExpected)->getUpdateExpr(), (*itActual)->getUpdateExpr());
            }

            BatchedCommandResponse response;
            response.setOk(true);
            response.setNModified(1);

            return response.toBSON();
        });
    }

    /**
     * Waits for a set of batched updates and ensures that the host, namespace, and updates exactly
     * match what's expected. Responds with a failure status.
     */
    void expectUpdatesReturnFailure(const HostAndPort& expectedHost,
                                    const NamespaceString& expectedNss,
                                    BatchedUpdateRequest* expectedBatchedUpdates,
                                    const Status& statusToReturn) {
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
                ASSERT_BSONOBJ_EQ((*itExpected)->getQuery(), (*itActual)->getQuery());
                ASSERT_BSONOBJ_EQ((*itExpected)->getUpdateExpr(), (*itActual)->getUpdateExpr());
            }

            return statusToReturn;
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

    void forwardAddShardNetwork(Date_t when) {
        networkForAddShard()->enterNetwork();
        networkForAddShard()->runUntil(when);
        networkForAddShard()->exitNetwork();
    }

    OID _clusterId;
};

TEST_F(AddShardTest, CreateShardIdentityUpsertForAddShard) {
    std::string shardName = "shardName";

    BSONObj expectedBSON = BSON("update"
                                << "system.version"
                                << "updates"
                                << BSON_ARRAY(
                                       BSON("q" << BSON("_id"
                                                        << "shardIdentity"
                                                        << "shardName"
                                                        << shardName
                                                        << "clusterId"
                                                        << _clusterId)
                                                << "u"
                                                << BSON("$set" << BSON("configsvrConnectionString"
                                                                       << replicationCoordinator()
                                                                              ->getConfig()
                                                                              .getConnectionString()
                                                                              .toString()))
                                                << "upsert"
                                                << true))
                                << "writeConcern"
                                << BSON("w"
                                        << "majority"
                                        << "wtimeout"
                                        << 15000));
    ASSERT_BSONOBJ_EQ(
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

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
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

    // The shardIdentity doc inserted into the admin.system.version collection on the shard.
    expectShardIdentityUpsertReturnSuccess(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
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

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
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

    // The shardIdentity doc inserted into the admin.system.version collection on the shard.
    expectShardIdentityUpsertReturnSuccess(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
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

    future.timed_get(kLongFutureTimeout);
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

    future.timed_get(kLongFutureTimeout);
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
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host unreachable");
    });

    Status hostUnreachableStatus = Status(ErrorCodes::HostUnreachable, "host unreachable");
    expectIsMaster(shardTarget, hostUnreachableStatus);

    future.timed_get(kLongFutureTimeout);
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

    future.timed_get(kLongFutureTimeout);
}

// Attempt to add a pre-v3.4 mongod.
TEST_F(AddShardTest, AddVersion32Shard) {
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
        ASSERT_EQUALS(ErrorCodes::IncompatibleServerVersion, status);
    });

    // The maxWireVersion indicates that this is a v3.2 shard.
    BSONObj commandResponse =
        BSON("ok" << 1 << "ismaster" << true << "maxWireVersion" << WireVersion::FIND_COMMAND);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
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
                                        << "myOtherSet"
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
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

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
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
                                        << "myOtherSet"
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
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
                                        << true
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
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
                                        << hosts.arr()
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
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
                                        << hosts.arr()
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    future.timed_get(kLongFutureTimeout);
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
                                        << hosts.arr()
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    expectListDatabases(shardTarget, {BSON("name" << existingDB.getName())});

    future.timed_get(kLongFutureTimeout);
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
                                        << hosts.arr()
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget, std::vector<BSONObj>{BSON("name" << discoveredDB.getName())});

    // The shardIdentity doc inserted into the admin.system.version collection on the shard.
    expectShardIdentityUpsertReturnSuccess(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
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
                                        << hosts.arr()
                                        << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
    expectIsMaster(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget, std::vector<BSONObj>{BSON("name" << discoveredDB.getName())});

    // The shardIdentity doc inserted into the admin.system.version collection on the shard.
    expectShardIdentityUpsertReturnSuccess(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
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

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "maxWireVersion"
                                        << WireVersion::COMMANDS_ACCEPT_WRITE_CONCERN);
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

    // The shardIdentity doc inserted into the admin.system.version collection on the shard.
    expectShardIdentityUpsertReturnSuccess(shardTarget, expectedShardName);

    // Wait for the addShard to complete before checking the config database
    future.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was properly added to config.shards.
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

// Tests both that trying to add a shard with the same host as an existing shard but with different
// options fails, and that adding a shard with the same host as an existing shard with the *same*
// options succeeds.
TEST_F(AddShardTest, AddExistingShardStandalone) {
    HostAndPort shardTarget("StandaloneHost:12345");
    std::unique_ptr<RemoteCommandTargeterMock> standaloneTargeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    standaloneTargeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    standaloneTargeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget),
                                           std::move(standaloneTargeter));

    std::unique_ptr<RemoteCommandTargeterMock> replsetTargeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    replsetTargeter->setConnectionStringReturnValue(
        ConnectionString::forReplicaSet("mySet", {shardTarget}));
    replsetTargeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(ConnectionString::forReplicaSet("mySet", {shardTarget}),
                                           std::move(replsetTargeter));

    std::string existingShardName = "myShard";
    ShardType existingShard;
    existingShard.setName(existingShardName);
    existingShard.setHost(shardTarget.toString());
    existingShard.setMaxSizeMB(100);
    existingShard.setState(ShardType::ShardState::kShardAware);

    // Make sure the shard already exists.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    ShardType::ConfigNS,
                                                    existingShard.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertShardExists(existingShard);

    // Adding the same standalone host with a different shard name should fail.
    std::string differentName = "anotherShardName";
    auto future1 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      catalogManager()->addShard(operationContext(),
                                                 &differentName,
                                                 ConnectionString(shardTarget),
                                                 existingShard.getMaxSizeMB()));
    });
    future1.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host with a different maxSize should fail.
    auto future2 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      catalogManager()->addShard(operationContext(),
                                                 nullptr,
                                                 ConnectionString(shardTarget),
                                                 existingShard.getMaxSizeMB() + 100));
    });
    future2.timed_get(kLongFutureTimeout);

    // Adding the same standalone host but as part of a replica set should fail.
    // Ensures that even if the user changed the standalone shard to a single-node replica set, you
    // can't change the sharded cluster's notion of the shard from standalone to replica set just
    // by calling addShard.
    auto future3 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(
            ErrorCodes::IllegalOperation,
            catalogManager()->addShard(operationContext(),
                                       nullptr,
                                       ConnectionString::forReplicaSet("mySet", {shardTarget}),
                                       existingShard.getMaxSizeMB()));
    });
    future3.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host with the same options should succeed.
    auto future4 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(catalogManager()->addShard(operationContext(),
                                                              &existingShardName,
                                                              ConnectionString(shardTarget),
                                                              existingShard.getMaxSizeMB()));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future4.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same standalone host with the same options (without explicitly specifying the
    // shard name) should succeed.
    auto future5 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(catalogManager()->addShard(operationContext(),
                                                              nullptr,
                                                              ConnectionString(shardTarget),
                                                              existingShard.getMaxSizeMB()));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future5.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);
}

// Tests both that trying to add a shard with the same replica set as an existing shard but with
// different options fails, and that adding a shard with the same replica set as an existing shard
// with the *same* options succeeds.
TEST_F(AddShardTest, AddExistingShardReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> replsetTargeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString = assertGet(ConnectionString::parse("mySet/host1:12345"));
    replsetTargeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    replsetTargeter->setFindHostReturnValue(shardTarget);
    targeterFactory()->addTargeterToReturn(connString, std::move(replsetTargeter));

    std::string existingShardName = "myShard";
    ShardType existingShard;
    existingShard.setName(existingShardName);
    existingShard.setHost(connString.toString());
    existingShard.setMaxSizeMB(100);
    existingShard.setState(ShardType::ShardState::kShardAware);

    // Make sure the shard already exists.
    ASSERT_OK(catalogClient()->insertConfigDocument(operationContext(),
                                                    ShardType::ConfigNS,
                                                    existingShard.toBSON(),
                                                    ShardingCatalogClient::kMajorityWriteConcern));
    assertShardExists(existingShard);

    // Adding the same connection string with a different shard name should fail.
    std::string differentName = "anotherShardName";
    auto future1 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(
            ErrorCodes::IllegalOperation,
            catalogManager()->addShard(
                operationContext(), &differentName, connString, existingShard.getMaxSizeMB()));
    });
    future1.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same connection string with a different maxSize should fail.
    auto future2 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(
            ErrorCodes::IllegalOperation,
            catalogManager()->addShard(
                operationContext(), nullptr, connString, existingShard.getMaxSizeMB() + 100));
    });
    future2.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a connecting string with a host of an existing shard but using a different connection
    // string type should fail.
    // Ensures that even if the user changed the replica set shard to a standalone, you can't change
    // the sharded cluster's notion of the shard from replica set to standalone just by calling
    // addShard.
    auto future3 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      catalogManager()->addShard(operationContext(),
                                                 nullptr,
                                                 ConnectionString(shardTarget),
                                                 existingShard.getMaxSizeMB()));
    });
    future3.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding a connecting string with the same hosts but a different replica set name should fail.
    // Ensures that even if you manually change the shard's replica set name somehow, you can't
    // change the replica set name the sharded cluster knows for it just by calling addShard again.
    std::string differentSetName = "differentSet";
    auto future4 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        ASSERT_EQUALS(ErrorCodes::IllegalOperation,
                      catalogManager()->addShard(operationContext(),
                                                 nullptr,
                                                 ConnectionString::forReplicaSet(
                                                     differentSetName, connString.getServers()),
                                                 existingShard.getMaxSizeMB()));
    });
    future4.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same host with the same options should succeed.
    auto future5 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(catalogManager()->addShard(
            operationContext(), &existingShardName, connString, existingShard.getMaxSizeMB()));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future5.timed_get(kLongFutureTimeout);

    // Adding the same host with the same options (without explicitly specifying the shard name)
    // should succeed.
    auto future6 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(catalogManager()->addShard(
            operationContext(), nullptr, connString, existingShard.getMaxSizeMB()));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future6.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);

    // Adding the same replica set but different host membership (but otherwise the same options)
    // should succeed
    auto otherHost = connString.getServers().back();
    ConnectionString otherHostConnString = assertGet(ConnectionString::parse("mySet/host2:12345"));
    {
        // Add a targeter for the different seed string this addShard request will use.
        std::unique_ptr<RemoteCommandTargeterMock> otherHostTargeter(
            stdx::make_unique<RemoteCommandTargeterMock>());
        otherHostTargeter->setConnectionStringReturnValue(otherHostConnString);
        otherHostTargeter->setFindHostReturnValue(otherHost);
        targeterFactory()->addTargeterToReturn(otherHostConnString, std::move(otherHostTargeter));
    }
    auto future7 = launchAsync([&] {
        Client::initThreadIfNotAlready();
        auto shardName = assertGet(catalogManager()->addShard(
            operationContext(), nullptr, otherHostConnString, existingShard.getMaxSizeMB()));
        ASSERT_EQUALS(existingShardName, shardName);
    });
    future7.timed_get(kLongFutureTimeout);

    // Ensure that the shard document was unchanged.
    assertShardExists(existingShard);
}

}  // namespace
}  // namespace mongo
