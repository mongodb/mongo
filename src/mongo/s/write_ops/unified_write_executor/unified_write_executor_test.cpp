/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/s/write_ops/unified_write_executor/unified_write_executor.h"

#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/sharding_environment/mongos_server_parameters_gen.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/s/commands/query_cmd/populate_cursor.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/unified_write_executor/write_op.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unified_write_executor {
namespace {

class UnifiedWriteExecutorTest : public ShardingTestFixture {
public:
    const ShardId shardId1 = ShardId("shard1");
    const ShardId shardId2 = ShardId("shard2");
    const int port = 12345;

    void setUp() override {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(HostAndPort("config", port));

        std::vector<std::tuple<ShardId, HostAndPort>> remoteShards{
            {shardId1, HostAndPort(shardId1.toString(), port)},
            {shardId2, HostAndPort(shardId2.toString(), port)},
        };

        std::vector<ShardType> shards;
        for (size_t i = 0; i < remoteShards.size(); i++) {
            ShardType shardType;
            shardType.setName(get<0>(remoteShards[i]).toString());
            shardType.setHost(get<1>(remoteShards[i]).toString());
            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(get<1>(remoteShards[i])));
            targeter->setFindHostReturnValue(get<1>(remoteShards[i]));
            targeterFactory()->addTargeterToReturn(ConnectionString(get<1>(remoteShards[i])),
                                                   std::move(targeter));
        }
        setupShards(shards);
    }

    // Like expectCollectionRoutingRequest but uses a caller-supplied epoch, timestamp, and uuid so
    // that repeated calls return the same collection identity and ShardVersion (needed to simulate
    // an unproductive refresh).
    void expectCollectionRoutingRequestWithVersion(
        NamespaceString nss, ShardId shardId, OID epoch, Timestamp timestamp, UUID uuid) {
        onFindCommand([=, this](const executor::RemoteCommandRequest&) {
            CollectionType coll(nss, epoch, timestamp, Date_t::now(), uuid, BSON("a" << 1));
            ChunkType chunk(uuid,
                            ChunkRange(BSON("a" << MINKEY), BSON("a" << MAXKEY)),
                            ChunkVersion({epoch, timestamp}, {1, 0}),
                            shardId);
            chunk.setName(OID::gen());
            return std::vector<BSONObj>{coll.toBSON(), BSON("chunks" << chunk.toConfigBSON())};
        });
    }

    void expectDatabaseRoutingRequest(DatabaseName dbName, ShardId shardId) {
        onFindCommand([=, this](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.dbname.toString_forTest(), "config");
            ASSERT_EQ(request.cmdObj.getField("find").String(), "databases");
            ASSERT_BSONOBJ_EQ(request.cmdObj.getField("filter").Obj(),
                              BSON("_id" << dbName.toString_forTest()));

            DatabaseType db(dbName,
                            shardId,
                            DatabaseVersion(UUID::gen(), Timestamp(++_routingVersionCounter, 1)));
            return std::vector<BSONObj>{db.toBSON()};
        });
    }

    // Like expectDatabaseRoutingRequest but returns a fixed DatabaseVersion (needed to simulate
    // an unproductive database-version refresh).
    void expectDatabaseRoutingRequestWithVersion(DatabaseName dbName,
                                                 ShardId shardId,
                                                 UUID uuid,
                                                 Timestamp timestamp) {
        onFindCommand([=, this](const executor::RemoteCommandRequest&) {
            DatabaseType db(dbName, shardId, DatabaseVersion(uuid, timestamp));
            return std::vector<BSONObj>{db.toBSON()};
        });
    }

    // Intercepts the collection routing lookup for an untracked collection by returning an empty
    // result. This causes the catalog cache to record "no routing table" for the namespace.
    void expectUntrackedCollectionRoutingRequest(NamespaceString nss) {
        onFindCommand([=, this](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.dbname.toString_forTest(), "config");
            ASSERT_EQ(request.cmdObj.getField("aggregate").String(), "collections");
            return std::vector<BSONObj>{};
        });
        // An untracked collection triggers a follow-up lookup for the timeseries buckets namespace.
        onFindCommand([=, this](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.dbname.toString_forTest(), "config");
            ASSERT_EQ(request.cmdObj.getField("aggregate").String(), "collections");
            return std::vector<BSONObj>{};
        });
    }

    void expectCollectionRoutingRequest(NamespaceString nss, ShardId shardId) {
        onFindCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.dbname.toString_forTest(), "config");
            ASSERT_EQ(request.cmdObj.getField("aggregate").String(), "collections");
            ASSERT_BSONOBJ_EQ(request.cmdObj.getField("pipeline").Array()[0].Obj(),
                              BSON("$match" << BSON("_id" << nss.toString_forTest())));

            auto uuid = UUID::gen();
            auto epoch = OID::gen();
            auto timestamp = Timestamp(++_routingVersionCounter, 1);
            CollectionType coll(nss, epoch, timestamp, Date_t::now(), uuid, BSON("a" << 1));
            ChunkType chunk(uuid,
                            ChunkRange(BSON("a" << MINKEY), BSON("a" << MAXKEY)),
                            ChunkVersion({epoch, timestamp}, {1, 0}),
                            shardId);
            chunk.setName(OID::gen());
            return std::vector<BSONObj>{coll.toBSON(), BSON("chunks" << chunk.toConfigBSON())};
        });
    }

    void expectBulkWriteShardRequest(std::vector<BSONObj> opList,
                                     std::vector<NamespaceString> nssList,
                                     ShardId shardId,
                                     std::vector<BSONObj> replyItems,
                                     int nErrors,
                                     int nInserted,
                                     int nMatched,
                                     int nModified,
                                     int nUpserted,
                                     int nDeleted) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            auto opMsg = static_cast<OpMsgRequest>(request);
            BulkWriteCommandRequest bulkWrite =
                BulkWriteCommandRequest::parse(opMsg.body, IDLParserContext("bulkWrite"));
            ASSERT_EQ(opList.size(), bulkWrite.getOps().size());
            for (size_t i = 0; i < opList.size(); i++) {
                ASSERT_BSONOBJ_EQ(BulkWriteCRUDOp(bulkWrite.getOps()[i]).toBSON(), opList[i]);
            }
            ASSERT_EQ(nssList.size(), bulkWrite.getNsInfo().size());
            for (size_t i = 0; i < nssList.size(); i++) {
                ASSERT_EQ(bulkWrite.getNsInfo()[i].getNs(), nssList[i]);
            }
            BSONArrayBuilder firstBatch;
            for (const auto& replyItem : replyItems) {
                firstBatch.append(replyItem);
            }
            return BSON("cursor" << BSON("id" << 0ll << "firstBatch" << firstBatch.arr() << "ns"
                                              << "admin.$cmd.bulkWrite")
                                 << "nErrors" << nErrors << "nInserted" << nInserted << "nMatched"
                                 << nMatched << "nModified" << nModified << "nUpserted" << nUpserted
                                 << "nDeleted" << nDeleted);
        });
    }

    void expectInsertShardRequest(std::vector<BSONObj> opList, const NamespaceString& nss, int n) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            auto opMsg = static_cast<OpMsgRequest>(request);
            BatchedCommandRequest bcr = BatchedCommandRequest::parseInsert(opMsg);
            WriteCommandRef cmdRef(bcr);

            ASSERT_EQ(opList.size(), cmdRef.getNumOps());

            for (size_t i = 0; i < opList.size(); i++) {
                auto bulkWriteOp = write_op_helpers::getOrMakeBulkWriteOp(cmdRef.getOp(i));
                ASSERT_BSONOBJ_EQ(BulkWriteCRUDOp(bulkWriteOp).toBSON(), opList[i]);
            }

            ASSERT_EQ(nss, bcr.getNS());

            return BSON("ok" << 1 << "n" << n);
        });
    }

    void expectUpdateShardRequest(std::vector<BSONObj> opList,
                                  const NamespaceString& nss,
                                  int n,
                                  int nModified) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            auto opMsg = static_cast<OpMsgRequest>(request);
            BatchedCommandRequest bcr = BatchedCommandRequest::parseUpdate(opMsg);
            WriteCommandRef cmdRef(bcr);

            ASSERT_EQ(opList.size(), cmdRef.getNumOps());

            for (size_t i = 0; i < opList.size(); i++) {
                auto bulkWriteOp = write_op_helpers::getOrMakeBulkWriteOp(cmdRef.getOp(i));
                ASSERT_BSONOBJ_EQ(BulkWriteCRUDOp(bulkWriteOp).toBSON(), opList[i]);
            }

            ASSERT_EQ(nss, bcr.getNS());

            return BSON("ok" << 1 << "n" << n << "nModified" << nModified);
        });
    }

    void expectDeleteShardRequest(std::vector<BSONObj> opList, const NamespaceString& nss, int n) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            auto opMsg = static_cast<OpMsgRequest>(request);
            BatchedCommandRequest bcr = BatchedCommandRequest::parseDelete(opMsg);
            WriteCommandRef cmdRef(bcr);

            ASSERT_EQ(opList.size(), cmdRef.getNumOps());

            for (size_t i = 0; i < opList.size(); i++) {
                auto bulkWriteOp = write_op_helpers::getOrMakeBulkWriteOp(cmdRef.getOp(i));
                ASSERT_BSONOBJ_EQ(BulkWriteCRUDOp(bulkWriteOp).toBSON(), opList[i]);
            }

            ASSERT_EQ(nss, bcr.getNS());

            return BSON("ok" << 1 << "n" << n);
        });
    }

private:
    uint32_t _routingVersionCounter = 0;
};

TEST_F(UnifiedWriteExecutorTest, BulkWriteBasic) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(dbName, "coll1");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(dbName, "coll2");
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << 2))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});

    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        auto replyItems = reply.getCursor().getFirstBatch();
        ASSERT_EQ(replyItems.size(), 2);
        ASSERT_BSONOBJ_EQ(replyItems[0].toBSON(), BSON("ok" << 1.0 << "idx" << 0 << "n" << 1));
        ASSERT_BSONOBJ_EQ(replyItems[1].toBSON(), BSON("ok" << 1.0 << "idx" << 1 << "n" << 1));
        ASSERT_EQ(reply.getNErrors(), 0);
        ASSERT_EQ(reply.getNInserted(), 2);
        ASSERT_EQ(reply.getNMatched(), 0);
        ASSERT_EQ(reply.getNModified(), 0);
        ASSERT_EQ(reply.getNUpserted(), 0);
        ASSERT_EQ(reply.getNDeleted(), 0);
    });

    // Load catalog cache from the config server
    expectDatabaseRoutingRequest(dbName, shardId1);
    expectCollectionRoutingRequest(nss1, shardId1);
    expectCollectionRoutingRequest(nss2, shardId2);

    // First batch of ops for shard1
    expectBulkWriteShardRequest({BSON("insert" << 0 << "document" << BSON("x" << 1))} /* opList */,
                                {nss1} /* nssList */,
                                shardId1 /* shardId */,
                                {BSON("ok" << 1.0 << "idx" << 0 << "n" << 1)} /* replyItems */,
                                0 /* nErrors */,
                                1 /* nInserted */,
                                0 /* nMatched */,
                                0 /* nModified */,
                                0 /* nUpserted */,
                                0 /* nDelete */
    );

    // Second batch of ops for shard 2
    expectBulkWriteShardRequest({BSON("insert" << 0 << "document" << BSON("x" << 2))} /* opList */,
                                {nss2} /* nssList */,
                                shardId2 /* shardId */,
                                {BSON("ok" << 1.0 << "idx" << 0 << "n" << 1)} /* replyItems */,
                                0 /* nErrors */,
                                1 /* nInserted */,
                                0 /* nMatched */,
                                0 /* nModified */,
                                0 /* nUpserted */,
                                0 /* nDelete */
    );

    future.default_timed_get();
}

TEST_F(UnifiedWriteExecutorTest, BatchWriteBasic) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll1");

    // Insert two documents
    BatchedCommandRequest insertRequest([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());


    auto future = launchAsync([&]() {
        Stats uweStats;
        auto resp = write(operationContext(), insertRequest, uweStats);
        ASSERT(resp.getOk());
        ASSERT_FALSE(resp.isErrDetailsSet());
        ASSERT_EQ(resp.getN(), 2);
        ASSERT_EQ(resp.getNModified(), 0);
        ASSERT_FALSE(resp.isUpsertDetailsSet());
    });


    // Load catalog cache from the config server
    expectDatabaseRoutingRequest(dbName, shardId1);
    expectCollectionRoutingRequest(nss, shardId1);

    // Single insert batch.
    expectInsertShardRequest({BSON("insert" << 0 << "document" << BSON("x" << 1)),
                              BSON("insert" << 0 << "document" << BSON("x" << 2))},
                             nss,
                             2);

    future.default_timed_get();

    // Now, update one doc.
    BatchedCommandRequest updateRequest([&] {
        write_ops::UpdateCommandRequest updateOp(nss);
        updateOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        updateOp.setUpdates(std::vector{write_ops::UpdateOpEntry(
            BSON("x" << 2),
            write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 3)))});
        return updateOp;
    }());

    auto updateFuture = launchAsync([&]() {
        Stats uweStats;
        auto resp = write(operationContext(), updateRequest, uweStats);
        ASSERT(resp.getOk());
        ASSERT_FALSE(resp.isErrDetailsSet());
        ASSERT_EQ(resp.getN(), 1);
        ASSERT_EQ(resp.getNModified(), 1);
        ASSERT_FALSE(resp.isUpsertDetailsSet());
    });

    // Single update batch.
    expectUpdateShardRequest(
        {BSON("update" << 0 << "filter" << BSON("x" << 2) << "multi" << false << "updateMods"
                       << BSON("x" << 3) << "upsert" << false)},
        nss,
        1,
        1);

    updateFuture.default_timed_get();

    // Delete the updated doc.
    BatchedCommandRequest deleteRequest([&] {
        write_ops::DeleteCommandRequest deleteOp(nss);
        deleteOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase writeCommandBase;
            writeCommandBase.setOrdered(true);
            return writeCommandBase;
        }());
        deleteOp.setDeletes(std::vector{write_ops::DeleteOpEntry(BSON("x" << 3), false)});
        return deleteOp;
    }());

    auto deleteFuture = launchAsync([&]() {
        Stats uweStats;
        auto resp = write(operationContext(), deleteRequest, uweStats);
        ASSERT(resp.getOk());
        ASSERT_FALSE(resp.isErrDetailsSet());
        ASSERT_EQ(resp.getN(), 1);
        ASSERT_EQ(resp.getNModified(), 0);
        ASSERT_FALSE(resp.isUpsertDetailsSet());
    });

    // Single delete batch.
    expectDeleteShardRequest(
        {BSON("delete" << 0 << "filter" << BSON("x" << 3) << "multi" << false)}, nss, 1);

    deleteFuture.default_timed_get();
}

TEST_F(UnifiedWriteExecutorTest, BulkWriteImplicitCollectionCreation) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(dbName, "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss1)});

    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        auto replyItems = reply.getCursor().getFirstBatch();
        ASSERT_EQ(replyItems.size(), 1);
        ASSERT_BSONOBJ_EQ(replyItems[0].toBSON(), BSON("ok" << 1.0 << "idx" << 0 << "n" << 1));
        ASSERT_EQ(reply.getNErrors(), 0);
        ASSERT_EQ(reply.getNInserted(), 1);
        ASSERT_EQ(reply.getNMatched(), 0);
        ASSERT_EQ(reply.getNModified(), 0);
        ASSERT_EQ(reply.getNUpserted(), 0);
        ASSERT_EQ(reply.getNDeleted(), 0);
    });

    // Load catalog cache from the config server
    expectDatabaseRoutingRequest(dbName, shardId1);
    expectCollectionRoutingRequest(nss1, shardId1);

    // First try implicit create collection error
    expectBulkWriteShardRequest(
        {BSON("insert" << 0 << "document" << BSON("x" << 1))} /* opList */,
        {nss1} /* nssList */,
        shardId1 /* shardId */,
        {BSON("ok" << 0.0 << "idx" << 0 << "code" << ErrorCodes::CannotImplicitlyCreateCollection
                   << "ns" << nss1.toString_forTest() << "n" << 0)} /* replyItems */,
        1 /* nErrors */,
        0 /* nInserted */,
        0 /* nMatched */,
        0 /* nModified */,
        0 /* nUpserted */,
        0 /* nDelete */
    );

    // Create collection explicitly
    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        ASSERT_EQ(request.dbname, nss1.dbName());
        ASSERT_EQ(request.cmdObj.getField("_shardsvrCreateCollection").String(), nss1.coll());
        BSONObjBuilder shardVersionBuilder;
        ShardVersion::UNTRACKED().serialize("", &shardVersionBuilder);
        return BSON("ok" << 1.0 << "collectionUUID" << UUID::gen() << "collectionVersion"
                         << shardVersionBuilder.obj().firstElement().Obj());
    });

    // Load catalog cache from the config server
    expectCollectionRoutingRequest(nss1, shardId1);

    // Second try successful
    expectBulkWriteShardRequest({BSON("insert" << 0 << "document" << BSON("x" << 1))} /* opList */,
                                {nss1} /* nssList */,
                                shardId1 /* shardId */,
                                {BSON("ok" << 1.0 << "idx" << 0 << "n" << 1)} /* replyItems */,
                                0 /* nErrors */,
                                1 /* nInserted */,
                                0 /* nMatched */,
                                0 /* nModified */,
                                0 /* nUpserted */,
                                0 /* nDelete */
    );

    future.default_timed_get();
}

TEST_F(UnifiedWriteExecutorTest, OrderedBulkWriteErrorsAndStops) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(dbName, "coll1");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(dbName, "coll2");
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << 2))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    request.setOrdered(true);

    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        auto replyItems = reply.getCursor().getFirstBatch();
        ASSERT_EQ(replyItems.size(), 1);
        ASSERT_BSONOBJ_EQ(replyItems[0].toBSON(),
                          BSON("ok" << 0.0 << "idx" << 0 << "code" << ErrorCodes::BadValue
                                    << "errmsg" << "failed" << "n" << 0));
        ASSERT_EQ(reply.getNErrors(), 1);
        ASSERT_EQ(reply.getNInserted(), 0);
        ASSERT_EQ(reply.getNMatched(), 0);
        ASSERT_EQ(reply.getNModified(), 0);
        ASSERT_EQ(reply.getNUpserted(), 0);
        ASSERT_EQ(reply.getNDeleted(), 0);
    });

    // Load catalog cache from the config server
    expectDatabaseRoutingRequest(dbName, shardId1);
    expectCollectionRoutingRequest(nss1, shardId1);
    expectCollectionRoutingRequest(nss2, shardId2);

    // First batch, then errors and stops
    expectBulkWriteShardRequest(
        {BSON("insert" << 0 << "document" << BSON("x" << 1))} /* opList */,
        {nss1} /* nssList */,
        shardId1 /* shardId */,
        {BSON("ok" << 0.0 << "idx" << 0 << "code" << ErrorCodes::BadValue << "errmsg"
                   << "failed"
                   << "n" << 0)} /* replyItems */,
        1 /* nErrors */,
        0 /* nInserted */,
        0 /* nMatched */,
        0 /* nModified */,
        0 /* nUpserted */,
        0 /* nDelete */
    );

    future.default_timed_get();
}

TEST_F(UnifiedWriteExecutorTest, UnorderedBulkWriteErrorsAndStops) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(dbName, "coll1");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(dbName, "coll2");
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << 2))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});
    request.setOrdered(false);


    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        auto replyItems = reply.getCursor().getFirstBatch();
        ASSERT_EQ(replyItems.size(), 2);
        ASSERT_BSONOBJ_EQ(replyItems[0].toBSON(),
                          BSON("ok" << 0.0 << "idx" << 0 << "code" << ErrorCodes::BadValue
                                    << "errmsg" << "failed" << "n" << 0));
        ASSERT_BSONOBJ_EQ(replyItems[1].toBSON(), BSON("ok" << 1.0 << "idx" << 1 << "n" << 1));
        ASSERT_EQ(reply.getNErrors(), 1);
        ASSERT_EQ(reply.getNInserted(), 1);
        ASSERT_EQ(reply.getNMatched(), 0);
        ASSERT_EQ(reply.getNModified(), 0);
        ASSERT_EQ(reply.getNUpserted(), 0);
        ASSERT_EQ(reply.getNDeleted(), 0);
    });

    // Load catalog cache from the config server
    expectDatabaseRoutingRequest(dbName, shardId1);
    expectCollectionRoutingRequest(nss1, shardId1);
    expectCollectionRoutingRequest(nss2, shardId2);

    // First batch, returns an error.
    expectBulkWriteShardRequest(
        {BSON("insert" << 0 << "document" << BSON("x" << 1))} /* opList */,
        {nss1} /* nssList */,
        shardId1 /* shardId */,
        {BSON("ok" << 0.0 << "idx" << 0 << "code" << ErrorCodes::BadValue << "errmsg"
                   << "failed"
                   << "n" << 0)} /* replyItems */,
        1 /* nErrors */,
        0 /* nInserted */,
        0 /* nMatched */,
        0 /* nModified */,
        0 /* nUpserted */,
        0 /* nDelete */
    );

    // Second batch of ops for shard 2
    expectBulkWriteShardRequest({BSON("insert" << 0 << "document" << BSON("x" << 2))} /* opList */,
                                {nss2} /* nssList */,
                                shardId2 /* shardId */,
                                {BSON("ok" << 1.0 << "idx" << 0 << "n" << 1)} /* replyItems */,
                                0 /* nErrors */,
                                1 /* nInserted */,
                                0 /* nMatched */,
                                0 /* nModified */,
                                0 /* nUpserted */,
                                0 /* nDelete */
    );

    future.default_timed_get();
}

namespace {

// Returns a bulkWrite cursor response with a StaleDbVersion error for each op in the request,
// using the DatabaseVersion the scheduler actually sent as receivedVersion.
BSONObj makeBulkWriteStaleDbVersionResponse(const NamespaceString& nss,
                                            const executor::RemoteCommandRequest& request) {
    const auto opMsgRequest = static_cast<OpMsgRequest>(request);
    const auto bulkWrite =
        BulkWriteCommandRequest::parse(opMsgRequest.body, IDLParserContext("bulkWrite"));
    const auto& ops = bulkWrite.getOps();

    boost::optional<DatabaseVersion> receivedVersion;
    for (const auto& nsEntry : bulkWrite.getNsInfo()) {
        if (nsEntry.getNs() == nss) {
            receivedVersion = nsEntry.getDatabaseVersion();
            break;
        }
    }
    tassert(12378202, "NSS not found in bulkWrite nsInfo", receivedVersion.has_value());

    BSONArrayBuilder firstBatch;
    for (size_t i = 0; i < ops.size(); ++i) {
        BulkWriteReplyItem item(
            static_cast<int>(i),
            Status(StaleDbRoutingVersion(nss.dbName(), *receivedVersion, boost::none),
                   "Stale db version error"));
        firstBatch.append(item.serialize());
    }

    return BSON("cursor" << BSON("id" << 0ll << "firstBatch" << firstBatch.arr() << "ns"
                                      << "admin.$cmd.bulkWrite")
                         << "nErrors" << static_cast<int>(ops.size()) << "nInserted" << 0
                         << "nMatched" << 0 << "nModified" << 0 << "nUpserted" << 0 << "nDeleted"
                         << 0);
}

// Returns a bulkWrite cursor response with a StaleConfig error for each op in the request,
// using the ShardVersion the scheduler actually sent as receivedVersion.
BSONObj makeBulkWriteStaleConfigResponse(const NamespaceString& nss,
                                         const executor::RemoteCommandRequest& request,
                                         const ShardId& shardId) {
    const auto opMsgRequest = static_cast<OpMsgRequest>(request);
    const auto bulkWrite =
        BulkWriteCommandRequest::parse(opMsgRequest.body, IDLParserContext("bulkWrite"));
    const auto& ops = bulkWrite.getOps();

    boost::optional<ShardVersion> receivedVersion;
    for (const auto& nsEntry : bulkWrite.getNsInfo()) {
        if (nsEntry.getNs() == nss) {
            receivedVersion = nsEntry.getShardVersion();
            break;
        }
    }
    tassert(12378200,
            "NSS not found in bulkWrite nsInfo with shardVersion",
            receivedVersion.has_value());

    // Pass boost::none as wantedVersion so the routing cache does a forced refresh rather than
    // trying to advance to a specific version the test mock cannot satisfy.
    BSONArrayBuilder firstBatch;
    for (size_t i = 0; i < ops.size(); ++i) {
        BulkWriteReplyItem item(
            static_cast<int>(i),
            Status(StaleConfigInfo(nss, *receivedVersion, boost::none, shardId), "Stale error"));
        firstBatch.append(item.serialize());
    }

    return BSON("cursor" << BSON("id" << 0ll << "firstBatch" << firstBatch.arr() << "ns"
                                      << "admin.$cmd.bulkWrite")
                         << "nErrors" << static_cast<int>(ops.size()) << "nInserted" << 0
                         << "nMatched" << 0 << "nModified" << 0 << "nUpserted" << 0 << "nDeleted"
                         << 0);
}

}  // namespace

// When each stale round is followed by a routing refresh that returns new metadata
// (different ShardVersion), metadataRefreshed=true resets numRoundsWithoutProgress each time.
// Even kMaxRoundsWithoutProgress+1 stale rounds should therefore not trigger NoProgressMade.
TEST_F(UnifiedWriteExecutorTest, StaleRoundsWithProductiveRefreshSucceed) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");

    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss)});

    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        ASSERT_EQ(reply.getNErrors(), 0);
        ASSERT_EQ(reply.getNInserted(), 1);
    });

    expectDatabaseRoutingRequest(dbName, shardId1);
    expectCollectionRoutingRequest(nss, shardId1);

    // Run kMaxRoundsWithoutProgress+1 stale rounds. Each call to expectCollectionRoutingRequest
    // increments _routingVersionCounter, producing a distinct Timestamp and therefore a different
    // ChunkVersion, making metadataRefreshed=true after every refresh.
    const int kStaleRounds = gMaxRoundsWithoutProgress.loadRelaxed() + 1;
    for (int i = 0; i < kStaleRounds; ++i) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return makeBulkWriteStaleConfigResponse(nss, request, shardId1);
        });
        expectCollectionRoutingRequest(nss, shardId1);
    }

    expectBulkWriteShardRequest({BSON("insert" << 0 << "document" << BSON("x" << 1))},
                                {nss},
                                shardId1,
                                {BSON("ok" << 1.0 << "idx" << 0 << "n" << 1)},
                                0,
                                1,
                                0,
                                0,
                                0,
                                0);

    future.default_timed_get();
}

// When each stale round's routing refresh returns the same ShardVersion as the receivedVersion
// in the stale error, metadataRefreshed=false and numRoundsWithoutProgress increments normally.
// After kMaxRoundsWithoutProgress+1 unproductive stale rounds, NoProgressMade is returned.
TEST_F(UnifiedWriteExecutorTest, StaleRoundsWithUnproductiveRefreshReturnsNoProgressMade) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");

    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss)});

    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        ASSERT_EQ(reply.getNErrors(), 1);
        const auto& status = reply.getCursor().getFirstBatch()[0].getStatus();
        ASSERT_EQ(status.code(), ErrorCodes::NoProgressMade);
        const int kMax = gMaxRoundsWithoutProgress.loadRelaxed();
        ASSERT_STRING_CONTAINS(status.reason(), str::stream() << kMax << " rounds");
        ASSERT_STRING_CONTAINS(status.reason(), str::stream() << (kMax + 1) << " rounds total");
        ASSERT_EQ(reply.getNInserted(), 0);
    });

    // Use a fixed epoch, timestamp, and UUID so every routing refresh returns an identical
    // collection entry → metadataRefreshed=false every round.
    const OID fixedEpoch = OID::gen();
    const Timestamp fixedTimestamp(1, 1);
    const UUID fixedUUID = UUID::gen();

    expectDatabaseRoutingRequest(dbName, shardId1);
    expectCollectionRoutingRequestWithVersion(nss, shardId1, fixedEpoch, fixedTimestamp, fixedUUID);

    // Each stale round: the shard echoes back the same receivedVersion, and the refresh returns
    // the same version, so metadataRefreshed=false and numRoundsWithoutProgress increments.
    // NoProgressMade fires after kMaxRoundsWithoutProgress+1 rounds (the check is strict >).
    const int kStaleRounds = gMaxRoundsWithoutProgress.loadRelaxed() + 1;
    for (int i = 0; i < kStaleRounds; ++i) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return makeBulkWriteStaleConfigResponse(nss, request, shardId1);
        });
        // After the last stale round numRoundsWithoutProgress exceeds the limit, so run() breaks
        // out of the loop before issuing another routing request.
        if (i < kStaleRounds - 1) {
            expectCollectionRoutingRequestWithVersion(
                nss, shardId1, fixedEpoch, fixedTimestamp, fixedUUID);
        }
    }

    future.default_timed_get();
}

// When each StaleDbVersion round is followed by a database routing refresh that returns a new
// DatabaseVersion, metadataRefreshed=true resets numRoundsWithoutProgress each time. Even
// kMaxRoundsWithoutProgress+1 stale rounds should not trigger NoProgressMade.
TEST_F(UnifiedWriteExecutorTest, StaleDbRoundsWithProductiveRefreshSucceed) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");

    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss)});

    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        ASSERT_EQ(reply.getNErrors(), 0);
        ASSERT_EQ(reply.getNInserted(), 1);
    });

    // Initial routing: DB version V1, collection is untracked (empty routing table).
    expectDatabaseRoutingRequest(dbName, shardId1);
    expectUntrackedCollectionRoutingRequest(nss);

    // Run kMaxRoundsWithoutProgress+1 stale rounds. Each call to expectDatabaseRoutingRequest
    // increments _routingVersionCounter, producing a distinct Timestamp and therefore a different
    // DatabaseVersion each time, so metadataRefreshed=true after every refresh.
    // The collection routing entry stays cached across rounds (onStaleDatabaseVersion only
    // invalidates the database cache entry, not per-collection entries).
    const int kStaleRounds = gMaxRoundsWithoutProgress.loadRelaxed() + 1;
    for (int i = 0; i < kStaleRounds; ++i) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return makeBulkWriteStaleDbVersionResponse(nss, request);
        });
        expectDatabaseRoutingRequest(dbName, shardId1);
    }

    expectBulkWriteShardRequest({BSON("insert" << 0 << "document" << BSON("x" << 1))},
                                {nss},
                                shardId1,
                                {BSON("ok" << 1.0 << "idx" << 0 << "n" << 1)},
                                0,
                                1,
                                0,
                                0,
                                0,
                                0);

    future.default_timed_get();
}

// When each StaleDbVersion round's database refresh returns the same DatabaseVersion,
// metadataRefreshed=false and numRoundsWithoutProgress increments normally. After
// kMaxRoundsWithoutProgress+1 unproductive rounds, NoProgressMade is returned.
TEST_F(UnifiedWriteExecutorTest, StaleDbRoundsWithUnproductiveRefreshReturnsNoProgressMade) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(dbName, "coll");

    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss)});

    auto future = launchAsync([&]() {
        Stats uweStats;
        auto replyInfo = bulkWrite(operationContext(), request, uweStats);
        auto reply = populateCursorReply(operationContext(), request, request.toBSON(), replyInfo);
        ASSERT_EQ(reply.getNErrors(), 1);
        const auto& status = reply.getCursor().getFirstBatch()[0].getStatus();
        ASSERT_EQ(status.code(), ErrorCodes::NoProgressMade);
        const int kMax = gMaxRoundsWithoutProgress.loadRelaxed();
        ASSERT_STRING_CONTAINS(status.reason(), str::stream() << kMax << " rounds");
        ASSERT_STRING_CONTAINS(status.reason(), str::stream() << (kMax + 1) << " rounds total");
        ASSERT_EQ(reply.getNInserted(), 0);
    });

    // Use a fixed UUID and timestamp so every database refresh returns the same DatabaseVersion,
    // making metadataRefreshed=false each round.
    const UUID fixedUUID = UUID::gen();
    const Timestamp fixedTimestamp(1, 1);

    expectDatabaseRoutingRequestWithVersion(dbName, shardId1, fixedUUID, fixedTimestamp);
    expectUntrackedCollectionRoutingRequest(nss);

    // Each stale round: the shard echoes back the same receivedVersion, the refresh returns the
    // same DatabaseVersion, so numRoundsWithoutProgress increments each time.
    // NoProgressMade fires after kMaxRoundsWithoutProgress+1 rounds (the check is strict >).
    const int kStaleRounds = gMaxRoundsWithoutProgress.loadRelaxed() + 1;
    for (int i = 0; i < kStaleRounds; ++i) {
        onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
            return makeBulkWriteStaleDbVersionResponse(nss, request);
        });
        // After the last stale round numRoundsWithoutProgress exceeds the limit, so run() breaks
        // out of the loop before issuing another routing request.
        if (i < kStaleRounds - 1) {
            expectDatabaseRoutingRequestWithVersion(dbName, shardId1, fixedUUID, fixedTimestamp);
        }
    }

    future.default_timed_get();
}

}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
