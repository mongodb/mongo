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

#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unified_write_executor {
namespace {

class WriteBatchExecutorTest : public ShardingTestFixture {
public:
    const ShardId shardId1 = ShardId("shard1");
    const ShardId shardId2 = ShardId("shard2");

    void setUp() override {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(HostAndPort("config", 12345));

        std::vector<std::tuple<ShardId, HostAndPort>> remoteShards{
            {shardId1, HostAndPort(shardId1.toString(), 12345)},
            {shardId2, HostAndPort(shardId2.toString(), 12345)},
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

    NamespaceInfoEntry getNamespaceInfoEntry(const NamespaceString& nss,
                                             boost::optional<ShardVersion> shardVersion,
                                             boost::optional<DatabaseVersion> databaseVersion) {

        NamespaceInfoEntry entry(nss);
        entry.setShardVersion(shardVersion);
        entry.setDatabaseVersion(databaseVersion);
        return entry;
    }

    void assertBulkWriteRequest(BSONObj cmdObj,
                                std::vector<BSONObj> expectedOps,
                                std::vector<NamespaceInfoEntry> expectedNsInfos,
                                boost::optional<LogicalSessionId> expectedLsid,
                                boost::optional<TxnNumber> expectedTxnNumber,
                                boost::optional<const WriteConcernOptions&> expectedWriteConcern) {
        ASSERT_EQ(cmdObj.getField("bulkWrite").number(), 1);

        auto ops = cmdObj.getField("ops").Array();
        ASSERT_EQ(ops.size(), expectedOps.size());
        for (size_t i = 0; i < ops.size(); i++) {
            ASSERT_BSONOBJ_EQ(ops[i].Obj(), expectedOps[i]);
        }

        auto nsInfos = cmdObj.getField("nsInfo").Array();
        ASSERT_EQ(nsInfos.size(), expectedNsInfos.size());
        for (size_t i = 0; i < nsInfos.size(); i++) {
            auto nsInfo = nsInfos[i].Obj();
            ASSERT_EQ(nsInfo.getField("ns").String(),
                      expectedNsInfos[i].getNs().toString_forTest());
            if (expectedNsInfos[i].getDatabaseVersion()) {
                ASSERT_EQ(*expectedNsInfos[i].getDatabaseVersion(),
                          DatabaseVersion(nsInfo.getField("databaseVersion").Obj()));
            }
            if (expectedNsInfos[i].getShardVersion()) {
                ASSERT_EQ(*expectedNsInfos[i].getShardVersion(),
                          ShardVersion::parse(nsInfo.getField("shardVersion")));
            }
        }

        if (expectedLsid) {
            ASSERT_BSONOBJ_EQ(expectedLsid->toBSON(), cmdObj.getField("lsid").Obj());
        }
        if (expectedTxnNumber) {
            ASSERT_EQ(*expectedTxnNumber, cmdObj.getField("txnNumber").number());
        }
        if (expectedWriteConcern) {
            ASSERT_BSONOBJ_EQ(expectedWriteConcern->toBSON(),
                              cmdObj.getField("writeConcern").Obj());
        }
    }
};

TEST_F(WriteBatchExecutorTest, ExecuteSimpleWriteBatch) {
    const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test", "coll2");
    // We create a bulkRequest with three ops, the insert op and the delete op run against
    // the same namespace nss1, and the update op runs against nss2. nss0 is skipped here to test
    // the namespace index is remapped correctly in the generated request. The nsIndex [1, 2, 1]
    // will be remapped into [0, 1, 0]. The insert and delete op target shard1 only, and
    // the update op targets both shard1 and shard2.
    BulkWriteCommandRequest bulkRequest(
        {BulkWriteInsertOp(1, BSON("a" << 0)),
         BulkWriteUpdateOp(
             2, BSON("a" << 1), write_ops::UpdateModification(BSON("$set" << BSON("b" << 1)))),
         BulkWriteDeleteOp(1, BSON("a" << 2))},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1), NamespaceInfoEntry(nss2)});

    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNSHARDED(), nss1DbVersion);
    const ShardVersion nss2ShardVersion1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(1, 0)}, CollectionPlacement(1, 0)));
    const ShardVersion nss2ShardVersion2 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(2, 0)}, CollectionPlacement(1, 0)));
    const ShardEndpoint nss2Shard1(shardId2, nss2ShardVersion1, boost::none);
    const ShardEndpoint nss2Shard2(shardId2, nss2ShardVersion2, boost::none);
    auto batch = SimpleWriteBatch{{
        {shardId1,
         {
             {
                 {nss1, nss1Shard1},
                 {nss2, nss2Shard1},
             },
             {WriteOp(bulkRequest, 0), WriteOp(bulkRequest, 1), WriteOp(bulkRequest, 2)},
         }},
        {shardId2,
         {
             {
                 {nss2, nss2Shard2},
             },
             {WriteOp(bulkRequest, 1)},
         }},
    }};

    auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
    operationContext()->setLogicalSessionId(lsid);
    auto txnNumber = 0;
    operationContext()->setTxnNumber(txnNumber);

    auto future = launchAsync([&]() {
        WriteBatchExecutor executor;
        auto responses = executor.execute(operationContext(), batch);
        std::set<ShardId> expectedShardIds{shardId1, shardId2};
        ASSERT_EQ(2, responses.size());
        for (auto& [shardId, response] : responses) {
            ASSERT(expectedShardIds.contains(shardId));
            ASSERT(response.swResponse.getStatus().isOK());
            ASSERT_BSONOBJ_EQ(BSON("ok" << 1), response.swResponse.getValue().data);
        }
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        assertBulkWriteRequest(
            request.cmdObj,
            {
                BSON("insert" << 0 << "document" << BSON("a" << 0)),
                BSON("update" << 1 << "filter" << BSON("a" << 1) << "multi" << false << "updateMods"
                              << BSON("$set" << BSON("b" << 1)) << "upsert" << false),
                BSON("delete" << 0 << "filter" << BSON("a" << 2) << "multi" << false),
            },
            {
                getNamespaceInfoEntry(nss1, boost::none, nss1DbVersion),
                getNamespaceInfoEntry(nss2, nss2ShardVersion1, boost::none),
            },
            lsid,
            txnNumber,
            operationContext()->getWriteConcern());
        return BSON("ok" << 1);
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        assertBulkWriteRequest(
            request.cmdObj,
            {
                BSON("update" << 0 << "filter" << BSON("a" << 1) << "multi" << false << "updateMods"
                              << BSON("$set" << BSON("b" << 1)) << "upsert" << false),
            },
            {
                getNamespaceInfoEntry(nss2, nss2ShardVersion2, boost::none),
            },
            lsid,
            txnNumber,
            operationContext()->getWriteConcern());
        return BSON("ok" << 1);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
