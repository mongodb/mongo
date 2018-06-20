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
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/tracking_metadata.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/database_version_helpers.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/write_ops/batched_command_response.h"
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
using std::vector;
using unittest::assertGet;

using ShardingCatalogClientTest = ShardingTestFixture;

const int kMaxCommandRetry = 3;
const NamespaceString kNamespace("TestDB", "TestColl");

BSONObj getReplSecondaryOkMetadata() {
    BSONObjBuilder o;
    ReadPreferenceSetting(ReadPreference::Nearest).toContainingBSON(&o);
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}

TEST_F(ShardingCatalogClientTest, GetCollectionExisting) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    CollectionType expectedColl;
    expectedColl.setNs(NamespaceString("TestDB.TestNS"));
    expectedColl.setKeyPattern(BSON("KeyName" << 1));
    expectedColl.setUpdatedAt(Date_t());
    expectedColl.setEpoch(OID::gen());

    const OpTime newOpTime(Timestamp(7, 6), 5);

    auto future = launchAsync([this, &expectedColl] {
        return assertGet(catalogClient()->getCollection(operationContext(), expectedColl.getNs()));
    });

    onFindWithMetadataCommand(
        [this, &expectedColl, newOpTime](const RemoteCommandRequest& request) {

            ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                              rpc::TrackingMetadata::removeTrackingData(request.metadata));

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss, CollectionType::ConfigNS);

            auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

            // Ensure the query is correct
            ASSERT_EQ(query->nss(), CollectionType::ConfigNS);
            ASSERT_BSONOBJ_EQ(query->getFilter(),
                              BSON(CollectionType::fullNs(expectedColl.getNs().ns())));
            ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());
            ASSERT_EQ(query->getLimit().get(), 1);

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
            BSONObjBuilder builder;
            metadata.writeToMetadata(&builder).transitional_ignore();

            return std::make_tuple(vector<BSONObj>{expectedColl.toBSON()}, builder.obj());
        });

    // Now wait for the getCollection call to return
    const auto collOpTimePair = future.timed_get(kFutureTimeout);
    ASSERT_EQ(newOpTime, collOpTimePair.opTime);
    ASSERT_BSONOBJ_EQ(expectedColl.toBSON(), collOpTimePair.value.toBSON());
}

TEST_F(ShardingCatalogClientTest, GetCollectionNotExisting) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        auto status =
            catalogClient()->getCollection(operationContext(), NamespaceString("NonExistent"));
        ASSERT_EQUALS(status.getStatus(), ErrorCodes::NamespaceNotFound);
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    // Now wait for the getCollection call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetDatabaseInvalidName) {
    auto status =
        catalogClient()
            ->getDatabase(operationContext(), "b.c", repl::ReadConcernLevel::kMajorityReadConcern)
            .getStatus();
    ASSERT_EQ(ErrorCodes::InvalidNamespace, status.code());
    ASSERT_FALSE(status.reason().empty());
}

TEST_F(ShardingCatalogClientTest, GetDatabaseExisting) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    DatabaseType expectedDb("bigdata", ShardId("shard0000"), true, databaseVersion::makeNew());

    const OpTime newOpTime(Timestamp(7, 6), 5);

    auto future = launchAsync([this, &expectedDb] {
        return assertGet(
            catalogClient()->getDatabase(operationContext(),
                                         expectedDb.getName(),
                                         repl::ReadConcernLevel::kMajorityReadConcern));
    });

    onFindWithMetadataCommand([this, &expectedDb, newOpTime](const RemoteCommandRequest& request) {
        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, DatabaseType::ConfigNS);

        ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->nss(), DatabaseType::ConfigNS);
        ASSERT_BSONOBJ_EQ(query->getFilter(), BSON(DatabaseType::name(expectedDb.getName())));
        ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());
        ASSERT(!query->getLimit());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
        BSONObjBuilder builder;
        metadata.writeToMetadata(&builder).transitional_ignore();

        return std::make_tuple(vector<BSONObj>{expectedDb.toBSON()}, builder.obj());
    });

    const auto dbOpTimePair = future.timed_get(kFutureTimeout);
    ASSERT_EQ(newOpTime, dbOpTimePair.opTime);
    ASSERT_BSONOBJ_EQ(expectedDb.toBSON(), dbOpTimePair.value.toBSON());
}

TEST_F(ShardingCatalogClientTest, GetDatabaseStaleSecondaryRetrySuccess) {
    HostAndPort firstHost{"TestHost1"};
    HostAndPort secondHost{"TestHost2"};
    configTargeter()->setFindHostReturnValue(firstHost);

    DatabaseType expectedDb("bigdata", ShardId("shard0000"), true, databaseVersion::makeNew());

    auto future = launchAsync([this, &expectedDb] {
        return assertGet(
            catalogClient()->getDatabase(operationContext(),
                                         expectedDb.getName(),
                                         repl::ReadConcernLevel::kMajorityReadConcern));
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
    ASSERT_BSONOBJ_EQ(expectedDb.toBSON(), dbOpTimePair.value.toBSON());
}

TEST_F(ShardingCatalogClientTest, GetDatabaseStaleSecondaryRetryNoPrimary) {
    HostAndPort testHost{"TestHost1"};
    configTargeter()->setFindHostReturnValue(testHost);

    auto future = launchAsync([this] {
        auto dbResult = catalogClient()->getDatabase(
            operationContext(), "NonExistent", repl::ReadConcernLevel::kMajorityReadConcern);
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
        auto dbResult = catalogClient()->getDatabase(
            operationContext(), "NonExistent", repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_EQ(dbResult.getStatus(), ErrorCodes::NamespaceNotFound);
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });
    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

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
        auto shards = assertGet(catalogClient()->getAllShards(
            operationContext(), repl::ReadConcernLevel::kMajorityReadConcern));
        return shards.value;
    });

    onFindCommand([this, &s1, &s2, &s3](const RemoteCommandRequest& request) {
        ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, ShardType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->nss(), ShardType::ConfigNS);
        ASSERT_BSONOBJ_EQ(query->getFilter(), BSONObj());
        ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{s1.toBSON(), s2.toBSON(), s3.toBSON()};
    });

    const vector<ShardType> actualShardsList = future.timed_get(kFutureTimeout);
    ASSERT_EQ(actualShardsList.size(), expectedShardsList.size());

    for (size_t i = 0; i < actualShardsList.size(); ++i) {
        ASSERT_BSONOBJ_EQ(actualShardsList[i].toBSON(), expectedShardsList[i].toBSON());
    }
}

TEST_F(ShardingCatalogClientTest, GetAllShardsWithInvalidShard) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->getAllShards(operationContext(),
                                                    repl::ReadConcernLevel::kMajorityReadConcern);

        ASSERT_EQ(ErrorCodes::NoSuchKey, status.getStatus());
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
    chunkA.setNS(kNamespace);
    chunkA.setMin(BSON("a" << 1));
    chunkA.setMax(BSON("a" << 100));
    chunkA.setVersion({1, 2, oid});
    chunkA.setShard(ShardId("shard0000"));

    ChunkType chunkB;
    chunkB.setNS(kNamespace);
    chunkB.setMin(BSON("a" << 100));
    chunkB.setMax(BSON("a" << 200));
    chunkB.setVersion({3, 4, oid});
    chunkB.setShard(ShardId("shard0001"));

    ChunkVersion queryChunkVersion({1, 2, oid});

    const BSONObj chunksQuery(
        BSON(ChunkType::ns("TestDB.TestColl")
             << ChunkType::lastmod()
             << BSON("$gte" << static_cast<long long>(queryChunkVersion.toLong()))));

    const OpTime newOpTime(Timestamp(7, 6), 5);

    auto future = launchAsync([this, &chunksQuery, newOpTime] {
        OpTime opTime;

        const auto chunks =
            assertGet(catalogClient()->getChunks(operationContext(),
                                                 chunksQuery,
                                                 BSON(ChunkType::lastmod() << -1),
                                                 1,
                                                 &opTime,
                                                 repl::ReadConcernLevel::kMajorityReadConcern));
        ASSERT_EQ(2U, chunks.size());
        ASSERT_EQ(newOpTime, opTime);

        return chunks;
    });

    onFindWithMetadataCommand(
        [this, &chunksQuery, chunkA, chunkB, newOpTime](const RemoteCommandRequest& request) {
            ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                              rpc::TrackingMetadata::removeTrackingData(request.metadata));

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss, ChunkType::ConfigNS);

            auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

            ASSERT_EQ(query->nss(), ChunkType::ConfigNS);
            ASSERT_BSONOBJ_EQ(query->getFilter(), chunksQuery);
            ASSERT_BSONOBJ_EQ(query->getSort(), BSON(ChunkType::lastmod() << -1));
            ASSERT_EQ(query->getLimit().get(), 1);

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
            BSONObjBuilder builder;
            metadata.writeToMetadata(&builder).transitional_ignore();

            return std::make_tuple(vector<BSONObj>{chunkA.toConfigBSON(), chunkB.toConfigBSON()},
                                   builder.obj());
        });

    const auto& chunks = future.timed_get(kFutureTimeout);
    ASSERT_BSONOBJ_EQ(chunkA.toConfigBSON(), chunks[0].toConfigBSON());
    ASSERT_BSONOBJ_EQ(chunkB.toConfigBSON(), chunks[1].toConfigBSON());
}

TEST_F(ShardingCatalogClientTest, GetChunksForNSNoSortNoLimit) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    ChunkVersion queryChunkVersion({1, 2, OID::gen()});

    const BSONObj chunksQuery(
        BSON(ChunkType::ns("TestDB.TestColl")
             << ChunkType::lastmod()
             << BSON("$gte" << static_cast<long long>(queryChunkVersion.toLong()))));

    auto future = launchAsync([this, &chunksQuery] {
        const auto chunks =
            assertGet(catalogClient()->getChunks(operationContext(),
                                                 chunksQuery,
                                                 BSONObj(),
                                                 boost::none,
                                                 nullptr,
                                                 repl::ReadConcernLevel::kMajorityReadConcern));
        ASSERT_EQ(0U, chunks.size());

        return chunks;
    });

    onFindCommand([this, &chunksQuery](const RemoteCommandRequest& request) {
        ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, ChunkType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->nss(), ChunkType::ConfigNS);
        ASSERT_BSONOBJ_EQ(query->getFilter(), chunksQuery);
        ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());
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
             << ChunkType::lastmod()
             << BSON("$gte" << static_cast<long long>(queryChunkVersion.toLong()))));

    auto future = launchAsync([this, &chunksQuery] {
        const auto swChunks =
            catalogClient()->getChunks(operationContext(),
                                       chunksQuery,
                                       BSONObj(),
                                       boost::none,
                                       nullptr,
                                       repl::ReadConcernLevel::kMajorityReadConcern);

        ASSERT_EQUALS(ErrorCodes::NoSuchKey, swChunks.getStatus());
    });

    onFindCommand([&chunksQuery](const RemoteCommandRequest& request) {
        ChunkType chunkA;
        chunkA.setNS(kNamespace);
        chunkA.setMin(BSON("a" << 1));
        chunkA.setMax(BSON("a" << 100));
        chunkA.setVersion({1, 2, OID::gen()});
        chunkA.setShard(ShardId("shard0000"));

        ChunkType chunkB;
        chunkB.setNS(kNamespace);
        chunkB.setMin(BSON("a" << 100));
        chunkB.setMax(BSON("a" << 200));
        chunkB.setVersion({3, 4, OID::gen()});
        // Missing shard id

        return vector<BSONObj>{chunkA.toConfigBSON(), chunkB.toConfigBSON()};
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
        const BSONObj kReplPrimaryPreferredMetadata = ([] {
            BSONObjBuilder o;
            ReadPreferenceSetting(ReadPreference::PrimaryPreferred).toContainingBSON(&o);
            o.append(rpc::kReplSetMetadataFieldName, 1);
            return o.obj();
        }());

        ASSERT_BSONOBJ_EQ(kReplPrimaryPreferredMetadata,
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        ASSERT_EQUALS("test", request.dbname);
        ASSERT_BSONOBJ_EQ(BSON("usersInfo" << 1 << "maxTimeMS" << 30000), request.cmdObj);

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
        ASSERT_BSONOBJ_EQ(BSON("dropUser"
                               << "test"
                               << "writeConcern"
                               << BSON("w"
                                       << "majority"
                                       << "wtimeout"
                                       << 0)
                               << "maxTimeMS"
                               << 30000),
                          request.cmdObj);

        ASSERT_BSONOBJ_EQ(BSON(rpc::kReplSetMetadataFieldName << 1),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        BSONObjBuilder responseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            responseBuilder, Status(ErrorCodes::UserNotFound, "User test@test not found"));
        return responseBuilder.obj();
    });

    // Now wait for the runUserManagementWriteCommand call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RunUserManagementWriteCommandInvalidWriteConcern) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

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

TEST_F(ShardingCatalogClientTest, RunUserManagementWriteCommandRewriteWriteConcern) {
    // Tests that if you send a w:1 write concern it gets replaced with w:majority
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    distLock()->expectLock(
        [](StringData name, StringData whyMessage, Milliseconds waitFor) {
            ASSERT_EQUALS("authorizationData", name);
            ASSERT_EQUALS("dropUser", whyMessage);
        },
        Status::OK());

    auto future = launchAsync([this] {
        BSONObjBuilder responseBuilder;
        bool ok = catalogClient()->runUserManagementWriteCommand(operationContext(),
                                                                 "dropUser",
                                                                 "test",
                                                                 BSON("dropUser"
                                                                      << "test"
                                                                      << "writeConcern"
                                                                      << BSON("w" << 1 << "wtimeout"
                                                                                  << 30)),
                                                                 &responseBuilder);
        ASSERT_FALSE(ok);

        Status commandStatus = getStatusFromCommandResult(responseBuilder.obj());
        ASSERT_EQUALS(ErrorCodes::UserNotFound, commandStatus);
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("test", request.dbname);
        ASSERT_BSONOBJ_EQ(BSON("dropUser"
                               << "test"
                               << "writeConcern"
                               << BSON("w"
                                       << "majority"
                                       << "wtimeout"
                                       << 30)
                               << "maxTimeMS"
                               << 30000),
                          request.cmdObj);

        ASSERT_BSONOBJ_EQ(BSON(rpc::kReplSetMetadataFieldName << 1),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        BSONObjBuilder responseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            responseBuilder, Status(ErrorCodes::UserNotFound, "User test@test not found"));
        return responseBuilder.obj();
    });

    // Now wait for the runUserManagementWriteCommand call to return
    future.timed_get(kFutureTimeout);
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
            CommandHelpers::appendCommandStatusNoThrow(responseBuilder,
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
        CommandHelpers::appendCommandStatusNoThrow(responseBuilder,
                                                   Status(ErrorCodes::NotMaster, "not master"));

        // Ensure that when the catalog manager tries to retarget after getting the
        // NotMaster response, it will get back a new target.
        configTargeter()->setFindHostReturnValue(host2);
        return responseBuilder.obj();
    });

    onCommand([host2](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(host2, request.target);
        ASSERT_EQUALS("test", request.dbname);
        // Since no write concern was sent we will add w:majority
        ASSERT_BSONOBJ_EQ(BSON("dropUser"
                               << "test"
                               << "writeConcern"
                               << BSON("w"
                                       << "majority"
                                       << "wtimeout"
                                       << 0)
                               << "maxTimeMS"
                               << 30000),
                          request.cmdObj);

        ASSERT_BSONOBJ_EQ(BSON(rpc::kReplSetMetadataFieldName << 1),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

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

        OpTime opTime;
        const auto& collections =
            assertGet(catalogClient()->getCollections(operationContext(), nullptr, &opTime));

        ASSERT_EQ(newOpTime, opTime);

        return std::move(collections);
    });

    onFindWithMetadataCommand(
        [this, coll1, coll2, coll3, newOpTime](const RemoteCommandRequest& request) {
            ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                              rpc::TrackingMetadata::removeTrackingData(request.metadata));

            const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
            ASSERT_EQ(nss, CollectionType::ConfigNS);

            auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

            ASSERT_EQ(query->nss(), CollectionType::ConfigNS);
            ASSERT_BSONOBJ_EQ(query->getFilter(), BSONObj());
            ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());

            checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

            ReplSetMetadata metadata(10, newOpTime, newOpTime, 100, OID(), 30, -1);
            BSONObjBuilder builder;
            metadata.writeToMetadata(&builder).transitional_ignore();

            return std::make_tuple(vector<BSONObj>{coll1.toBSON(), coll2.toBSON(), coll3.toBSON()},
                                   builder.obj());
        });

    const auto& actualColls = future.timed_get(kFutureTimeout);
    ASSERT_EQ(3U, actualColls.size());
    ASSERT_BSONOBJ_EQ(coll1.toBSON(), actualColls[0].toBSON());
    ASSERT_BSONOBJ_EQ(coll2.toBSON(), actualColls[1].toBSON());
    ASSERT_BSONOBJ_EQ(coll3.toBSON(), actualColls[2].toBSON());
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
        const std::string dbName = "test";
        return assertGet(catalogClient()->getCollections(operationContext(), &dbName, nullptr));
    });

    onFindCommand([this, coll1, coll2](const RemoteCommandRequest& request) {
        ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, CollectionType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->nss(), CollectionType::ConfigNS);
        {
            BSONObjBuilder b;
            b.appendRegex(CollectionType::fullNs(), "^test\\.");
            ASSERT_BSONOBJ_EQ(query->getFilter(), b.obj());
        }

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{coll1.toBSON(), coll2.toBSON()};
    });

    const auto& actualColls = future.timed_get(kFutureTimeout);
    ASSERT_EQ(2U, actualColls.size());
    ASSERT_BSONOBJ_EQ(coll1.toBSON(), actualColls[0].toBSON());
    ASSERT_BSONOBJ_EQ(coll2.toBSON(), actualColls[1].toBSON());
}

TEST_F(ShardingCatalogClientTest, GetCollectionsInvalidCollectionType) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        const std::string dbName = "test";
        const auto swCollections =
            catalogClient()->getCollections(operationContext(), &dbName, nullptr);

        ASSERT_EQ(ErrorCodes::FailedToParse, swCollections.getStatus());
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
        ASSERT_EQ(nss, CollectionType::ConfigNS);

        ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->nss(), CollectionType::ConfigNS);
        {
            BSONObjBuilder b;
            b.appendRegex(CollectionType::fullNs(), "^test\\.");
            ASSERT_BSONOBJ_EQ(query->getFilter(), b.obj());
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

    DatabaseType dbt1("db1", ShardId("shard0000"), false, databaseVersion::makeNew());
    DatabaseType dbt2("db2", ShardId("shard0000"), false, databaseVersion::makeNew());

    auto future = launchAsync([this] {
        return assertGet(
            catalogClient()->getDatabasesForShard(operationContext(), ShardId("shard0000")));
    });

    onFindCommand([this, dbt1, dbt2](const RemoteCommandRequest& request) {
        ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, DatabaseType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->nss(), DatabaseType::ConfigNS);
        ASSERT_BSONOBJ_EQ(query->getFilter(),
                          BSON(DatabaseType::primary(dbt1.getPrimary().toString())));
        ASSERT_BSONOBJ_EQ(query->getSort(), BSONObj());

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
        const auto swDatabaseNames =
            catalogClient()->getDatabasesForShard(operationContext(), ShardId("shard0000"));

        ASSERT_EQ(ErrorCodes::TypeMismatch, swDatabaseNames.getStatus());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        DatabaseType dbt1("db1", {"shard0000"}, false, databaseVersion::makeNew());
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
    tagA.setNS(NamespaceString("TestDB.TestColl"));
    tagA.setTag("TagA");
    tagA.setMinKey(BSON("a" << 100));
    tagA.setMaxKey(BSON("a" << 200));

    TagsType tagB;
    tagB.setNS(NamespaceString("TestDB.TestColl"));
    tagB.setTag("TagB");
    tagB.setMinKey(BSON("a" << 200));
    tagB.setMaxKey(BSON("a" << 300));

    auto future = launchAsync([this] {
        const auto& tags = assertGet(catalogClient()->getTagsForCollection(
            operationContext(), NamespaceString("TestDB.TestColl")));

        ASSERT_EQ(2U, tags.size());

        return tags;
    });

    onFindCommand([this, tagA, tagB](const RemoteCommandRequest& request) {
        ASSERT_BSONOBJ_EQ(getReplSecondaryOkMetadata(),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(nss, TagsType::ConfigNS);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        ASSERT_EQ(query->nss(), TagsType::ConfigNS);
        ASSERT_BSONOBJ_EQ(query->getFilter(), BSON(TagsType::ns("TestDB.TestColl")));
        ASSERT_BSONOBJ_EQ(query->getSort(), BSON(TagsType::min() << 1));

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{tagA.toBSON(), tagB.toBSON()};
    });

    const auto& tags = future.timed_get(kFutureTimeout);
    ASSERT_BSONOBJ_EQ(tagA.toBSON(), tags[0].toBSON());
    ASSERT_BSONOBJ_EQ(tagB.toBSON(), tags[1].toBSON());
}

TEST_F(ShardingCatalogClientTest, GetTagsForCollectionNoTags) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        const auto& tags = assertGet(catalogClient()->getTagsForCollection(
            operationContext(), NamespaceString("TestDB.TestColl")));

        ASSERT_EQ(0U, tags.size());

        return tags;
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetTagsForCollectionInvalidTag) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        const auto swTags = catalogClient()->getTagsForCollection(
            operationContext(), NamespaceString("TestDB.TestColl"));

        ASSERT_EQUALS(ErrorCodes::NoSuchKey, swTags.getStatus());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        TagsType tagA;
        tagA.setNS(NamespaceString("TestDB.TestColl"));
        tagA.setTag("TagA");
        tagA.setMinKey(BSON("a" << 100));
        tagA.setMaxKey(BSON("a" << 200));

        TagsType tagB;
        tagB.setNS(NamespaceString("TestDB.TestColl"));
        tagB.setTag("TagB");
        tagB.setMinKey(BSON("a" << 200));
        // Missing maxKey

        return vector<BSONObj>{tagA.toBSON(), tagB.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateDatabase) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    DatabaseType dbt("test", ShardId("shard0000"), true, databaseVersion::makeNew());

    auto future = launchAsync([this, dbt] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  DatabaseType::ConfigNS,
                                                  BSON(DatabaseType::name(dbt.getName())),
                                                  dbt.toBSON(),
                                                  true,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_OK(status);
    });

    onCommand([dbt](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("config", request.dbname);

        ASSERT_BSONOBJ_EQ(BSON(rpc::kReplSetMetadataFieldName << 1),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));

        const auto opMsgRequest = OpMsgRequest::fromDBAndBody(request.dbname, request.cmdObj);
        const auto updateOp = UpdateOp::parse(opMsgRequest);
        ASSERT_EQUALS(DatabaseType::ConfigNS, updateOp.getNamespace());

        const auto& updates = updateOp.getUpdates();
        ASSERT_EQUALS(1U, updates.size());

        const auto& update = updates.front();
        ASSERT(update.getUpsert());
        ASSERT(!update.getMulti());
        ASSERT_BSONOBJ_EQ(update.getQ(), BSON(DatabaseType::name(dbt.getName())));
        ASSERT_BSONOBJ_EQ(update.getU(), dbt.toBSON());

        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    // Now wait for the updateDatabase call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, UpdateConfigDocumentExceededTimeLimit) {
    HostAndPort host1("TestHost1");
    configTargeter()->setFindHostReturnValue(host1);

    DatabaseType dbt("test", ShardId("shard0001"), false, databaseVersion::makeNew());

    auto future = launchAsync([this, dbt] {
        auto status =
            catalogClient()->updateConfigDocument(operationContext(),
                                                  DatabaseType::ConfigNS,
                                                  BSON(DatabaseType::name(dbt.getName())),
                                                  dbt.toBSON(),
                                                  true,
                                                  ShardingCatalogClient::kMajorityWriteConcern);
        ASSERT_EQ(ErrorCodes::ExceededTimeLimit, status);
    });

    onCommand([host1](const RemoteCommandRequest& request) {
        try {
            ASSERT_EQUALS(host1, request.target);

            BatchedCommandResponse response;
            response.setStatus({ErrorCodes::ExceededTimeLimit, "operation timed out"});

            return response.toBSON();
        } catch (const DBException& ex) {
            BSONObjBuilder bb;
            CommandHelpers::appendCommandStatusNoThrow(bb, ex.toStatus());
            return bb.obj();
        }
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
    const NamespaceString nss("config.chunks");
    ChunkVersion lastChunkVersion(0, 0, OID());

    auto future = launchAsync([this, updateOps, preCondition, nss, lastChunkVersion] {
        auto status =
            catalogClient()->applyChunkOpsDeprecated(operationContext(),
                                                     updateOps,
                                                     preCondition,
                                                     nss,
                                                     lastChunkVersion,
                                                     ShardingCatalogClient::kMajorityWriteConcern,
                                                     repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_OK(status);
    });

    onCommand([updateOps, preCondition, nss](const RemoteCommandRequest& request) {
        ASSERT_EQUALS("config", request.dbname);
        ASSERT_BSONOBJ_EQ(BSON("w"
                               << "majority"
                               << "wtimeout"
                               << 15000),
                          request.cmdObj["writeConcern"].Obj());
        ASSERT_BSONOBJ_EQ(BSON(rpc::kReplSetMetadataFieldName << 1),
                          rpc::TrackingMetadata::removeTrackingData(request.metadata));
        ASSERT_BSONOBJ_EQ(updateOps, request.cmdObj["applyOps"].Obj());
        ASSERT_BSONOBJ_EQ(preCondition, request.cmdObj["preCondition"].Obj());

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
    const NamespaceString nss("config.chunks");
    ChunkVersion lastChunkVersion(0, 0, OID());

    auto future = launchAsync([this, updateOps, preCondition, nss, lastChunkVersion] {
        auto status =
            catalogClient()->applyChunkOpsDeprecated(operationContext(),
                                                     updateOps,
                                                     preCondition,
                                                     nss,
                                                     lastChunkVersion,
                                                     ShardingCatalogClient::kMajorityWriteConcern,
                                                     repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_OK(status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BSONObjBuilder responseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            responseBuilder, Status(ErrorCodes::DuplicateKey, "precondition failed"));
        return responseBuilder.obj();
    });

    onFindCommand([this](const RemoteCommandRequest& request) {
        OID oid = OID::gen();
        ChunkType chunk;
        chunk.setNS(kNamespace);
        chunk.setMin(BSON("a" << 1));
        chunk.setMax(BSON("a" << 100));
        chunk.setVersion({1, 2, oid});
        chunk.setShard(ShardId("shard0000"));
        return vector<BSONObj>{chunk.toConfigBSON()};
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
    const NamespaceString nss("config.chunks");
    ChunkVersion lastChunkVersion(0, 0, OID());

    auto future = launchAsync([this, updateOps, preCondition, nss, lastChunkVersion] {
        auto status =
            catalogClient()->applyChunkOpsDeprecated(operationContext(),
                                                     updateOps,
                                                     preCondition,
                                                     nss,
                                                     lastChunkVersion,
                                                     ShardingCatalogClient::kMajorityWriteConcern,
                                                     repl::ReadConcernLevel::kMajorityReadConcern);
        ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, status);
    });

    onCommand([&](const RemoteCommandRequest& request) {
        BSONObjBuilder responseBuilder;
        CommandHelpers::appendCommandStatusNoThrow(
            responseBuilder, Status(ErrorCodes::NoMatchingDocument, "some error"));
        return responseBuilder.obj();
    });

    onFindCommand([this](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    // Now wait for the applyChunkOpsDeprecated call to return
    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, RetryOnFindCommandNetworkErrorFailsAtMaxRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->getDatabase(
            operationContext(), "TestDB", repl::ReadConcernLevel::kMajorityReadConcern);
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

    auto future = launchAsync([&] {
        ASSERT_OK(catalogClient()
                      ->getDatabase(operationContext(),
                                    "TestDB",
                                    repl::ReadConcernLevel::kMajorityReadConcern)
                      .getStatus());
    });

    for (int i = 0; i < kMaxCommandRetry - 1; ++i) {
        onFindCommand([](const RemoteCommandRequest&) {
            return Status{ErrorCodes::HostUnreachable, "bad host"};
        });
    }

    onFindCommand([](const RemoteCommandRequest& request) {
        DatabaseType dbType("TestDB", ShardId("TestShard"), true, databaseVersion::makeNew());

        return vector<BSONObj>{dbType.toBSON()};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientTest, GetNewKeys) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    std::string purpose("none");
    LogicalTime currentTime(Timestamp(1234, 5678));
    repl::ReadConcernLevel readConcernLevel(repl::ReadConcernLevel::kMajorityReadConcern);

    auto future = launchAsync([this, purpose, currentTime, readConcernLevel] {
        auto status =
            catalogClient()->getNewKeys(operationContext(), purpose, currentTime, readConcernLevel);
        ASSERT_OK(status.getStatus());
        return status.getValue();
    });

    LogicalTime dummyTime(Timestamp(9876, 5432));
    auto randomKey1 = TimeProofService::generateRandomKey();
    KeysCollectionDocument key1(1, "none", randomKey1, dummyTime);

    LogicalTime dummyTime2(Timestamp(123456, 789));
    auto randomKey2 = TimeProofService::generateRandomKey();
    KeysCollectionDocument key2(2, "none", randomKey2, dummyTime2);

    onFindCommand([this, key1, key2](const RemoteCommandRequest& request) {
        ASSERT_EQ("config:123", request.target.toString());
        ASSERT_EQ("admin", request.dbname);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(KeysCollectionDocument::ConfigNS, nss);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        BSONObj expectedQuery(
            fromjson("{purpose: 'none',"
                     "expiresAt: {$gt: {$timestamp: {t: 1234, i: 5678}}}}"));

        ASSERT_EQ(KeysCollectionDocument::ConfigNS, query->nss());
        ASSERT_BSONOBJ_EQ(expectedQuery, query->getFilter());
        ASSERT_BSONOBJ_EQ(BSON("expiresAt" << 1), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{key1.toBSON(), key2.toBSON()};
    });

    const auto keyDocs = future.timed_get(kFutureTimeout);
    ASSERT_EQ(2u, keyDocs.size());

    const auto& key1Result = keyDocs.front();
    ASSERT_EQ(1, key1Result.getKeyId());
    ASSERT_EQ("none", key1Result.getPurpose());
    ASSERT_EQ(randomKey1, key1Result.getKey());
    ASSERT_EQ(Timestamp(9876, 5432), key1Result.getExpiresAt().asTimestamp());

    const auto& key2Result = keyDocs.back();
    ASSERT_EQ(2, key2Result.getKeyId());
    ASSERT_EQ("none", key2Result.getPurpose());
    ASSERT_EQ(randomKey2, key2Result.getKey());
    ASSERT_EQ(Timestamp(123456, 789), key2Result.getExpiresAt().asTimestamp());
}

TEST_F(ShardingCatalogClientTest, GetNewKeysWithEmptyCollection) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    std::string purpose("none");
    LogicalTime currentTime(Timestamp(1234, 5678));
    repl::ReadConcernLevel readConcernLevel(repl::ReadConcernLevel::kMajorityReadConcern);

    auto future = launchAsync([this, purpose, currentTime, readConcernLevel] {
        auto status =
            catalogClient()->getNewKeys(operationContext(), purpose, currentTime, readConcernLevel);
        ASSERT_OK(status.getStatus());
        return status.getValue();
    });

    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ("config:123", request.target.toString());
        ASSERT_EQ("admin", request.dbname);

        const NamespaceString nss(request.dbname, request.cmdObj.firstElement().String());
        ASSERT_EQ(KeysCollectionDocument::ConfigNS, nss);

        auto query = assertGet(QueryRequest::makeFromFindCommand(nss, request.cmdObj, false));

        BSONObj expectedQuery(
            fromjson("{purpose: 'none',"
                     "expiresAt: {$gt: {$timestamp: {t: 1234, i: 5678}}}}"));

        ASSERT_EQ(KeysCollectionDocument::ConfigNS, query->nss());
        ASSERT_BSONOBJ_EQ(expectedQuery, query->getFilter());
        ASSERT_BSONOBJ_EQ(BSON("expiresAt" << 1), query->getSort());
        ASSERT_FALSE(query->getLimit().is_initialized());

        checkReadConcern(request.cmdObj, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        return vector<BSONObj>{};
    });

    const auto keyDocs = future.timed_get(kFutureTimeout);
    ASSERT_EQ(0u, keyDocs.size());
}

}  // namespace
}  // namespace mongo
