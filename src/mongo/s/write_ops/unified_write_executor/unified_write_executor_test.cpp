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

#include "mongo/s/sharding_mongos_test_fixture.h"
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

    void expectDatabaseRoutingRequest(DatabaseName dbName, ShardId shardId) {
        onFindCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.dbname.toString_forTest(), "config");
            ASSERT_EQ(request.cmdObj.getField("find").String(), "databases");
            ASSERT_BSONOBJ_EQ(request.cmdObj.getField("filter").Obj(),
                              BSON("_id" << dbName.toString_forTest()));

            DatabaseType db(dbName, shardId, DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
            return std::vector<BSONObj>{db.toBSON()};
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
            auto timestamp = Timestamp(1, 1);
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
                BulkWriteCommandRequest::parse(IDLParserContext("bulkWrite"), opMsg.body);
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
};

TEST_F(UnifiedWriteExecutorTest, BulkWriteBasic) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(dbName, "coll1");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(dbName, "coll2");
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSON("x" << 1)), BulkWriteInsertOp(1, BSON("x" << 2))},
        {NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});

    auto future = launchAsync([&]() {
        auto reply = bulkWrite(operationContext(), request);
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

    // First batch
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

    // Second batch
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

TEST_F(UnifiedWriteExecutorTest, BulkWriteImplicitCollectionCreation) {
    const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(dbName, "coll");
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSON("x" << 1))},
                                    {NamespaceInfoEntry(nss1)});

    auto future = launchAsync([&]() {
        auto reply = bulkWrite(operationContext(), request);
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
        ShardVersion::UNSHARDED().serialize("", &shardVersionBuilder);
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
        auto reply = bulkWrite(operationContext(), request);
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
        {BSON("ok" << 0.0 << "idx" << 0 << "code" << ErrorCodes::BadValue << "errmsg" << "failed"
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
}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
