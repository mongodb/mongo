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

#include <pcrecpp.h>

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using executor::TaskExecutor;
using rpc::ReplSetMetadata;
using repl::OpTime;
using std::string;
using std::vector;
using unittest::assertGet;

using ShardingCatalogClientTest = ShardingCatalogTestFixture;

const int kMaxCommandRetry = 3;

const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(rpc::ServerSelectionMetadata(true, boost::none).toBSON());
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}()};

TEST_F(ShardingCatalogClientTest, GetCollectionExisting) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    CollectionType expectedColl;
    expectedColl.setNs(NamespaceString("TestDB.TestNS"));
    expectedColl.setKeyPattern(BSON("KeyName" << 1));
    expectedColl.setUpdatedAt(Date_t());
    expectedColl.setEpoch(OID::gen());

    const OpTime newOpTime(Timestamp(7, 6), 5);

    auto future = launchAsync([this, &expectedColl] {
        return assertGet(
            catalogClient()->getCollection(operationContext(), expectedColl.getNs().ns()));
    });

    onFindWithMetadataCommand(
        [this, &expectedColl, newOpTime](const RemoteCommandRequest& request) {

            ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss.ns(), CollectionType::ConfigNS);

            auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

            // Ensure the query is correct
            ASSERT_EQ(query->ns(), CollectionType::ConfigNS);
            ASSERT_EQ(query->getFilter(), BSON(CollectionType::fullNs(expectedColl.getNs().ns())));
            ASSERT_EQ(query->getSort(), BSONObj());
            ASSERT_EQ(query->getLimit().get(), 1);

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
            BSONObjBuilder builder;
            metadata.writeToMetadata(&builder);

            return std::make_tuple(vector<BSONObj>{expectedColl.toBSON()}, builder.obj());
        });

    // Now wait for the getCollection call to return
    const auto collOpTimePair = future.timed_get(kFutureTimeout);
    ASSERT_EQ(newOpTime, collOpTimePair.opTime);
    ASSERT_EQ(expectedColl.toBSON(), collOpTimePair.value.toBSON());
}

TEST_F(ShardingCatalogClientTest, GetCollectionNotExisting) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->getCollection(operationContext(), "NonExistent");
        ASSERT_EQUALS(status.getStatus(), ErrorCodes::NamespaceNotFound);
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    // Now wait for the getCollection call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetDatabaseInvalidName) {
    auto status = catalogClient()->getDatabase(operationContext(), "b.c").getStatus();
    ASSERT_EQ(ErrorCodes::InvalidNamespace, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(ShardingCatalogClientTest, GetDatabaseExisting) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    DatabaseType expectedDb;
    expectedDb.setName("bigdata");
    expectedDb.setPrimary(ShardId("shard0000"));
    expectedDb.setSharded(true);

    const OpTime newOpTime(Timestamp(7, 6), 5);

    auto future = launchAsync([this, &expectedDb] {
        return assertGet(catalogClient()->getDatabase(operationContext(), expectedDb.getName()));
    });

    onFindWithMetadataCommand([this, &expectedDb, newOpTime](const RemoteCommandRequest& request) {
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), DatabaseType::ConfigNS);

        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSON(DatabaseType::name(expectedDb.getName())));
        ASSERT_EQ(query->getSort(), BSONObj());
        ASSERT_EQ(query->getLimit().get(), 1);

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
        BSONObjBuilder builder;
        metadata.writeToMetadata(&builder);

        return std::make_tuple(vector<BSONObj>{expectedDb.toBSON()}, builder.obj());
    });

    const auto dbOpTimePair = future.timed_get(kFutureTimeout);
    ASSERT_EQ(newOpTime, dbOpTimePair.opTime);
    ASSERT_EQ(expectedDb.toBSON(), dbOpTimePair.value.toBSON());
}

TEST_F(ShardingCatalogClientTest, GetDatabaseStaleSecondaryRetrySuccess) {
    HostAndPort firstHost{"TestHost1"};
    HostAndPort secondHost{"TestHost2"};
    configTargeter()->setFindHostReturnValue(firstHost);

    DatabaseType expectedDb;
    expectedDb.setName("bigdata");
    expectedDb.setPrimary(ShardId("shard0000"));
    expectedDb.setSharded(true);

    auto future = launchAsync([this, &expectedDb] {
        return assertGet(catalogClient()->getDatabase(operationContext(), expectedDb.getName()));
    });

    // Return empty result set as if the database wasn't found
    onFindCommand([this, &firstHost, &secondHost](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(firstHost, request.target);
        configTargeter()->setFindHostReturnValue(secondHost);
        return vector<BSONObj>{};
    });

    // Make sure we retarget and retry.
    onFindCommand([this, &expectedDb, &secondHost](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(secondHost, request.target);
        return vector<BSONObj>{expectedDb.toBSON()};
    });

    const auto dbOpTimePair = future.timed_get(kFutureTimeout);
    ASSERT_EQ(expectedDb.toBSON(), dbOpTimePair.value.toBSON());
}

TEST_F(ShardingCatalogClientTest, GetDatabaseStaleSecondaryRetryNoPrimary) {
    HostAndPort testHost{"TestHost1"};
    configTargeter()->setFindHostReturnValue(testHost);

    auto future = launchAsync([this] {
        auto dbResult = catalogClient()->getDatabase(operationContext(), "NonExistent");
        ASSERT_EQ(dbResult.getStatus(), ErrorCodes::NotMaster);
    });

    // Return empty result set as if the database wasn't found
    onFindCommand([this, &testHost](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(testHost, request.target);
        // Make it so when it attempts to retarget and retry it will get a NotMaster error.
        configTargeter()->setFindHostReturnValue(Status(ErrorCodes::NotMaster, "no config master"));
        return vector<BSONObj>{};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetDatabaseNotExisting) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        auto dbResult = catalogClient()->getDatabase(operationContext(), "NonExistent");
        ASSERT_EQ(dbResult.getStatus(), ErrorCodes::NamespaceNotFound);
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });
    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateCollection) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    CollectionType collection;
    collection.setNs(NamespaceString("db.coll"));
    collection.setUpdatedAt(network()->now());
    collection.setUnique(true);
    collection.setEpoch(OID::gen());
    collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

    auto future = launchAsync([this, collection] {
        auto status = catalogClient()->updateCollection(
            operationContext(), collection.getNs().toString(), collection);
        ASSERT_OK(status);
    });

    expectUpdateCollection(HostAndPort("TestHost1"), collection);

    // Now wait for the updateCollection call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateCollectionNotMaster) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    CollectionType collection;
    collection.setNs(NamespaceString("db.coll"));
    collection.setUpdatedAt(network()->now());
    collection.setUnique(true);
    collection.setEpoch(OID::gen());
    collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

    auto future = launchAsync([this, collection] {
        auto status = catalogClient()->updateCollection(
            operationContext(), collection.getNs().toString(), collection);
        ASSERT_EQUALS(ErrorCodes::NotMaster, status);
    });

    for (int i = 0; i < 3; ++i) {
        onCommand([](const RemoteCommandRequest& request) {
            BatchedCommandResponse response;
            response.setOk(false);
            response.setErrCode(ErrorCodes::NotMaster);
            response.setErrMessage("not master");

            return response.toBSON();
        });
    }

    // Now wait for the updateCollection call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateCollectionNotMasterFromTargeter) {
    configTargeter()->setFindHostReturnValue(Status(ErrorCodes::NotMaster, "not master"));

    CollectionType collection;
    collection.setNs(NamespaceString("db.coll"));
    collection.setUpdatedAt(network()->now());
    collection.setUnique(true);
    collection.setEpoch(OID::gen());
    collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

    auto future = launchAsync([this, collection] {
        auto status = catalogClient()->updateCollection(
            operationContext(), collection.getNs().toString(), collection);
        ASSERT_EQUALS(ErrorCodes::NotMaster, status);
    });

    // Now wait for the updateCollection call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateCollectionNotMasterRetrySuccess) {
    HostAndPort host1("TestHost1");
    HostAndPort host2("TestHost2");
    configTargeter()->setFindHostReturnValue(host1);

    CollectionType collection;
    collection.setNs(NamespaceString("db.coll"));
    collection.setUpdatedAt(network()->now());
    collection.setUnique(true);
    collection.setEpoch(OID::gen());
    collection.setKeyPattern(KeyPattern(BSON("_id" << 1)));

    auto future = launchAsync([this, collection] {
        auto status = catalogClient()->updateCollection(
            operationContext(), collection.getNs().toString(), collection);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(host1, request.target);

        BatchedCommandResponse response;
        response.setOk(false);
        response.setErrCode(ErrorCodes::NotMaster);
        response.setErrMessage("not master");

        // Ensure that when the catalog manager tries to retarget after getting the
        // NotMaster response, it will get back a new target.
        configTargeter()->setFindHostReturnValue(host2);
        return response.toBSON();
    });

    expectUpdateCollection(HostAndPort(host2), collection);

    // Now wait for the updateCollection call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetAllShardsValid) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ShardType s1;
    s1.setName("shard0000");
    s1.setHost("ShardHost");
    s1.setDraining(false);
    s1.setMaxSizeMB(50);
    s1.setTags({"tag1", "tag2", "tag3"});

    ShardType s2;
    s2.setName("shard0001");
    s2.setHost("ShardHost");

    ShardType s3;
    s3.setName("shard0002");
    s3.setHost("ShardHost");
    s3.setMaxSizeMB(65);

    const vector<ShardType> expectedShardsList = {s1, s2, s3};

    auto future = launchAsync([this] {
        auto shards = assertGet(catalogClient()->getAllShards(operationContext()));
        return shards.value;
    });

    onFindCommand([this, &s1, &s2, &s3](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), ShardType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), ShardType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSONObj());
        ASSERT_EQ(query->getSort(), BSONObj());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{s1.toBSON(), s2.toBSON(), s3.toBSON()};
    });

    const vector<ShardType> actualShardsList = future.timed_get(kFutureTimeout);
    ASSERT_EQ(actualShardsList.size(), expectedShardsList.size());

    for (size_t i = 0; i < actualShardsList.size(); ++i) {
        ASSERT_EQ(actualShardsList[i].toBSON(), expectedShardsList[i].toBSON());
    }
}

TEST_F(ShardingCatalogClientTest, GetAllShardsWithInvalidShard) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->getAllShards(operationContext());

        ASSERT_EQ(ErrorCodes::FailedToParse, status.getStatus());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        // Valid ShardType
        ShardType s1;
        s1.setName("shard0001");
        s1.setHost("ShardHost");

        return vector<BSONObj>{
            s1.toBSON(),
            BSONObj()  // empty document is invalid
        };
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetChunksForNSWithSortAndLimit) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    OID oid = OID::gen();

    ChunkType chunkA;
    chunkA.setNS("TestDB.TestColl");
    chunkA.setMin(BSON("a" << 1));
    chunkA.setMax(BSON("a" << 100));
    chunkA.setVersion({1, 2, oid});
    chunkA.setShard(ShardId("shard0000"));

    ChunkType chunkB;
    chunkB.setNS("TestDB.TestColl");
    chunkB.setMin(BSON("a" << 100));
    chunkB.setMax(BSON("a" << 200));
    chunkB.setVersion({3, 4, oid});
    chunkB.setShard(ShardId("shard0001"));

    ChunkVersion queryChunkVersion({1, 2, oid});

    const BSONObj chunksQuery(
        BSON(ChunkType::ns("TestDB.TestColl")
             << ChunkType::DEPRECATED_lastmod()
             << BSON("$gte" << static_cast<long long>(queryChunkVersion.toLong()))));

    const OpTime newOpTime(Timestamp(7, 6), 5);

    auto future = launchAsync([this, &chunksQuery, newOpTime] {
        vector<ChunkType> chunks;
        OpTime opTime;

        ASSERT_OK(catalogClient()->getChunks(operationContext(),
                                             chunksQuery,
                                             BSON(ChunkType::DEPRECATED_lastmod() << -1),
                                             1,
                                             &chunks,
                                             &opTime));
        ASSERT_EQ(2U, chunks.size());
        ASSERT_EQ(newOpTime, opTime);

        return chunks;
    });

    onFindWithMetadataCommand([this, &chunksQuery, chunkA, chunkB, newOpTime](
        const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), ChunkType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), ChunkType::ConfigNS);
        ASSERT_EQ(query->getFilter(), chunksQuery);
        ASSERT_EQ(query->getSort(), BSON(ChunkType::DEPRECATED_lastmod() << -1));
        ASSERT_EQ(query->getLimit().get(), 1);

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
        BSONObjBuilder builder;
        metadata.writeToMetadata(&builder);

        return std::make_tuple(vector<BSONObj>{chunkA.toBSON(), chunkB.toBSON()}, builder.obj());
    });

    const auto& chunks = future.timed_get(kFutureTimeout);
    ASSERT_EQ(chunkA.toBSON(), chunks[0].toBSON());
    ASSERT_EQ(chunkB.toBSON(), chunks[1].toBSON());
}

TEST_F(ShardingCatalogClientTest, GetChunksForNSNoSortNoLimit) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChunkVersion queryChunkVersion({1, 2, OID::gen()});

    const BSONObj chunksQuery(
        BSON(ChunkType::ns("TestDB.TestColl")
             << ChunkType::DEPRECATED_lastmod()
             << BSON("$gte" << static_cast<long long>(queryChunkVersion.toLong()))));

    auto future = launchAsync([this, &chunksQuery] {
        vector<ChunkType> chunks;

        ASSERT_OK(catalogClient()->getChunks(
            operationContext(), chunksQuery, BSONObj(), boost::none, &chunks, nullptr));
        ASSERT_EQ(0U, chunks.size());

        return chunks;
    });

    onFindCommand([this, &chunksQuery](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), ChunkType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), ChunkType::ConfigNS);
        ASSERT_EQ(query->getFilter(), chunksQuery);
        ASSERT_EQ(query->getSort(), BSONObj());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetChunksForNSInvalidChunk) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChunkVersion queryChunkVersion({1, 2, OID::gen()});

    const BSONObj chunksQuery(
        BSON(ChunkType::ns("TestDB.TestColl")
             << ChunkType::DEPRECATED_lastmod()
             << BSON("$gte" << static_cast<long long>(queryChunkVersion.toLong()))));

    auto future = launchAsync([this, &chunksQuery] {
        vector<ChunkType> chunks;
        Status status = catalogClient()->getChunks(
            operationContext(), chunksQuery, BSONObj(), boost::none, &chunks, nullptr);

        ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
        ASSERT_EQ(0U, chunks.size());
    });

    onFindCommand([&chunksQuery](const RemoteCommandRequest& request) {
        ChunkType chunkA;
        chunkA.setNS("TestDB.TestColl");
        chunkA.setMin(BSON("a" << 1));
        chunkA.setMax(BSON("a" << 100));
        chunkA.setVersion({1, 2, OID::gen()});
        chunkA.setShard(ShardId("shard0000"));

        ChunkType chunkB;
        chunkB.setNS("TestDB.TestColl");
        chunkB.setMin(BSON("a" << 100));
        chunkB.setMax(BSON("a" << 200));
        chunkB.setVersion({3, 4, OID::gen()});
        // Missing shard id

        return vector<BSONObj>{chunkA.toBSON(), chunkB.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RunUserManagementReadCommand) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        bool ok = catalogClient()->runUserManagementReadCommand(
            operationContext(), "test", BSON("usersInfo" << 1), &responseBuilder);
        ASSERT_TRUE(ok);

        BSONObj response = responseBuilder.obj();
        ASSERT_TRUE(response["ok"].trueValue());
        auto users = response["users"].Array();
        ASSERT_EQUALS(0U, users.size());
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQUALS("test", request.dbname);
        ASSERT_EQUALS(BSON("usersInfo" << 1 << "maxTimeMS" << 30000), request.cmdObj);

        return BSON("ok" << 1 << "users" << BSONArrayBuilder().arr());
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RunUserManagementReadCommandUnsatisfiedReadPref) {
    configTargeter()->setFindHostReturnValue(
        Status(ErrorCodes::FailedToSatisfyReadPreference, "no nodes up"));

    BSONObjBuilder responseBuilder;
    bool ok = catalogClient()->runUserManagementReadCommand(
        operationContext(), "test", BSON("usersInfo" << 1), &responseBuilder);
    ASSERT_FALSE(ok);

    Status commandStatus = getStatusFromCommandResult(responseBuilder.obj());
    ASSERT_EQUALS(ErrorCodes::FailedToSatisfyReadPreference, commandStatus);
}

TEST_F(ShardingCatalogClientTest, RunUserManagementWriteCommandSuccess) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        bool ok = catalogClient()->runUserManagementWriteCommand(operationContext(),
                                                                 "dropUser",
                                                                 "test",
                                                                 BSON("dropUser"
                                                                      << "test"),
                                                                 &responseBuilder);
        ASSERT_FALSE(ok);

        Status commandStatus = getStatusFromCommandResult(responseBuilder.obj());
        ASSERT_EQUALS(ErrorCodes::UserNotFound, commandStatus);
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("test", request.dbname);
        // Since no write concern was sent we will add w:majority
        ASSERT_EQUALS(BSON("dropUser"
                           << "test"
                           << "writeConcern"
                           << BSON("w"
                                   << "majority"
                                   << "wtimeout"
                                   << 0)
                           << "maxTimeMS"
                           << 30000),
                      request.cmdObj);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BSONObjBuilder responseBuilder;
        Command::appendCommandStatus(responseBuilder,
                                     Status(ErrorCodes::UserNotFound, "User test@test not found"));
        return responseBuilder.obj();
    });

    // Now wait for the runUserManagementWriteCommand call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RunUserManagementWriteCommandWithInvalidWriteConcernSingleNode) {
    // Tests that if you send a non-majority write concern it will not be accepted
    BSONObjBuilder responseBuilder;
    bool ok =
        catalogClient()->runUserManagementWriteCommand(operationContext(),
                                                       "dropUser",
                                                       "test",
                                                       BSON("dropUser"
                                                            << "test"
                                                            << "writeConcern"
                                                            << BSON("w" << 1 << "wtimeout" << 30)),
                                                       &responseBuilder);
    ASSERT_FALSE(ok);

    Status commandStatus = getStatusFromCommandResult(responseBuilder.obj());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, commandStatus);
    ASSERT_STRING_CONTAINS(commandStatus.reason(), "Invalid replication write concern");
}

TEST_F(ShardingCatalogClientTest, RunUserManagementWriteCommandInvalidWriteConcernTwoNodes) {
    // Tests that if you send a non-majority write concern it will not be accepted
    BSONObjBuilder responseBuilder;
    bool ok = catalogClient()->runUserManagementWriteCommand(operationContext(),
                                                             "dropUser",
                                                             "test",
                                                             BSON("dropUser"
                                                                  << "test"
                                                                  << "writeConcern"
                                                                  << BSON("w" << 2)),
                                                             &responseBuilder);
    ASSERT_FALSE(ok);

    Status commandStatus = getStatusFromCommandResult(responseBuilder.obj());
    ASSERT_EQUALS(ErrorCodes::InvalidOptions, commandStatus);
    ASSERT_STRING_CONTAINS(commandStatus.reason(), "Invalid replication write concern");
}

TEST_F(ShardingCatalogClientTest, RunUserManagementWriteCommandNotMaster) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        bool ok = catalogClient()->runUserManagementWriteCommand(operationContext(),
                                                                 "dropUser",
                                                                 "test",
                                                                 BSON("dropUser"
                                                                      << "test"),
                                                                 &responseBuilder);
        ASSERT_FALSE(ok);

        Status commandStatus = getStatusFromCommandResult(responseBuilder.obj());
        ASSERT_EQUALS(ErrorCodes::NotMaster, commandStatus);
    });

    for (int i = 0; i < 3; ++i) {
        onCommand([](const RemoteCommandRequest& request) {
            BSONObjBuilder responseBuilder;
            Command::appendCommandStatus(responseBuilder,
                                         Status(ErrorCodes::NotMaster, "not master"));
            return responseBuilder.obj();
        });
    }

    // Now wait for the runUserManagementWriteCommand call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RunUserManagementWriteCommandNotMasterRetrySuccess) {
    HostAndPort host1("TestHost1");
    HostAndPort host2("TestHost2");

    configTargeter()->setFindHostReturnValue(host1);

    auto future = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        bool ok = catalogClient()->runUserManagementWriteCommand(operationContext(),
                                                                 "dropUser",
                                                                 "test",
                                                                 BSON("dropUser"
                                                                      << "test"),
                                                                 &responseBuilder);
        ASSERT_TRUE(ok);

        Status commandStatus = getStatusFromCommandResult(responseBuilder.obj());
        ASSERT_OK(commandStatus);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(host1, request.target);

        BSONObjBuilder responseBuilder;
        Command::appendCommandStatus(responseBuilder, Status(ErrorCodes::NotMaster, "not master"));

        // Ensure that when the catalog manager tries to retarget after getting the
        // NotMaster response, it will get back a new target.
        configTargeter()->setFindHostReturnValue(host2);
        return responseBuilder.obj();
    });

    onCommand([host2](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(host2, request.target);
        ASSERT_EQUALS("test", request.dbname);
        // Since no write concern was sent we will add w:majority
        ASSERT_EQUALS(BSON("dropUser"
                           << "test"
                           << "writeConcern"
                           << BSON("w"
                                   << "majority"
                                   << "wtimeout"
                                   << 0)
                           << "maxTimeMS"
                           << 30000),
                      request.cmdObj);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        return BSON("ok" << 1);
    });

    // Now wait for the runUserManagementWriteCommand call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetCollectionsValidResultsNoDb) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    CollectionType coll1;
    coll1.setNs(NamespaceString{"test.system.indexes"});
    coll1.setUpdatedAt(network()->now());
    coll1.setUnique(true);
    coll1.setEpoch(OID::gen());
    coll1.setKeyPattern(KeyPattern{BSON("_id" << 1)});
    ASSERT_OK(coll1.validate());

    CollectionType coll2;
    coll2.setNs(NamespaceString{"test.coll1"});
    coll2.setUpdatedAt(network()->now());
    coll2.setUnique(false);
    coll2.setEpoch(OID::gen());
    coll2.setKeyPattern(KeyPattern{BSON("_id" << 1)});
    ASSERT_OK(coll2.validate());

    CollectionType coll3;
    coll3.setNs(NamespaceString{"anotherdb.coll1"});
    coll3.setUpdatedAt(network()->now());
    coll3.setUnique(false);
    coll3.setEpoch(OID::gen());
    coll3.setKeyPattern(KeyPattern{BSON("_id" << 1)});
    ASSERT_OK(coll3.validate());

    const OpTime newOpTime(Timestamp(7, 6), 5);

    auto future = launchAsync([this, newOpTime] {
        vector<CollectionType> collections;

        OpTime opTime;
        const auto status =
            catalogClient()->getCollections(operationContext(), nullptr, &collections, &opTime);

        ASSERT_OK(status);
        ASSERT_EQ(newOpTime, opTime);

        return collections;
    });

    onFindWithMetadataCommand(
        [this, coll1, coll2, coll3, newOpTime](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss.ns(), CollectionType::ConfigNS);

            auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

            ASSERT_EQ(query->ns(), CollectionType::ConfigNS);
            ASSERT_EQ(query->getFilter(), BSONObj());
            ASSERT_EQ(query->getSort(), BSONObj());

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
            BSONObjBuilder builder;
            metadata.writeToMetadata(&builder);

            return std::make_tuple(vector<BSONObj>{coll1.toBSON(), coll2.toBSON(), coll3.toBSON()},
                                   builder.obj());
        });

    const auto& actualColls = future.timed_get(kFutureTimeout);
    ASSERT_EQ(3U, actualColls.size());
    ASSERT_EQ(coll1.toBSON(), actualColls[0].toBSON());
    ASSERT_EQ(coll2.toBSON(), actualColls[1].toBSON());
    ASSERT_EQ(coll3.toBSON(), actualColls[2].toBSON());
}

TEST_F(ShardingCatalogClientTest, GetCollectionsValidResultsWithDb) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    CollectionType coll1;
    coll1.setNs(NamespaceString{"test.system.indexes"});
    coll1.setUpdatedAt(network()->now());
    coll1.setUnique(true);
    coll1.setEpoch(OID::gen());
    coll1.setKeyPattern(KeyPattern{BSON("_id" << 1)});

    CollectionType coll2;
    coll2.setNs(NamespaceString{"test.coll1"});
    coll2.setUpdatedAt(network()->now());
    coll2.setUnique(false);
    coll2.setEpoch(OID::gen());
    coll2.setKeyPattern(KeyPattern{BSON("_id" << 1)});

    auto future = launchAsync([this] {
        string dbName = "test";
        vector<CollectionType> collections;

        const auto status =
            catalogClient()->getCollections(operationContext(), &dbName, &collections, nullptr);

        ASSERT_OK(status);
        return collections;
    });

    onFindCommand([this, coll1, coll2](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), CollectionType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), CollectionType::ConfigNS);
        {
            BSONObjBuilder b;
            b.appendRegex(CollectionType::fullNs(), "^test\\.");
            ASSERT_EQ(query->getFilter(), b.obj());
        }

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{coll1.toBSON(), coll2.toBSON()};
    });

    const auto& actualColls = future.timed_get(kFutureTimeout);
    ASSERT_EQ(2U, actualColls.size());
    ASSERT_EQ(coll1.toBSON(), actualColls[0].toBSON());
    ASSERT_EQ(coll2.toBSON(), actualColls[1].toBSON());
}

TEST_F(ShardingCatalogClientTest, GetCollectionsInvalidCollectionType) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        string dbName = "test";
        vector<CollectionType> collections;

        const auto status =
            catalogClient()->getCollections(operationContext(), &dbName, &collections, nullptr);

        ASSERT_EQ(ErrorCodes::FailedToParse, status);
        ASSERT_EQ(0U, collections.size());
    });

    CollectionType validColl;
    validColl.setNs(NamespaceString{"test.system.indexes"});
    validColl.setUpdatedAt(network()->now());
    validColl.setUnique(true);
    validColl.setEpoch(OID::gen());
    validColl.setKeyPattern(KeyPattern{BSON("_id" << 1)});
    ASSERT_OK(validColl.validate());

    onFindCommand([this, validColl](const RemoteCommandRequest& request) {
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), CollectionType::ConfigNS);

        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), CollectionType::ConfigNS);
        {
            BSONObjBuilder b;
            b.appendRegex(CollectionType::fullNs(), "^test\\.");
            ASSERT_EQ(query->getFilter(), b.obj());
        }

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{
            validColl.toBSON(),
            BSONObj()  // empty document is invalid
        };
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetDatabasesForShardValid) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    DatabaseType dbt1;
    dbt1.setName("db1");
    dbt1.setPrimary(ShardId("shard0000"));

    DatabaseType dbt2;
    dbt2.setName("db2");
    dbt2.setPrimary(ShardId("shard0000"));

    auto future = launchAsync([this] {
        vector<string> dbs;
        const auto status =
            catalogClient()->getDatabasesForShard(operationContext(), ShardId("shard0000"), &dbs);

        ASSERT_OK(status);
        return dbs;
    });

    onFindCommand([this, dbt1, dbt2](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), DatabaseType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), DatabaseType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSON(DatabaseType::primary(dbt1.getPrimary().toString())));
        ASSERT_EQ(query->getSort(), BSONObj());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{dbt1.toBSON(), dbt2.toBSON()};
    });

    const auto& actualDbNames = future.timed_get(kFutureTimeout);
    ASSERT_EQ(2U, actualDbNames.size());
    ASSERT_EQ(dbt1.getName(), actualDbNames[0]);
    ASSERT_EQ(dbt2.getName(), actualDbNames[1]);
}

TEST_F(ShardingCatalogClientTest, GetDatabasesForShardInvalidDoc) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        vector<string> dbs;
        const auto status =
            catalogClient()->getDatabasesForShard(operationContext(), ShardId("shard0000"), &dbs);

        ASSERT_EQ(ErrorCodes::TypeMismatch, status);
        ASSERT_EQ(0U, dbs.size());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        DatabaseType dbt1;
        dbt1.setName("db1");
        dbt1.setPrimary(ShardId("shard0000"));

        return vector<BSONObj>{
            dbt1.toBSON(),
            BSON(DatabaseType::name() << 0)  // DatabaseType::name() should be a string
        };
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetTagsForCollection) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    TagsType tagA;
    tagA.setNS("TestDB.TestColl");
    tagA.setTag("TagA");
    tagA.setMinKey(BSON("a" << 100));
    tagA.setMaxKey(BSON("a" << 200));

    TagsType tagB;
    tagB.setNS("TestDB.TestColl");
    tagB.setTag("TagB");
    tagB.setMinKey(BSON("a" << 200));
    tagB.setMaxKey(BSON("a" << 300));

    auto future = launchAsync([this] {
        vector<TagsType> tags;

        ASSERT_OK(
            catalogClient()->getTagsForCollection(operationContext(), "TestDB.TestColl", &tags));
        ASSERT_EQ(2U, tags.size());

        return tags;
    });

    onFindCommand([this, tagA, tagB](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), TagsType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), TagsType::ConfigNS);
        ASSERT_EQ(query->getFilter(), BSON(TagsType::ns("TestDB.TestColl")));
        ASSERT_EQ(query->getSort(), BSON(TagsType::min() << 1));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{tagA.toBSON(), tagB.toBSON()};
    });

    const auto& tags = future.timed_get(kFutureTimeout);
    ASSERT_EQ(tagA.toBSON(), tags[0].toBSON());
    ASSERT_EQ(tagB.toBSON(), tags[1].toBSON());
}

TEST_F(ShardingCatalogClientTest, GetTagsForCollectionNoTags) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        vector<TagsType> tags;

        ASSERT_OK(
            catalogClient()->getTagsForCollection(operationContext(), "TestDB.TestColl", &tags));
        ASSERT_EQ(0U, tags.size());

        return tags;
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetTagsForCollectionInvalidTag) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        vector<TagsType> tags;
        Status status =
            catalogClient()->getTagsForCollection(operationContext(), "TestDB.TestColl", &tags);

        ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
        ASSERT_EQ(0U, tags.size());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        TagsType tagA;
        tagA.setNS("TestDB.TestColl");
        tagA.setTag("TagA");
        tagA.setMinKey(BSON("a" << 100));
        tagA.setMaxKey(BSON("a" << 200));

        TagsType tagB;
        tagB.setNS("TestDB.TestColl");
        tagB.setTag("TagB");
        tagB.setMinKey(BSON("a" << 200));
        // Missing maxKey

        return vector<BSONObj>{tagA.toBSON(), tagB.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetTagForChunkOneTagFound) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChunkType chunk;
    chunk.setNS("test.coll");
    chunk.setMin(BSON("a" << 1));
    chunk.setMax(BSON("a" << 100));
    chunk.setVersion({1, 2, OID::gen()});
    chunk.setShard(ShardId("shard0000"));
    ASSERT_OK(chunk.validate());

    auto future = launchAsync([this, chunk] {
        return assertGet(catalogClient()->getTagForChunk(operationContext(), "test.coll", chunk));
    });

    onFindCommand([this, chunk](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), TagsType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), TagsType::ConfigNS);
        ASSERT_EQ(query->getFilter(),
                  BSON(TagsType::ns(chunk.getNS()) << TagsType::min()
                                                   << BSON("$lte" << chunk.getMin())
                                                   << TagsType::max()
                                                   << BSON("$gte" << chunk.getMax())));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        TagsType tt;
        tt.setNS("test.coll");
        tt.setTag("tag");
        tt.setMinKey(BSON("a" << 1));
        tt.setMaxKey(BSON("a" << 100));

        return vector<BSONObj>{tt.toBSON()};
    });

    const string& tagStr = future.timed_get(kFutureTimeout);
    ASSERT_EQ("tag", tagStr);
}

TEST_F(ShardingCatalogClientTest, GetTagForChunkNoTagFound) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChunkType chunk;
    chunk.setNS("test.coll");
    chunk.setMin(BSON("a" << 1));
    chunk.setMax(BSON("a" << 100));
    chunk.setVersion({1, 2, OID::gen()});
    chunk.setShard(ShardId("shard0000"));
    ASSERT_OK(chunk.validate());

    auto future = launchAsync([this, chunk] {
        return assertGet(catalogClient()->getTagForChunk(operationContext(), "test.coll", chunk));
    });

    onFindCommand([this, chunk](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), TagsType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), TagsType::ConfigNS);
        ASSERT_EQ(query->getFilter(),
                  BSON(TagsType::ns(chunk.getNS()) << TagsType::min()
                                                   << BSON("$lte" << chunk.getMin())
                                                   << TagsType::max()
                                                   << BSON("$gte" << chunk.getMax())));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    const string& tagStr = future.timed_get(kFutureTimeout);
    ASSERT_EQ("", tagStr);  // empty string returned when tag document not found
}

TEST_F(ShardingCatalogClientTest, GetTagForChunkInvalidTagDoc) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChunkType chunk;
    chunk.setNS("test.coll");
    chunk.setMin(BSON("a" << 1));
    chunk.setMax(BSON("a" << 100));
    chunk.setVersion({1, 2, OID::gen()});
    chunk.setShard(ShardId("shard0000"));
    ASSERT_OK(chunk.validate());

    auto future = launchAsync([this, chunk] {
        const auto tagResult =
            catalogClient()->getTagForChunk(operationContext(), "test.coll", chunk);

        ASSERT_EQ(ErrorCodes::FailedToParse, tagResult.getStatus());
    });

    onFindCommand([this, chunk](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss.ns(), TagsType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->ns(), TagsType::ConfigNS);
        ASSERT_EQ(query->getFilter(),
                  BSON(TagsType::ns(chunk.getNS()) << TagsType::min()
                                                   << BSON("$lte" << chunk.getMin())
                                                   << TagsType::max()
                                                   << BSON("$gte" << chunk.getMax())));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        // Return a tag document missing the min key
        return vector<BSONObj>{BSON(TagsType::ns("test.mycol") << TagsType::tag("tag")
                                                               << TagsType::max(BSON("a" << 20)))};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateDatabase) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    DatabaseType dbt;
    dbt.setName("test");
    dbt.setPrimary(ShardId("shard0000"));
    dbt.setSharded(true);

    auto future = launchAsync([this, dbt] {
        auto status = catalogClient()->updateDatabase(operationContext(), dbt.getName(), dbt);
        ASSERT_OK(status);
    });

    onCommand([dbt](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(DatabaseType::ConfigNS, actualBatchedUpdate.getNS().ns());
        auto updates = actualBatchedUpdate.getUpdates();
        ASSERT_EQUALS(1U, updates.size());
        auto update = updates.front();

        ASSERT_TRUE(update->getUpsert());
        ASSERT_FALSE(update->getMulti());
        ASSERT_EQUALS(update->getQuery(), BSON(DatabaseType::name(dbt.getName())));
        ASSERT_EQUALS(update->getUpdateExpr(), dbt.toBSON());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    // Now wait for the updateDatabase call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateDatabaseExceededTimeLimit) {
    HostAndPort host1("TestHost1");
    configTargeter()->setFindHostReturnValue(host1);

    DatabaseType dbt;
    dbt.setName("test");
    dbt.setPrimary(ShardId("shard0001"));
    dbt.setSharded(false);

    auto future = launchAsync([this, dbt] {
        auto status = catalogClient()->updateDatabase(operationContext(), dbt.getName(), dbt);
        ASSERT_EQ(ErrorCodes::ExceededTimeLimit, status);
    });

    onCommand([host1](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(host1, request.target);

        BatchedCommandResponse response;
        response.setOk(false);
        response.setErrCode(ErrorCodes::ExceededTimeLimit);
        response.setErrMessage("operation timed out");

        return response.toBSON();
    });

    // Now wait for the updateDatabase call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, ApplyChunkOpsDeprecatedSuccessful) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONArray updateOps = BSON_ARRAY(BSON("update1"
                                          << "first update")
                                     << BSON("update2"
                                             << "second update"));
    BSONArray preCondition = BSON_ARRAY(BSON("precondition1"
                                             << "first precondition")
                                        << BSON("precondition2"
                                                << "second precondition"));
    std::string nss = "config.chunks";
    ChunkVersion lastChunkVersion(0, 0, OID());

    auto future = launchAsync([this, updateOps, preCondition, nss, lastChunkVersion] {
        auto status = catalogClient()->applyChunkOpsDeprecated(
            operationContext(), updateOps, preCondition, nss, lastChunkVersion);
        ASSERT_OK(status);
    });

    onCommand(
        [updateOps, preCondition, nss](const RemoteCommandRequest& request) {
            ASSERT_EQUALS("config", request.dbname);
            ASSERT_EQUALS(BSON("w"
                               << "majority"
                               << "wtimeout"
                               << 15000),
                          request.cmdObj["writeConcern"].Obj());
            ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);
            ASSERT_EQUALS(updateOps, request.cmdObj["applyOps"].Obj());
            ASSERT_EQUALS(preCondition, request.cmdObj["preCondition"].Obj());

            return BSON("ok" << 1);
        });

    // Now wait for the applyChunkOpsDeprecated call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, ApplyChunkOpsDeprecatedSuccessfulWithCheck) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONArray updateOps = BSON_ARRAY(BSON("update1"
                                          << "first update")
                                     << BSON("update2"
                                             << "second update"));
    BSONArray preCondition = BSON_ARRAY(BSON("precondition1"
                                             << "first precondition")
                                        << BSON("precondition2"
                                                << "second precondition"));
    std::string nss = "config.chunks";
    ChunkVersion lastChunkVersion(0, 0, OID());

    auto future = launchAsync([this, updateOps, preCondition, nss, lastChunkVersion] {
        auto status = catalogClient()->applyChunkOpsDeprecated(
            operationContext(), updateOps, preCondition, nss, lastChunkVersion);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BSONObjBuilder responseBuilder;
        Command::appendCommandStatus(responseBuilder,
                                     Status(ErrorCodes::DuplicateKey, "precondition failed"));
        return responseBuilder.obj();
    });

    onFindCommand([this](const RemoteCommandRequest& request) {
        OID oid = OID::gen();
        ChunkType chunk;
        chunk.setNS("TestDB.TestColl");
        chunk.setMin(BSON("a" << 1));
        chunk.setMax(BSON("a" << 100));
        chunk.setVersion({1, 2, oid});
        chunk.setShard(ShardId("shard0000"));
        return vector<BSONObj>{chunk.toBSON()};
    });

    // Now wait for the applyChunkOpsDeprecated call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, ApplyChunkOpsDeprecatedFailedWithCheck) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONArray updateOps = BSON_ARRAY(BSON("update1"
                                          << "first update")
                                     << BSON("update2"
                                             << "second update"));
    BSONArray preCondition = BSON_ARRAY(BSON("precondition1"
                                             << "first precondition")
                                        << BSON("precondition2"
                                                << "second precondition"));
    std::string nss = "config.chunks";
    ChunkVersion lastChunkVersion(0, 0, OID());

    auto future = launchAsync([this, updateOps, preCondition, nss, lastChunkVersion] {
        auto status = catalogClient()->applyChunkOpsDeprecated(
            operationContext(), updateOps, preCondition, nss, lastChunkVersion);
        ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BSONObjBuilder responseBuilder;
        Command::appendCommandStatus(responseBuilder,
                                     Status(ErrorCodes::NoMatchingDocument, "some error"));
        return responseBuilder.obj();
    });

    onFindCommand([this](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    // Now wait for the applyChunkOpsDeprecated call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, createDatabaseSuccess) {
    const string dbname = "databaseToCreate";
    const HostAndPort configHost("TestHost1");
    configTargeter()->setFindHostReturnValue(configHost);

    ShardType s0;
    s0.setName("shard0000");
    s0.setHost("ShardHost0:27017");

    ShardType s1;
    s1.setName("shard0001");
    s1.setHost("ShardHost1:27017");

    ShardType s2;
    s2.setName("shard0002");
    s2.setHost("ShardHost2:27017");

    // Prime the shard registry with information about the existing shards
    auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(ShardType::ConfigNS, query->ns());
        ASSERT_EQ(BSONObj(), query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{s0.toBSON(), s1.toBSON(), s2.toBSON()};
    });

    future.timed_get(kFutureTimeout);

    // Set up all the target mocks return values.
    RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), s0.getName())->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s0.getHost()));
    RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), s1.getName())->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s1.getHost()));
    RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), s2.getName())->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s2.getHost()));


    // Now actually start the createDatabase work.

    distLock()->expectLock([dbname](StringData name,
                                    StringData whyMessage,
                                    Milliseconds waitFor,
                                    Milliseconds lockTryInterval) {},
                           Status::OK());


    future = launchAsync([this, dbname] {
        Status status = catalogClient()->createDatabase(operationContext(), dbname);
        ASSERT_OK(status);
    });

    // Report no databases with the same name already exist
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
        ASSERT_EQ(DatabaseType::ConfigNS, nss.ns());
        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);
        return vector<BSONObj>{};
    });

    // Return size information about first shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s0.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return BSON("ok" << 1 << "totalSize" << 10);
    });

    // Return size information about second shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s1.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return BSON("ok" << 1 << "totalSize" << 1);
    });

    // Return size information about third shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s2.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return BSON("ok" << 1 << "totalSize" << 100);
    });

    // Process insert to config.databases collection
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(DatabaseType::ConfigNS, actualBatchedInsert.getNS().ns());
        auto inserts = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(1U, inserts.size());
        auto insert = inserts.front();

        DatabaseType expectedDb;
        expectedDb.setName(dbname);
        expectedDb.setPrimary(
            ShardId(s1.getName()));  // This is the one we reported with the smallest size
        expectedDb.setSharded(false);

        ASSERT_EQUALS(expectedDb.toBSON(), insert);

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, createDatabaseDistLockHeld) {
    const string dbname = "databaseToCreate";


    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    distLock()->expectLock(
        [dbname](StringData name,
                 StringData whyMessage,
                 Milliseconds waitFor,
                 Milliseconds lockTryInterval) {
            ASSERT_EQUALS(dbname, name);
            ASSERT_EQUALS("createDatabase", whyMessage);
        },
        Status(ErrorCodes::LockBusy, "lock already held"));

    Status status = catalogClient()->createDatabase(operationContext(), dbname);
    ASSERT_EQUALS(ErrorCodes::LockBusy, status);
}

TEST_F(ShardingCatalogClientTest, createDatabaseDBExists) {
    const string dbname = "databaseToCreate";


    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    distLock()->expectLock([dbname](StringData name,
                                    StringData whyMessage,
                                    Milliseconds waitFor,
                                    Milliseconds lockTryInterval) {},
                           Status::OK());


    auto future = launchAsync([this, dbname] {
        Status status = catalogClient()->createDatabase(operationContext(), dbname);
        ASSERT_EQUALS(ErrorCodes::NamespaceExists, status);
    });

    onFindCommand([this, dbname](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        BSONObjBuilder queryBuilder;
        queryBuilder.appendRegex(
            DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbname) + "$", "i");

        ASSERT_EQ(DatabaseType::ConfigNS, query->ns());
        ASSERT_EQ(queryBuilder.obj(), query->getFilter());
        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{BSON("_id" << dbname)};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, createDatabaseDBExistsDifferentCase) {
    const string dbname = "databaseToCreate";
    const string dbnameDiffCase = "databasetocreate";


    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    distLock()->expectLock([dbname](StringData name,
                                    StringData whyMessage,
                                    Milliseconds waitFor,
                                    Milliseconds lockTryInterval) {},
                           Status::OK());


    auto future = launchAsync([this, dbname] {
        Status status = catalogClient()->createDatabase(operationContext(), dbname);
        ASSERT_EQUALS(ErrorCodes::DatabaseDifferCase, status);
    });

    onFindCommand([this, dbname, dbnameDiffCase](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        BSONObjBuilder queryBuilder;
        queryBuilder.appendRegex(
            DatabaseType::name(), (string) "^" + pcrecpp::RE::QuoteMeta(dbname) + "$", "i");

        ASSERT_EQ(DatabaseType::ConfigNS, query->ns());
        ASSERT_EQ(queryBuilder.obj(), query->getFilter());
        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{BSON("_id" << dbnameDiffCase)};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, createDatabaseNoShards) {
    const string dbname = "databaseToCreate";


    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    distLock()->expectLock([dbname](StringData name,
                                    StringData whyMessage,
                                    Milliseconds waitFor,
                                    Milliseconds lockTryInterval) {},
                           Status::OK());


    auto future = launchAsync([this, dbname] {
        Status status = catalogClient()->createDatabase(operationContext(), dbname);
        ASSERT_EQUALS(ErrorCodes::ShardNotFound, status);
    });

    // Report no databases with the same name already exist
    onFindCommand([this, dbname](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(DatabaseType::ConfigNS, nss.ns());
        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);
        return vector<BSONObj>{};
    });

    // Report no shards exist
    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(ShardType::ConfigNS, query->ns());
        ASSERT_EQ(BSONObj(), query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, createDatabaseDuplicateKeyOnInsert) {
    const string dbname = "databaseToCreate";
    const HostAndPort configHost("TestHost1");
    configTargeter()->setFindHostReturnValue(configHost);

    ShardType s0;
    s0.setName("shard0000");
    s0.setHost("ShardHost0:27017");

    ShardType s1;
    s1.setName("shard0001");
    s1.setHost("ShardHost1:27017");

    ShardType s2;
    s2.setName("shard0002");
    s2.setHost("ShardHost2:27017");

    // Prime the shard registry with information about the existing shards
    auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });

    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(ShardType::ConfigNS, query->ns());
        ASSERT_EQ(BSONObj(), query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{s0.toBSON(), s1.toBSON(), s2.toBSON()};
    });

    future.timed_get(kFutureTimeout);

    // Set up all the target mocks return values.
    RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), s0.getName())->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s0.getHost()));
    RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), s1.getName())->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s1.getHost()));
    RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), s2.getName())->getTargeter())
        ->setFindHostReturnValue(HostAndPort(s2.getHost()));


    // Now actually start the createDatabase work.

    distLock()->expectLock([dbname](StringData name,
                                    StringData whyMessage,
                                    Milliseconds waitFor,
                                    Milliseconds lockTryInterval) {},
                           Status::OK());


    future = launchAsync([this, dbname] {
        Status status = catalogClient()->createDatabase(operationContext(), dbname);
        ASSERT_EQUALS(ErrorCodes::NamespaceExists, status);
    });

    // Report no databases with the same name already exist
    onFindCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(DatabaseType::ConfigNS, nss.ns());
        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);
        return vector<BSONObj>{};
    });

    // Return size information about first shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s0.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return BSON("ok" << 1 << "totalSize" << 10);
    });

    // Return size information about second shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s1.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return BSON("ok" << 1 << "totalSize" << 1);
    });

    // Return size information about third shard
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(s2.getHost(), request.target.toString());
        ASSERT_EQUALS("admin", request.dbname);
        string cmdName = request.cmdObj.firstElement().fieldName();
        ASSERT_EQUALS("listDatabases", cmdName);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return BSON("ok" << 1 << "totalSize" << 100);
    });

    // Process insert to config.databases collection
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(configHost, request.target);
        ASSERT_EQUALS("config", request.dbname);
        ASSERT_FALSE(request.cmdObj.hasField(repl::ReadConcernArgs::kReadConcernFieldName));

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BatchedInsertRequest actualBatchedInsert;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedInsert.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(DatabaseType::ConfigNS, actualBatchedInsert.getNS().ns());
        auto inserts = actualBatchedInsert.getDocuments();
        ASSERT_EQUALS(1U, inserts.size());
        auto insert = inserts.front();

        DatabaseType expectedDb;
        expectedDb.setName(dbname);
        expectedDb.setPrimary(
            ShardId(s1.getName()));  // This is the one we reported with the smallest size
        expectedDb.setSharded(false);

        ASSERT_EQUALS(expectedDb.toBSON(), insert);

        BatchedCommandResponse response;
        response.setOk(false);
        response.setErrCode(ErrorCodes::DuplicateKey);
        response.setErrMessage("duplicate key");

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, EnableShardingNoDBExists) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    vector<ShardType> shards;
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    setupShards(vector<ShardType>{shard});

    auto shardTargeter = RemoteCommandTargeterMock::get(
        shardRegistry()->getShard(operationContext(), ShardId("shard0"))->getTargeter());
    shardTargeter->setFindHostReturnValue(HostAndPort("shard0:12"));

    distLock()->expectLock(
        [](StringData name, StringData whyMessage, Milliseconds, Milliseconds) {
            ASSERT_EQ("test", name);
            ASSERT_FALSE(whyMessage.empty());
        },
        Status::OK());

    auto future = launchAsync([this] {
        auto status = catalogClient()->enableSharding(operationContext(), "test");
        ASSERT_OK(status);
    });

    // Query to find if db already exists in config.
    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(DatabaseType::ConfigNS, nss.toString());

        auto queryResult = QueryRequest::makeFromFindCommand(nss, request.cmdObj, false);
        ASSERT_OK(queryResult.getStatus());

        const auto& query = queryResult.getValue();
        BSONObj expectedQuery(fromjson(R"({ _id: { $regex: "^test$", $options: "i" }})"));

        ASSERT_EQ(DatabaseType::ConfigNS, query->ns());
        ASSERT_EQ(expectedQuery, query->getFilter());
        ASSERT_EQ(BSONObj(), query->getSort());
        ASSERT_EQ(1, query->getLimit().get());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    // list databases for checking shard size.
    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("shard0:12"), request.target);
        ASSERT_EQ("admin", request.dbname);
        ASSERT_EQ(BSON("listDatabases" << 1), request.cmdObj);

        ASSERT_EQUALS(rpc::ServerSelectionMetadata(true, boost::none).toBSON(), request.metadata);

        return fromjson(R"({
                databases: [],
                totalSize: 1,
                ok: 1
            })");
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BSONObj expectedCmd(fromjson(R"({
            update: "databases",
            updates: [{
                q: { _id: "test" },
                u: { _id: "test", primary: "shard0", partitioned: true },
                multi: false,
                upsert: true
            }],
            writeConcern: { w: "majority", wtimeout: 15000 },
            maxTimeMS: 30000
        })"));

        ASSERT_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                nModified: 0,
                n: 1,
                upserted: [
                    { _id: "test", primary: "shard0", partitioned: true }
                ],
                ok: 1
            })");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, EnableShardingLockBusy) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    distLock()->expectLock([](StringData, StringData, Milliseconds, Milliseconds) {},
                           {ErrorCodes::LockBusy, "lock taken"});

    auto status = catalogClient()->enableSharding(operationContext(), "test");
    ASSERT_EQ(ErrorCodes::LockBusy, status.code());
}

TEST_F(ShardingCatalogClientTest, EnableShardingDBExistsWithDifferentCase) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    vector<ShardType> shards;
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    setupShards(vector<ShardType>{shard});

    distLock()->expectLock([](StringData, StringData, Milliseconds, Milliseconds) {}, Status::OK());

    auto future = launchAsync([this] {
        auto status = catalogClient()->enableSharding(operationContext(), "test");
        ASSERT_EQ(ErrorCodes::DatabaseDifferCase, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    // Query to find if db already exists in config.
    onFindCommand([](const RemoteCommandRequest& request) {
        BSONObj existingDoc(fromjson(R"({ _id: "Test", primary: "shard0", partitioned: true })"));
        return vector<BSONObj>{existingDoc};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, EnableShardingDBExists) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    vector<ShardType> shards;
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    setupShards(vector<ShardType>{shard});

    distLock()->expectLock([](StringData, StringData, Milliseconds, Milliseconds) {}, Status::OK());

    auto future = launchAsync([this] {
        auto status = catalogClient()->enableSharding(operationContext(), "test");
        ASSERT_OK(status);
    });

    // Query to find if db already exists in config.
    onFindCommand([](const RemoteCommandRequest& request) {
        BSONObj existingDoc(fromjson(R"({ _id: "test", primary: "shard2", partitioned: false })"));
        return vector<BSONObj>{existingDoc};
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

        BSONObj expectedCmd(fromjson(R"({
            update: "databases",
            updates: [{
                q: { _id: "test" },
                u: { _id: "test", primary: "shard2", partitioned: true },
                multi: false,
                upsert: true
            }],
            writeConcern: { w: "majority", wtimeout: 15000 },
            maxTimeMS: 30000
        })"));

        ASSERT_EQ(expectedCmd, request.cmdObj);

        return fromjson(R"({
                nModified: 0,
                n: 1,
                upserted: [
                    { _id: "test", primary: "shard2", partitioned: true }
                ],
                ok: 1
            })");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, EnableShardingFailsWhenTheDatabaseIsAlreadySharded) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    vector<ShardType> shards;
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    setupShards(vector<ShardType>{shard});

    distLock()->expectLock([](StringData, StringData, Milliseconds, Milliseconds) {}, Status::OK());

    auto future = launchAsync([this] {
        auto status = catalogClient()->enableSharding(operationContext(), "test");
        ASSERT_EQ(status.code(), ErrorCodes::AlreadyInitialized);
    });

    // Query to find if db already exists in config and it is sharded.
    onFindCommand([](const RemoteCommandRequest& request) {
        BSONObj existingDoc(fromjson(R"({ _id: "test", primary: "shard2", partitioned: true })"));
        return vector<BSONObj>{existingDoc};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, EnableShardingDBExistsInvalidFormat) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    vector<ShardType> shards;
    ShardType shard;
    shard.setName("shard0");
    shard.setHost("shard0:12");

    setupShards(vector<ShardType>{shard});

    distLock()->expectLock([](StringData, StringData, Milliseconds, Milliseconds) {}, Status::OK());

    auto future = launchAsync([this] {
        auto status = catalogClient()->enableSharding(operationContext(), "test");
        ASSERT_EQ(ErrorCodes::TypeMismatch, status.code());
    });

    // Query to find if db already exists in config.
    onFindCommand([](const RemoteCommandRequest& request) {
        // Bad type for primary field.
        BSONObj existingDoc(fromjson(R"({ _id: "test", primary: 12, partitioned: false })"));
        return vector<BSONObj>{existingDoc};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, EnableShardingNoDBExistsNoShards) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    distLock()->expectLock([](StringData, StringData, Milliseconds, Milliseconds) {}, Status::OK());

    auto future = launchAsync([this] {
        auto status = catalogClient()->enableSharding(operationContext(), "test");
        ASSERT_EQ(ErrorCodes::ShardNotFound, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    // Query to find if db already exists in config.
    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    // Query for config.shards reload.
    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, BasicReadAfterOpTime) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    OpTime lastOpTime;
    for (int x = 0; x < 3; x++) {
        auto future = launchAsync([this] {
            BSONObjBuilder responseBuilder;
            ASSERT_TRUE(getCatalogClient()->runReadCommandForTest(
                operationContext(), "test", BSON("dummy" << 1), &responseBuilder));
        });

        const OpTime newOpTime(Timestamp(x + 2, x + 6), x + 5);

        onCommandWithMetadata([this, &newOpTime, &lastOpTime](const RemoteCommandRequest& request) {
            ASSERT_EQUALS("test", request.dbname);

            ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

            ASSERT_EQ(string("dummy"), request.cmdObj.firstElementFieldName());
            checkReadConcern(request.cmdObj, lastOpTime.getTimestamp(), lastOpTime.getTerm());

            ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
            BSONObjBuilder builder;
            metadata.writeToMetadata(&builder);

            return RemoteCommandResponse(BSON("ok" << 1), builder.obj(), Milliseconds(1));
        });

        // Now wait for the runReadCommand call to return
        future.timed_get(kFutureTimeout);

        lastOpTime = newOpTime;
    }
}

TEST_F(ShardingCatalogClientTest, ReadAfterOpTimeShouldNotGoBack) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    // Initialize the internal config OpTime
    auto future1 = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        ASSERT_TRUE(getCatalogClient()->runReadCommandForTest(
            operationContext(), "test", BSON("dummy" << 1), &responseBuilder));
    });

    OpTime highestOpTime;
    const OpTime newOpTime(Timestamp(7, 6), 5);

    onCommandWithMetadata([this, &newOpTime, &highestOpTime](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("test", request.dbname);

        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(string("dummy"), request.cmdObj.firstElementFieldName());
        checkReadConcern(request.cmdObj, highestOpTime.getTimestamp(), highestOpTime.getTerm());

        ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
        BSONObjBuilder builder;
        metadata.writeToMetadata(&builder);

        return RemoteCommandResponse(BSON("ok" << 1), builder.obj(), Milliseconds(1));
    });

    future1.timed_get(kFutureTimeout);

    highestOpTime = newOpTime;

    // Return an older OpTime
    auto future2 = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        ASSERT_TRUE(getCatalogClient()->runReadCommandForTest(
            operationContext(), "test", BSON("dummy" << 1), &responseBuilder));
    });

    const OpTime oldOpTime(Timestamp(3, 10), 5);

    onCommandWithMetadata([this, &oldOpTime, &highestOpTime](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("test", request.dbname);

        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(string("dummy"), request.cmdObj.firstElementFieldName());
        checkReadConcern(request.cmdObj, highestOpTime.getTimestamp(), highestOpTime.getTerm());

        ReplSetMetadata metadata(10, oldOpTime, oldOpTime, 100, OID(), 30, -1);
        BSONObjBuilder builder;
        metadata.writeToMetadata(&builder);

        return RemoteCommandResponse(BSON("ok" << 1), builder.obj(), Milliseconds(1));
    });

    future2.timed_get(kFutureTimeout);

    // Check that older OpTime does not override highest OpTime
    auto future3 = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        ASSERT_TRUE(getCatalogClient()->runReadCommandForTest(
            operationContext(), "test", BSON("dummy" << 1), &responseBuilder));
    });

    onCommandWithMetadata([this, &oldOpTime, &highestOpTime](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("test", request.dbname);

        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(string("dummy"), request.cmdObj.firstElementFieldName());
        checkReadConcern(request.cmdObj, highestOpTime.getTimestamp(), highestOpTime.getTerm());

        ReplSetMetadata metadata(10, oldOpTime, oldOpTime, 100, OID(), 30, -1);
        BSONObjBuilder builder;
        metadata.writeToMetadata(&builder);

        return RemoteCommandResponse(BSON("ok" << 1), builder.obj(), Milliseconds(1));
    });

    future3.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, ReadAfterOpTimeFindThenCmd) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future1 = launchAsync([this] {
        ASSERT_OK(catalogClient()->getDatabase(operationContext(), "TestDB").getStatus());
    });

    OpTime highestOpTime;
    const OpTime newOpTime(Timestamp(7, 6), 5);

    onFindWithMetadataCommand(
        [this, &newOpTime, &highestOpTime](const RemoteCommandRequest& request) {
            ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);
            checkReadConcern(request.cmdObj, highestOpTime.getTimestamp(), highestOpTime.getTerm());

            ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
            BSONObjBuilder builder;
            metadata.writeToMetadata(&builder);

            DatabaseType dbType;
            dbType.setName("TestDB");
            dbType.setPrimary(ShardId("TestShard"));
            dbType.setSharded("true");

            return std::make_tuple(vector<BSONObj>{dbType.toBSON()}, builder.obj());
        });

    future1.timed_get(kFutureTimeout);

    highestOpTime = newOpTime;

    // Return an older OpTime
    auto future2 = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        ASSERT_TRUE(getCatalogClient()->runReadCommandForTest(
            operationContext(), "test", BSON("dummy" << 1), &responseBuilder));
    });

    const OpTime oldOpTime(Timestamp(3, 10), 5);

    onCommand([this, &oldOpTime, &highestOpTime](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("test", request.dbname);

        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(string("dummy"), request.cmdObj.firstElementFieldName());
        checkReadConcern(request.cmdObj, highestOpTime.getTimestamp(), highestOpTime.getTerm());

        return BSON("ok" << 1);
    });

    future2.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, ReadAfterOpTimeCmdThenFind) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    // Initialize the internal config OpTime
    auto future1 = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        ASSERT_TRUE(getCatalogClient()->runReadCommandForTest(
            operationContext(), "test", BSON("dummy" << 1), &responseBuilder));
    });

    OpTime highestOpTime;
    const OpTime newOpTime(Timestamp(7, 6), 5);

    onCommandWithMetadata([this, &newOpTime, &highestOpTime](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("test", request.dbname);

        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(string("dummy"), request.cmdObj.firstElementFieldName());
        checkReadConcern(request.cmdObj, highestOpTime.getTimestamp(), highestOpTime.getTerm());

        ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
        BSONObjBuilder builder;
        metadata.writeToMetadata(&builder);

        return RemoteCommandResponse(BSON("ok" << 1), builder.obj(), Milliseconds(1));
    });

    future1.timed_get(kFutureTimeout);

    highestOpTime = newOpTime;

    // Return an older OpTime
    auto future2 = launchAsync([this] {
        ASSERT_OK(catalogClient()->getDatabase(operationContext(), "TestDB").getStatus());
    });

    const OpTime oldOpTime(Timestamp(3, 10), 5);

    onFindCommand([this, &oldOpTime, &highestOpTime](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(string("find"), request.cmdObj.firstElementFieldName());
        checkReadConcern(request.cmdObj, highestOpTime.getTimestamp(), highestOpTime.getTerm());

        DatabaseType dbType;
        dbType.setName("TestDB");
        dbType.setPrimary(ShardId("TestShard"));
        dbType.setSharded("true");

        return vector<BSONObj>{dbType.toBSON()};
    });

    future2.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RetryOnReadCommandNetworkErrorFailsAtMaxRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future1 = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        auto ok = getCatalogClient()->runReadCommandForTest(
            operationContext(), "test", BSON("dummy" << 1), &responseBuilder);
        ASSERT_FALSE(ok);
        auto status = getStatusFromCommandResult(responseBuilder.obj());
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.code());
    });

    for (int i = 0; i < kMaxCommandRetry; ++i) {
        onCommand([](const RemoteCommandRequest&) {
            return Status{ErrorCodes::HostUnreachable, "bad host"};
        });
    }

    future1.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RetryOnReadCommandNetworkErrorSucceedsAtMaxRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONObj expectedResult = BSON("ok" << 1 << "yes"
                                       << "dummy");

    auto future1 = launchAsync([this, expectedResult] {
        BSONObjBuilder responseBuilder;
        auto ok = getCatalogClient()->runReadCommandForTest(
            operationContext(), "test", BSON("dummy" << 1), &responseBuilder);
        ASSERT_TRUE(ok);
        auto response = responseBuilder.obj();
        ASSERT_EQ(expectedResult, response);
    });

    for (int i = 0; i < kMaxCommandRetry - 1; ++i) {
        onCommand([](const RemoteCommandRequest&) {
            return Status{ErrorCodes::HostUnreachable, "bad host"};
        });
    }

    onCommand([expectedResult](const RemoteCommandRequest& request) { return expectedResult; });

    future1.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RetryOnFindCommandNetworkErrorFailsAtMaxRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->getDatabase(operationContext(), "TestDB");
        ASSERT_EQ(ErrorCodes::HostUnreachable, status.getStatus().code());
    });

    for (int i = 0; i < kMaxCommandRetry; ++i) {
        onFindCommand([](const RemoteCommandRequest&) {
            return Status{ErrorCodes::HostUnreachable, "bad host"};
        });
    }

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RetryOnFindCommandNetworkErrorSucceedsAtMaxRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync(
        [&] { ASSERT_OK(catalogClient()->getDatabase(operationContext(), "TestDB").getStatus()); });

    for (int i = 0; i < kMaxCommandRetry - 1; ++i) {
        onFindCommand([](const RemoteCommandRequest&) {
            return Status{ErrorCodes::HostUnreachable, "bad host"};
        });
    }

    onFindCommand([](const RemoteCommandRequest& request) {
        DatabaseType dbType;
        dbType.setName("TestDB");
        dbType.setPrimary(ShardId("TestShard"));
        dbType.setSharded("true");

        return vector<BSONObj>{dbType.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
