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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_factory_mock.h"
#include "mongo/s/client/shard_registry.h"
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

class AddShardTest : public CatalogManagerReplSetTestFixture {
protected:
    /**
     * Performs the test setup steps from the parent class and then configures the config shard and
     * the client name.
     */
    void setUp() override {
        CatalogManagerReplSetTestFixture::setUp();

        getMessagingPort()->setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);
    }

    void expectIsMongosCheck(const HostAndPort& target) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("isdbgrid" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            BSONObjBuilder responseBuilder;
            Command::appendCommandStatus(
                responseBuilder, Status(ErrorCodes::CommandNotFound, "isdbgrid command not found"));

            return responseBuilder.obj();
        });
    }

    void expectReplSetIsMasterCheck(const HostAndPort& target,
                                    const std::vector<std::string>& hosts) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("isMaster" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            BSONObjBuilder isMasterBuilder;
            isMasterBuilder.append("ok", 1);
            isMasterBuilder.append("ismaster", true);
            isMasterBuilder.append("setName", "mySet");

            if (hosts.size()) {
                BSONArrayBuilder hostsBuilder(isMasterBuilder.subarrayStart("hosts"));
                for (const auto& host : hosts) {
                    hostsBuilder.append(host);
                }
            }

            return isMasterBuilder.obj();
        });
    }

    void expectReplSetGetStatusCheck(const HostAndPort& target) {
        onCommandForAddShard([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, target);
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("replSetGetStatus" << 1));
            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ok" << 1);
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
     * Waits for the new shard validation sequence to run and responds with success and as if it is
     * a standalone host.
     */
    void expectNewShardCheckStandalone(const HostAndPort& target) {
        expectIsMongosCheck(target);

        onCommandForAddShard([](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("isMaster" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ok" << 1 << "ismaster" << true);
        });

        onCommandForAddShard([](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("replSetGetStatus" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            BSONObjBuilder responseBuilder;
            Command::appendCommandStatus(
                responseBuilder,
                Status(ErrorCodes::NoReplicationEnabled, "not running with --replSet"));

            return responseBuilder.obj();
        });
    }

    /**
     * Wait for a single update request and ensure that the items being updated exactly match the
     * expected items. Responds with a success status.
     */
    void expectDatabaseUpdate(const DatabaseType& dbtExpected) {
        onCommand([this, &dbtExpected](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, configHost);
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
        expectChangeLogCreate(configHost, BSON("ok" << 1));

        // Expect the change log operation
        expectChangeLogInsert(configHost,
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
                ASSERT_EQ(request.target, configHost);
                if (i == 0) {
                    ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
                } else if (i == 1) {
                    // The retry runs with PrimaryOnly read preference
                    ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);
                }

                const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
                ASSERT_EQ(nss.toString(), DatabaseType::ConfigNS);

                auto query =
                    assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

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

    const HostAndPort configHost{HostAndPort("ConfigHost:23456")};
};

TEST_F(AddShardTest, Standalone) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName] {
        auto shardName = assertGet(
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    // New shard validation
    expectNewShardCheckStandalone(shardTarget);

    // Get databases list from new shard
    expectListDatabases(shardTarget,
                        std::vector<BSONObj>{BSON("name"
                                                  << "local"
                                                  << "sizeOnDisk" << 1000),
                                             BSON("name"
                                                  << "TestDB1"
                                                  << "sizeOnDisk" << 2000),
                                             BSON("name"
                                                  << "TestDB2"
                                                  << "sizeOnDisk" << 5000)});

    // Make sure the shard add code checks for the presence of each of the two databases we returned
    // in the previous call, in the config server metadata
    expectGetDatabase("TestDB1", boost::none);
    expectGetDatabase("TestDB2", boost::none);

    // The new shard is being inserted
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost("StandaloneHost:12345");
    expectedShard.setMaxSizeMB(100);

    expectInserts(NamespaceString(ShardType::ConfigNS), {expectedShard.toBSON()});

    // Shards are being reloaded
    expectGetShards({expectedShard});

    // Add the existing database from the newly added shard
    {
        DatabaseType dbTestDB1;
        dbTestDB1.setName("TestDB1");
        dbTestDB1.setPrimary("StandaloneShard");
        dbTestDB1.setSharded(false);

        expectDatabaseUpdate(dbTestDB1);
    }

    {
        DatabaseType dbTestDB2;
        dbTestDB2.setName("TestDB2");
        dbTestDB2.setPrimary("StandaloneShard");
        dbTestDB2.setSharded(false);

        expectDatabaseUpdate(dbTestDB2);
    }

    // Expect the change log operation
    expectAddShardChangeLog("StandaloneShard", "StandaloneHost:12345");

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, StandaloneGenerateName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget("StandaloneHost:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "shard0006";

    auto future = launchAsync([this, expectedShardName, shardTarget] {
        auto shardName = assertGet(
            catalogManager()->addShard(operationContext(),
                                       nullptr,
                                       assertGet(ConnectionString::parse(shardTarget.toString())),
                                       100));
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    // New shard validation
    expectNewShardCheckStandalone(shardTarget);

    // Get databases list from new shard
    expectListDatabases(shardTarget,
                        std::vector<BSONObj>{BSON("name"
                                                  << "local"
                                                  << "sizeOnDisk" << 1000),
                                             BSON("name"
                                                  << "TestDB1"
                                                  << "sizeOnDisk" << 2000),
                                             BSON("name"
                                                  << "TestDB2"
                                                  << "sizeOnDisk" << 5000)});

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
        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), ShardType::ConfigNS);

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{existingShard.toBSON()};
    });

    // The new shard is being inserted
    ShardType expectedShard;
    expectedShard.setName(expectedShardName);
    expectedShard.setHost(shardTarget.toString());
    expectedShard.setMaxSizeMB(100);

    expectInserts(NamespaceString(ShardType::ConfigNS), {expectedShard.toBSON()});

    // Shards are being reloaded
    expectGetShards({expectedShard});

    // Add the existing database from the newly added shard
    {
        DatabaseType dbTestDB1;
        dbTestDB1.setName("TestDB1");
        dbTestDB1.setPrimary("shard0006");
        dbTestDB1.setSharded(false);

        expectDatabaseUpdate(dbTestDB1);
    }

    {
        DatabaseType dbTestDB2;
        dbTestDB2.setName("TestDB2");
        dbTestDB2.setPrimary("shard0006");
        dbTestDB2.setSharded(false);

        expectDatabaseUpdate(dbTestDB2);
    }

    // Expect the change log operation
    expectAddShardChangeLog("shard0006", "StandaloneHost:12345");

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

TEST_F(AddShardTest, UnreachableHost) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(HostAndPort("StandaloneHost:12345")));
    targeter->setFindHostReturnValue(HostAndPort("StandaloneHost:12345"));

    shardFactory()->addTargeterToReturn(ConnectionString(HostAndPort("StandaloneHost:12345")),
                                        std::move(targeter));
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

    onCommandForAddShard([](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
        return StatusWith<BSONObj>{ErrorCodes::HostUnreachable, "host unreachable"};
    });


    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, AddMongosAsShard) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(HostAndPort("StandaloneHost:12345")));
    targeter->setFindHostReturnValue(HostAndPort("StandaloneHost:12345"));

    shardFactory()->addTargeterToReturn(ConnectionString(HostAndPort("StandaloneHost:12345")),
                                        std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName] {
        auto status =
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_EQUALS("can't add a mongos process as a shard", status.getStatus().reason());
    });

    onCommandForAddShard([](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
        ASSERT_EQ(request.dbname, "admin");
        ASSERT_EQ(request.cmdObj, BSON("isdbgrid" << 1));

        // the isdbgrid command only exists on mongos, so returning ok:1 implies this is a mongos
        return BSON("ok" << 1);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, AddReplicaSetShardAsStandalone) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    HostAndPort shardTarget = HostAndPort("host1:12345");
    targeter->setConnectionStringReturnValue(ConnectionString(shardTarget));
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(ConnectionString(shardTarget), std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, shardTarget] {
        auto status =
            catalogManager()->addShard(operationContext(),
                                       &expectedShardName,
                                       assertGet(ConnectionString::parse(shardTarget.toString())),
                                       100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host is part of set mySet;");
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {});

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, AndStandaloneHostShardAsReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setConnectionStringReturnValue(connString);
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "host did not return a set name");
    });

    expectIsMongosCheck(shardTarget);

    onCommandForAddShard([](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort("host1:12345"));
        ASSERT_EQ(request.dbname, "admin");
        ASSERT_EQ(request.cmdObj, BSON("isMaster" << 1));

        ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

        return BSON("ok" << 1 << "ismaster" << true);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, ReplicaSetMistmatchedSetName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(), "does not match the actual set name");
    });

    expectIsMongosCheck(shardTarget);

    onCommandForAddShard([](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort("host1:12345"));
        ASSERT_EQ(request.dbname, "admin");
        ASSERT_EQ(request.cmdObj, BSON("isMaster" << 1));

        ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

        return BSON("ok" << 1 << "ismaster" << true << "setName"
                         << "myOtherSet");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, ShardIsCSRSConfigServer) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "since it is part of a config server replica set");
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {});

    onCommandForAddShard([](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort("host1:12345"));
        ASSERT_EQ(request.dbname, "admin");
        ASSERT_EQ(request.cmdObj, BSON("replSetGetStatus" << 1));
        ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

        return BSON("ok" << 1 << "configsvr" << true);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, ShardIsSCCConfigServer) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_EQUALS(status.getStatus().reason(),
                      "the specified mongod is a legacy-style config server and cannot be used as "
                      "a shard server");
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {});

    onCommandForAddShard([](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, HostAndPort("host1:12345"));
        ASSERT_EQ(request.dbname, "admin");
        ASSERT_EQ(request.cmdObj, BSON("replSetGetStatus" << 1));
        ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

        return BSON("ok" << 0 << "info"
                         << "configsvr");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, ReplicaSetMissingHostsProvidedInSeedList) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "StandaloneShard";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "host2:12345 does not belong to replica set");
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {"host1:12345"});

    expectReplSetGetStatusCheck(shardTarget);

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, ReplicaSetNameIsConfig) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "config";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::BadValue, status);
        ASSERT_EQUALS(status.getStatus().reason(),
                      "use of shard replica set with name 'config' is not allowed");
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {"host1:12345", "host2:12345"});

    expectReplSetGetStatusCheck(shardTarget);

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

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "mySet";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
        ASSERT_STRING_CONTAINS(
            status.getStatus().reason(),
            "because a local database 'existing' exists in another existingShard");
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {"host1:12345", "host2:12345"});

    expectReplSetGetStatusCheck(shardTarget);

    expectListDatabases(shardTarget,
                        {BSON("name"
                              << "existing")});

    DatabaseType existing;
    existing.setName("existing");
    existing.setPrimary("existingShard");
    expectGetDatabase("existing", existing);

    future.timed_get(kFutureTimeout);
}

TEST_F(AddShardTest, ExistingShardName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "mySet";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_EQUALS(ErrorCodes::DuplicateKey, status);
        ASSERT_STRING_CONTAINS(status.getStatus().reason(),
                               "E11000 duplicate key error collection: config.shards");
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {"host1:12345", "host2:12345"});

    expectReplSetGetStatusCheck(shardTarget);

    expectListDatabases(shardTarget,
                        {BSON("name"
                              << "shardDB")});

    expectGetDatabase("shardDB", boost::none);

    ShardType newShard;
    newShard.setName(expectedShardName);
    newShard.setMaxSizeMB(100);
    newShard.setHost(connString.toString());

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

TEST_F(AddShardTest, ReplicaSet) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    ConnectionString connString =
        assertGet(ConnectionString::parse("mySet/host1:12345,host2:12345"));
    targeter->setConnectionStringReturnValue(connString);
    HostAndPort shardTarget = connString.getServers().front();
    targeter->setFindHostReturnValue(shardTarget);

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "mySet";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_OK(status);
        auto shardName = status.getValue();
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {"host1:12345", "host2:12345"});

    expectReplSetGetStatusCheck(shardTarget);

    expectListDatabases(shardTarget,
                        {BSON("name"
                              << "shardDB")});

    expectGetDatabase("shardDB", boost::none);

    ShardType newShard;
    newShard.setName(expectedShardName);
    newShard.setMaxSizeMB(100);
    newShard.setHost(connString.toString());

    expectInserts(NamespaceString(ShardType::ConfigNS), {newShard.toBSON()});

    expectGetShards({newShard});

    // Add the existing database from the newly added shard
    DatabaseType shardDB;
    shardDB.setName("shardDB");
    shardDB.setPrimary(expectedShardName);
    shardDB.setSharded(false);

    expectDatabaseUpdate(shardDB);

    expectAddShardChangeLog(expectedShardName, connString.toString());

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

    shardFactory()->addTargeterToReturn(connString, std::move(targeter));
    std::string expectedShardName = "mySet";

    auto future = launchAsync([this, expectedShardName, connString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, connString, 100);
        ASSERT_OK(status);
        auto shardName = status.getValue();
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {"host1:12345", "host2:12345"});

    expectReplSetGetStatusCheck(shardTarget);

    expectListDatabases(shardTarget,
                        {BSON("name"
                              << "shardDB")});

    expectGetDatabase("shardDB", boost::none);

    ShardType newShard;
    newShard.setName(expectedShardName);
    newShard.setMaxSizeMB(100);
    newShard.setHost(connString.toString());

    expectInserts(NamespaceString(ShardType::ConfigNS), {newShard.toBSON()});

    expectGetShards({newShard});

    // Add the existing database from the newly added shard
    DatabaseType shardDB;
    shardDB.setName("shardDB");
    shardDB.setPrimary(expectedShardName);
    shardDB.setSharded(false);

    // Ensure that even if upserting the database discovered on the shard fails, the addShard
    // operation succeeds.
    onCommand([this, &shardDB](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, configHost);
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

    shardFactory()->addTargeterToReturn(seedString, std::move(targeter));
    std::string expectedShardName = "mySet";

    auto future = launchAsync([this, expectedShardName, seedString] {
        auto status =
            catalogManager()->addShard(operationContext(), &expectedShardName, seedString, 100);
        ASSERT_OK(status);
        auto shardName = status.getValue();
        ASSERT_EQUALS(expectedShardName, shardName);
    });

    expectIsMongosCheck(shardTarget);

    expectReplSetIsMasterCheck(shardTarget, {"host1:12345", "host2:12345"});

    expectReplSetGetStatusCheck(shardTarget);

    expectListDatabases(shardTarget, {});

    ShardType newShard;
    newShard.setName(expectedShardName);
    newShard.setMaxSizeMB(100);
    newShard.setHost(fullConnString.toString());

    expectInserts(NamespaceString(ShardType::ConfigNS), {newShard.toBSON()});

    expectGetShards({newShard});

    expectAddShardChangeLog(expectedShardName, seedString.toString());

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
