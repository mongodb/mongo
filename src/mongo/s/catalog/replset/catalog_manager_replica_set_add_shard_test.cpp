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
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_changelog.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_update_document.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::vector;
using unittest::assertGet;

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

    /**
     * Waits for the new shard validation sequence to run and responds with success and as if it is
     * a standalone host.
     */
    void expectNewShardCheckStandalone() {
        onCommandForAddShard([](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("isdbgrid" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            BSONObjBuilder responseBuilder;
            Command::appendCommandStatus(
                responseBuilder, Status(ErrorCodes::CommandNotFound, "isdbgrid command not found"));

            return responseBuilder.obj();
        });

        onCommandForAddShard([](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("isMaster" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            return BSON("ismaster" << true);
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
        onCommand([&dbtExpected](const RemoteCommandRequest& request) {
            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss.ns(), DatabaseType::ConfigNS);

            ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

            BatchedUpdateRequest actualBatchedUpdate;
            std::string errmsg;
            ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));

            ASSERT_EQUALS(nss.toString(), actualBatchedUpdate.getNS().toString());

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
                              "FakeRemoteClient:34567",
                              shardRegistry()->getNetwork()->now(),
                              "addShard",
                              "",
                              BSON("name" << shardName << "host" << shardHost));
    }

    const HostAndPort configHost{HostAndPort("ConfigHost:23456")};
};

TEST_F(AddShardTest, AddShardStandalone) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(HostAndPort("StandaloneHost:12345")));
    targeter->setFindHostReturnValue(HostAndPort("StandaloneHost:12345"));

    targeterFactory()->addTargeterToReturn(ConnectionString(HostAndPort("StandaloneHost:12345")),
                                           std::move(targeter));

    auto future = launchAsync([this] {
        const std::string shardName("StandaloneShard");
        auto status = assertGet(
            catalogManager()->addShard(operationContext(),
                                       &shardName,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100));
    });

    // New shard validation
    expectNewShardCheckStandalone();

    // Get databases list from new shard
    onCommandForAddShard(
        [](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("listDatabases" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            BSONArrayBuilder arr;

            arr.append(BSON("name"
                            << "local"
                            << "sizeOnDisk" << 1000));
            arr.append(BSON("name"
                            << "TestDB1"
                            << "sizeOnDisk" << 2000));
            arr.append(BSON("name"
                            << "TestDB2"
                            << "sizeOnDisk" << 5000));

            return BSON("databases" << arr.obj());
        });

    // Make sure the shard add code checks for the presence of each of the two databases we returned
    // in the previous call, in the config server metadata
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, configHost);
        ASSERT_EQUALS(BSON(rpc::kSecondaryOkFieldName << 1 << rpc::kReplSetMetadataFieldName << 1),
                      request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), DatabaseType::ConfigNS);

        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSON(DatabaseType::name("TestDB1")));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    onFindCommand([this](const RemoteCommandRequest& request) {
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), DatabaseType::ConfigNS);
        ASSERT_EQUALS(BSON(rpc::kSecondaryOkFieldName << 1 << rpc::kReplSetMetadataFieldName << 1),
                      request.metadata);

        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSON(DatabaseType::name("TestDB2")));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    // The new shard is being inserted
    ShardType expectedShard;
    expectedShard.setName("StandaloneShard");
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

TEST_F(AddShardTest, AddShardStandaloneGenerateName) {
    std::unique_ptr<RemoteCommandTargeterMock> targeter(
        stdx::make_unique<RemoteCommandTargeterMock>());
    targeter->setConnectionStringReturnValue(ConnectionString(HostAndPort("StandaloneHost:12345")));
    targeter->setFindHostReturnValue(HostAndPort("StandaloneHost:12345"));

    targeterFactory()->addTargeterToReturn(ConnectionString(HostAndPort("StandaloneHost:12345")),
                                           std::move(targeter));

    auto future = launchAsync([this] {
        auto status = assertGet(
            catalogManager()->addShard(operationContext(),
                                       nullptr,
                                       assertGet(ConnectionString::parse("StandaloneHost:12345")),
                                       100));
    });

    // New shard validation
    expectNewShardCheckStandalone();

    // Get databases list from new shard
    onCommandForAddShard(
        [](const RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, HostAndPort("StandaloneHost:12345"));
            ASSERT_EQ(request.dbname, "admin");
            ASSERT_EQ(request.cmdObj, BSON("listDatabases" << 1));

            ASSERT_EQUALS(rpc::makeEmptyMetadata(), request.metadata);

            BSONArrayBuilder arr;

            arr.append(BSON("name"
                            << "local"
                            << "sizeOnDisk" << 1000));
            arr.append(BSON("name"
                            << "TestDB1"
                            << "sizeOnDisk" << 2000));
            arr.append(BSON("name"
                            << "TestDB2"
                            << "sizeOnDisk" << 5000));

            return BSON("databases" << arr.obj());
        });

    // Make sure the shard add code checks for the presence of each of the two databases we returned
    // in the previous call, in the config server metadata
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(request.target, configHost);
        ASSERT_EQUALS(BSON(rpc::kSecondaryOkFieldName << 1 << rpc::kReplSetMetadataFieldName << 1),
                      request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), DatabaseType::ConfigNS);

        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSON(DatabaseType::name("TestDB1")));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(BSON(rpc::kSecondaryOkFieldName << 1 << rpc::kReplSetMetadataFieldName << 1),
                      request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), DatabaseType::ConfigNS);
        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSON(DatabaseType::name("TestDB2")));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    ShardType existingShard;
    existingShard.setName("shard0005");
    existingShard.setHost("shard0005:45678");
    existingShard.setMaxSizeMB(100);

    // New name is being generated for the new shard
    onFindCommand([this, &existingShard](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(BSON(rpc::kSecondaryOkFieldName << 1 << rpc::kReplSetMetadataFieldName << 1),
                      request.metadata);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.toString(), ShardType::ConfigNS);
        auto query = assertGet(LiteParsedQuery::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), ShardType::ConfigNS);

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{existingShard.toBSON()};
    });

    // The new shard is being inserted
    ShardType expectedShard;
    expectedShard.setName("shard0006");
    expectedShard.setHost("StandaloneHost:12345");
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

TEST_F(AddShardTest, AddShardInvalidHostName) {}

TEST_F(AddShardTest, AddShardInvalidShardName) {}

TEST_F(AddShardTest, AddShardShardNameIsConfig) {}

TEST_F(AddShardTest, AddShardUnreachableHost) {}

TEST_F(AddShardTest, AddShardAlreadyShard) {}

TEST_F(AddShardTest, AddShardMongoS) {}

TEST_F(AddShardTest, AddShardReplicaSet) {}

TEST_F(AddShardTest, AddShardReplicaSetNoSetSpecified) {}

TEST_F(AddShardTest, AddShardReplicaSetExtraHostsDiscovered) {}

TEST_F(AddShardTest, AddShardReplicaSetNotMaster) {}

TEST_F(AddShardTest, AddShardReplicaSetMistmatchedSetName) {}

TEST_F(AddShardTest, AddShardReplicaSetMismatchedHosts) {}

}  // namespace
}  // namespace mongo
