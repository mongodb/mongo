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
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_identity_loader.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/s/write_ops/batched_update_document.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/util/log.h"

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

class AddShardTest : public ShardingCatalogTestFixture {
protected:
    /**
     * Performs the test setup steps from the parent class and then configures the config shard and
     * the client name.
     */
    void setUp() override {
        ShardingCatalogTestFixture::setUp();
        setRemote(HostAndPort("FakeRemoteClient:34567"));

        configTargeter()->setConnectionStringReturnValue(_configConnStr);

        _configHost = _configConnStr.getServers().front();
        configTargeter()->setFindHostReturnValue(_configHost);

        _clusterId = OID::gen();

        // Ensure the cluster ID has been loaded and cached so that future requests for the cluster
        // ID will not require any network traffic.
        // TODO: use kLocalReadConcern once this test is switched to using the
        // ConfigServerTestFixture.
        auto future = launchAsync([&] {
            ASSERT_OK(ClusterIdentityLoader::get(operationContext())
                          ->loadClusterId(operationContext(),
                                          repl::ReadConcernLevel::kMajorityReadConcern));
        });
        expectGetConfigVersion();
        future.timed_get(kFutureTimeout);
        ASSERT_EQUALS(_clusterId, ClusterIdentityLoader::get(operationContext())->getClusterId());
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

    /**
     * Sets up the addShard validation checks to return expected results.
     *
     * First sets up no shards to be found in the ShardRegistry, and then the isMaster command to
     * return "isMasterResponse".
     */
    void expectValidationCheck(const HostAndPort& target, StatusWith<BSONObj> isMasterResponse) {
        std::vector<ShardType> noShards{};
        expectGetShards(noShards);
        expectIsMaster(target, isMasterResponse);
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
     * Intercepts a query on config.version and returns a basic config.version document containing
     * _clusterId
     */
    void expectGetConfigVersion() {
        VersionType version;
        version.setCurrentVersion(CURRENT_CONFIG_VERSION);
        version.setMinCompatibleVersion(MIN_COMPATIBLE_CONFIG_VERSION);
        version.setClusterId(_clusterId);

        onFindCommand([this, &version](const RemoteCommandRequest& request) {
            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss.toString(), VersionType::ConfigNS);

            auto queryResult = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
            ASSERT_OK(queryResult.getStatus());

            const auto& query = queryResult.getValue();
            ASSERT_EQ(query->ns(), VersionType::ConfigNS);

            ASSERT_EQ(query->getFilter(), BSONObj());
            ASSERT_EQ(query->getSort(), BSONObj());
            ASSERT_FALSE(query->getLimit().is_initialized());

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            return std::vector<BSONObj>{version.toBSON()};
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
     * Wait for a single update request and ensure that the items being updated exactly match the
     * expected items. Responds with a success status.
     */
    void expectDatabaseUpdate(const DatabaseType& dbtExpected) {
        onCommand([this, &dbtExpected](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, _configHost);
            ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

            BatchedUpdateRequest actualBatchedUpdate;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));

            ASSERT_EQUALS(DatabaseType::ConfigNS, actualBatchedUpdate.getNS().toString());

            auto updatesList = actualBatchedUpdate.getUpdates();
            ASSERT_EQUALS(1U, updatesList.size());

            auto updated = updatesList.front();
            ASSERT(updated->getUpsert());

            const DatabaseType dbt = assertGet(DatabaseType::fromBSON(updated->getUpdateExpr()));
            ASSERT_EQ(dbt.toBSON(), dbtExpected.toBSON());

            BatchedCommandResponse response;
            response.setOk(true);

            return response.toBSON();
        });
    }

    /**
     * Waits for a change log insertion to happen with the specified shard information in it.
     */
    void expectAddShardChangeLog(const std::string& shardName, const std::string& shardHost) {
        // Expect the change log collection to be created
        expectChangeLogCreate(_configHost, BSON("ok" << 1));

        // Expect the change log operation
        expectChangeLogInsert(_configHost,
                              network()->now(),
                              "addShard",
                              "",
                              BSON("name" << shardName << "host" << shardHost));
    }

    void expectGetDatabase(const std::string& dbname, boost::optional<DatabaseType> response) {
        for (int i = 0; i < (response ? 1 : 2); i++) {
            // Do it twice when there is no response set because getDatabase retries if it can't
            // find a database
            onFindCommand([&](const RemoteCommandRequest& request) {
                ASSERT_EQ(request.target, _configHost);
                if (i == 0) {
                    ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
                } else if (i == 1) {
                    // The retry runs with PrimaryOnly read preference
                    ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);
                }

                const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
                ASSERT_EQ(nss.toString(), DatabaseType::ConfigNS);

                auto query =
                    assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

                ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
                ASSERT_EQ(query->getFilter(), BSON(DatabaseType::name(dbname)));

                checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

                if (response) {
                    return vector<BSONObj>{response->toBSON()};
                } else {
                    return vector<BSONObj>{};
                }
            });
        }
    }

    const ConnectionString _configConnStr{ConnectionString::forReplicaSet(
        "configRS",
        {HostAndPort("host1:23456"), HostAndPort("host2:23456"), HostAndPort("host3:23456")})};
    HostAndPort _configHost;
    OID _clusterId;
};

TEST_F(AddShardTest, Standalone) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName] {
        auto shardName = assertGet(
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true);
    expectValidationCheck(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget,
                        std::vector<BSONObj>{BSON("name"
                                                  << "local"
                                                  << "sizeOnDisk"
                                                  << 1000),
                                             BSON("name"
                                                  << "TestDB1"
                                                  << "sizeOnDisk"
                                                  << 2000),
                                             BSON("name"
                                                  << "TestDB2"
                                                  << "sizeOnDisk"
                                                  << 5000)});

    // Make sure the shard add code checks for the presence of each of the two databases we returned
    // in the previous call, in the config server metadata
    expectGetDatabase("TestDB1", boost::none);
    expectGetDatabase("TestDB2", boost::none);

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setMaxSizeMB(100);
    expectedShard.setState(ShardType::ShardState::kShardAware);

    expectInserts(NamespaceString(ShardType::ConfigNS), {expectedShard.toBSON()});

    // Add the existing database from the newly added shard
    {
        DatabaseType dbTestDB1;
        dbTestDB1.setName("TestDB1");
        dbTestDB1.setPrimary(ShardId("StandaloneShard"));
        dbTestDB1.setSharded(false);

        expectDatabaseUpdate(dbTestDB1);
    }

    {
        DatabaseType dbTestDB2;
        dbTestDB2.setName("TestDB2");
        dbTestDB2.setPrimary(ShardId("StandaloneShard"));
        dbTestDB2.setSharded(false);

        expectDatabaseUpdate(dbTestDB2);
    }

    // Expect the change log operation
    expectAddShardChangeLog("StandaloneShard", "StandaloneHost:12345");

    // Shards are being reloaded
    expectGetShards({expectedShard});

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, StandaloneGenerateName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    targeterFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "shard0006";

    auto future = launchAsync([this, expectedShardName, shardTarget] {
        auto shardName = assertGet(
            catalogManager()->addShard(operationContext(),
                                       nullptr,
                                       assertGet(ConnectionString::parse(shardTarget.toString())),
                                       100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true);
    expectValidationCheck(shardTarget, commandResponse);

    // Get databases list from new shard
    expectListDatabases(shardTarget,
                        std::vector<BSONObj>{BSON("name"
                                                  << "local"
                                                  << "sizeOnDisk"
                                                  << 1000),
                                             BSON("name"
                                                  << "TestDB1"
                                                  << "sizeOnDisk"
                                                  << 2000),
                                             BSON("name"
                                                  << "TestDB2"
                                                  << "sizeOnDisk"
                                                  << 5000)});

    // Make sure the shard add code checks for the presence of each of the two databases we returned
    // in the previous call, in the config server metadata
    expectGetDatabase("TestDB1", boost::none);
    expectGetDatabase("TestDB2", boost::none);

    ShardType existingShard;
    existingShard.setName("shard0005");
    existingShard.setHost("shard0005:45678");
    existingShard.setMaxSizeMB(100);

    // New name is being generated for the new shard
    onFindCommand([this, &existingShard](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), ShardType::ConfigNS);
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), ShardType::ConfigNS);

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{existingShard.toBSON()};
    });

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(shardTarget.toString());
    expectedShard.setMaxSizeMB(100);
    expectedShard.setState(ShardType::ShardState::kShardAware);

    expectInserts(NamespaceString(ShardType::ConfigNS), {expectedShard.toBSON()});

    // Add the existing database from the newly added shard
    {
        DatabaseType dbTestDB1;
        dbTestDB1.setName("TestDB1");
        dbTestDB1.setPrimary(ShardId("shard0006"));
        dbTestDB1.setSharded(false);

        expectDatabaseUpdate(dbTestDB1);
    }

    {
        DatabaseType dbTestDB2;
        dbTestDB2.setName("TestDB2");
        dbTestDB2.setPrimary(ShardId("shard0006"));
        dbTestDB2.setSharded(false);

        expectDatabaseUpdate(dbTestDB2);
    }

    // Expect the change log operation
    expectAddShardChangeLog("shard0006", "StandaloneHost:12345");

    // Shards are being reloaded
    expectGetShards({expectedShard});

    future.timed_get(kFutureTimeout);
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

    auto future = launchAsync([this, expectedShardName] {
        auto status =
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100);
        ASSERT_EQUALS(ErrorCodes::HostUnreachable, status);
        ASSERT_EQUALS("host unreachable", status.getStatus().reason());
    });

    Status hostUnreachableStatus = Status(ErrorCodes::HostUnreachable, "host unreachable");
    expectValidationCheck(shardTarget, hostUnreachableStatus);

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

    auto future = launchAsync([this, expectedShardName] {
        auto status =
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100);
        ASSERT_EQUALS(ErrorCodes::RPCProtocolNegotiationFailed, status);
    });

    Status rpcProtocolNegFailedStatus =
        Status(ErrorCodes::RPCProtocolNegotiationFailed, "Unable to communicate");
    expectValidationCheck(shardTarget, rpcProtocolNegFailedStatus);

    future.timed_get(kFutureTimeout);
}

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
        auto status =
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse(shardTarget.toString())),
                                       100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "use replica set url format");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "myOtherSet");
    expectValidationCheck(shardTarget, commandResponse);

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
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host did not return a set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true);
    expectValidationCheck(shardTarget, commandResponse);

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
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "does not match the actual set name");
    });

    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "myOtherSet");
    expectValidationCheck(shardTarget, commandResponse);

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
    expectValidationCheck(shardTarget, commandResponse);

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
    expectValidationCheck(shardTarget, commandResponse);

    future.timed_get(kFutureTimeout);
}

// Cannot add a shard with the shard name "config".
TEST_F(AddShardTest, ShardNameIsConfig) {
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
    expectValidationCheck(shardTarget, commandResponse);

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

    auto future = launchAsync([this, expectedShardName, connString] {
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
    expectValidationCheck(shardTarget, commandResponse);

    expectListDatabases(shardTarget,
                        {BSON("name"
                              << "existing")});

    DatabaseType existing;
    existing.setName("existing");
    existing.setPrimary(ShardId("existingShard"));
    expectGetDatabase("existing", existing);

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
    expectValidationCheck(shardTarget, commandResponse);

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

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_OK(status);
        auto shardName = status.getValue();
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectValidationCheck(shardTarget, commandResponse);

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

    expectInserts(NamespaceString(ShardType::ConfigNS), {newShard.toBSON()});

    // Add the existing database from the newly added shard
    DatabaseType shardDB;
    shardDB.setName("shardDB");
    shardDB.setPrimary(ShardId(expectedShardName));
    shardDB.setSharded(false);

    expectDatabaseUpdate(shardDB);

    expectAddShardChangeLog(expectedShardName, connString.toString());

    expectGetShards({newShard});

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, AddShardSucceedsEvenIfAddingDBsFromNewShardFails) {
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
        auto shardName = status.getValue();
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectValidationCheck(shardTarget, commandResponse);

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

    expectInserts(NamespaceString(ShardType::ConfigNS), {newShard.toBSON()});

    // Add the existing database from the newly added shard
    DatabaseType shardDB;
    shardDB.setName("shardDB");
    shardDB.setPrimary(ShardId(expectedShardName));
    shardDB.setSharded(false);

    // Ensure that even if upserting the database discovered on the shard fails, the addShard
    // operation succeeds.
    onCommand([this, &shardDB](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, _configHost);
        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));

        ASSERT_EQUALS(DatabaseType::ConfigNS, actualBatchedUpdate.getNS().toString());

        auto updatesList = actualBatchedUpdate.getUpdates();
        ASSERT_EQUALS(1U, updatesList.size());

        auto updated = updatesList.front();
        ASSERT(updated->getUpsert());

        const DatabaseType dbt = assertGet(DatabaseType::fromBSON(updated->getUpdateExpr()));
        ASSERT_EQ(dbt.toBSON(), shardDB.toBSON());

        return Status(ErrorCodes::ExceededTimeLimit, "some random error");
    });

    expectAddShardChangeLog(expectedShardName, connString.toString());

    expectGetShards({newShard});

    future.timed_get(kFutureTimeout);
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

    auto future = launchAsync([this, expectedShardName, seedString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, seedString, 100);
        ASSERT_OK(status);
        auto shardName = status.getValue();
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    BSONArrayBuilder hosts;
    hosts.append("host1:12345");
    hosts.append("host2:12345");
    BSONObj commandResponse = BSON("ok" << 1 << "ismaster" << true << "setName"
                                        << "mySet"
                                        << "hosts"
                                        << hosts.arr());
    expectValidationCheck(shardTarget, commandResponse);

    expectListDatabases(shardTarget, {});

    // The shardIdentity doc inserted into the config.version collection on the shard.
    expectShardIdentityUpsert(shardTarget, expectedShardName);

    // The shard doc inserted into the config.shards collection on the config server.
    ShardType newShard;
    newShard.setName(expectedShardName);
    newShard.setMaxSizeMB(100);
    newShard.setHost(fullConnString.toString());
    newShard.setState(ShardType::ShardState::kShardAware);

    expectInserts(NamespaceString(ShardType::ConfigNS), {newShard.toBSON()});

    expectAddShardChangeLog(expectedShardName, seedString.toString());

    expectGetShards({newShard});

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, CreateShardIdentityUpsertForAddShard) {
    std::string shardName = "shardName";

    BSONObj expectedBSON = BSON("update"
                                << "system.version"
                                << "updates"
                                << BSON_ARRAY(BSON(
                                       "q" << BSON("_id"
                                                   << "shardIdentity"
                                                   << "shardName"
                                                   << shardName
                                                   << "clusterId"
                                                   << _clusterId)
                                           << "u"
                                           << BSON("$set" << BSON("configsvrConnectionString"
                                                                  << _configConnStr.toString()))
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

}  // namespace
}  // namespace mongo
