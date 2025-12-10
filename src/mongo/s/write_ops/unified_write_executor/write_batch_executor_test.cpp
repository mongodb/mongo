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
#include "mongo/bson/json.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unified_write_executor {
namespace {

class WriteBatchExecutorTest : public ShardingTestFixture {
public:
    const ShardId shardId1 = ShardId("shard1");
    const ShardId shardId2 = ShardId("shard2");

    const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test", "coll2");

    const bool nss0IsViewfulTimeseries = false;
    const bool nss1IsViewfulTimeseries = true;
    const bool nss2IsViewfulTimeseries = false;
    const std::set<NamespaceString> nssIsViewfulTimeseries{nss1};

    const UUID uuid0 = UUID::gen();
    const UUID uuid1 = UUID::gen();
    const UUID uuid2 = UUID::gen();

    const BSONObj emptyBulkWriteCommandReplyObj =
        BSON("cursor" << BSON("firstBatch" << BSONArray() << "id" << 0LL << "ns" << "foo")
                      << "nErrors" << 0 << "nDeleted" << 0 << "nInserted" << 0 << "nMatched" << 0
                      << "nModified" << 0 << "nUpserted" << 0);

    const BSONObj emptyFindAndModifyCommandReplyObj =
        BSON("lastErrorObject" << BSON("n" << 0) << "value" << BSONNULL);

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
                                             boost::optional<DatabaseVersion> databaseVersion,
                                             boost::optional<UUID> collectionUUID,
                                             bool isViewfulTimeseries = false) {

        auto translatedNss = isViewfulTimeseries ? nss.makeTimeseriesBucketsNamespace() : nss;
        NamespaceInfoEntry entry(translatedNss);
        entry.setShardVersion(shardVersion);
        entry.setDatabaseVersion(databaseVersion);
        entry.setCollectionUUID(collectionUUID);
        if (isViewfulTimeseries) {
            entry.setIsTimeseriesNamespace(true);
        }
        return entry;
    }

    void assertBulkWriteRequest(
        BSONObj cmdObj,
        std::vector<BSONObj> expectedOps,
        std::vector<NamespaceInfoEntry> expectedNsInfos,
        boost::optional<LogicalSessionId> expectedLsid,
        boost::optional<TxnNumber> expectedTxnNumber,
        boost::optional<const WriteConcernOptions&> expectedWriteConcern,
        boost::optional<bool> expectedBypassDocumentValidation = boost::none,
        boost::optional<bool> expectedErrorsOnly = boost::none,
        boost::optional<mongo::BSONObj> expectedLet = boost::none,
        boost::optional<mongo::IDLAnyTypeOwned> expectedComment = boost::none,
        boost::optional<std::int64_t> expectedMaxTimeMS = boost::none,
        boost::optional<std::vector<StmtId>> expectedStmtIds = boost::none) {
        BSONObjBuilder builder;
        builder.appendElements(cmdObj);
        // The serialized command object does not have '$db' field, which is required for parsing.
        builder.append(BulkWriteCommandRequest::kDbNameFieldName,
                       DatabaseName::kAdmin.toString_forTest());

        // Keep our serialized command in scope.
        BSONObj cmdObjWithDb = builder.obj();
        BulkWriteCommandRequest bulkWrite =
            BulkWriteCommandRequest::parse(cmdObjWithDb, IDLParserContext("bulkWrite"));

        ASSERT_EQ(bulkWrite.getOps().size(), expectedOps.size());
        for (size_t i = 0; i < bulkWrite.getOps().size(); i++) {
            ASSERT_BSONOBJ_EQ(BulkWriteCRUDOp(bulkWrite.getOps()[i]).toBSON(), expectedOps[i]);
        }

        ASSERT_EQ(bulkWrite.getNsInfo().size(), expectedNsInfos.size());
        for (size_t i = 0; i < bulkWrite.getNsInfo().size(); i++) {
            const auto& nsInfo = bulkWrite.getNsInfo()[i];
            ASSERT_EQ(nsInfo.getNs(), expectedNsInfos[i].getNs());
            if (expectedNsInfos[i].getDatabaseVersion()) {
                ASSERT_EQ(*expectedNsInfos[i].getDatabaseVersion(), *nsInfo.getDatabaseVersion());
            }
            if (expectedNsInfos[i].getShardVersion()) {
                ASSERT_EQ(*expectedNsInfos[i].getShardVersion(), *nsInfo.getShardVersion());
            }
            if (expectedNsInfos[i].getIsTimeseriesNamespace()) {
                ASSERT_TRUE(nsInfo.getIsTimeseriesNamespace());
            }
        }

        if (expectedLsid) {
            ASSERT_BSONOBJ_EQ(expectedLsid->toBSON(), bulkWrite.getLsid()->toBSON());
        }
        if (expectedTxnNumber) {
            ASSERT_EQ(*expectedTxnNumber, *bulkWrite.getTxnNumber());
        }
        if (expectedWriteConcern) {
            ASSERT_EQ(*expectedWriteConcern, *bulkWrite.getWriteConcern());
        }
        if (expectedBypassDocumentValidation) {
            ASSERT_EQ(expectedBypassDocumentValidation,
                      cmdObj.getField("bypassDocumentValidation").boolean());
        }
        if (expectedErrorsOnly) {
            ASSERT_EQ(expectedErrorsOnly, cmdObj.getField("errorsOnly").boolean());
        }
        if (expectedLet) {
            ASSERT_BSONOBJ_EQ(*expectedLet, cmdObj.getField("let").Obj());
        }
        if (expectedComment) {
            ASSERT_EQ(expectedComment->getElement().checkAndGetStringData(),
                      cmdObj.getField("comment").checkAndGetStringData());
        }
        if (expectedMaxTimeMS) {
            ASSERT_EQ(*expectedMaxTimeMS, cmdObj.getField("maxTimeMS").number());
        }
        if (expectedStmtIds) {
            ASSERT_EQ(expectedStmtIds->size(), bulkWrite.getStmtIds()->size());
            for (size_t i = 0; i < expectedStmtIds->size(); i++) {
                ASSERT_EQ(expectedStmtIds->at(i), bulkWrite.getStmtIds()->at(i));
            }
        }
    }
};

TEST_F(WriteBatchExecutorTest, ExecuteSimpleWriteBatch) {
    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNTRACKED(), nss1DbVersion);
    const ShardVersion nss2ShardVersion1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(1, 0)}, CollectionPlacement(1, 0)));
    const ShardVersion nss2ShardVersion2 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(2, 0)}, CollectionPlacement(1, 0)));
    const ShardEndpoint nss2Shard1(shardId2, nss2ShardVersion1, boost::none);
    const ShardEndpoint nss2Shard2(shardId2, nss2ShardVersion2, boost::none);

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
        {
            getNamespaceInfoEntry(
                nss0, boost::none /* shardVersion */, boost::none /* databaseVersion */, uuid0),
            getNamespaceInfoEntry(
                nss1, boost::none /* shardVersion */, boost::none /* databaseVersion */, uuid1),
            getNamespaceInfoEntry(
                nss2, boost::none /* shardVersion */, boost::none /* databaseVersion */, uuid2),
        });
    WriteCommandRef cmdRef(bulkRequest);

    auto batch = SimpleWriteBatch{{
        {shardId1,
         {
             {
                 {nss1, nss1Shard1},
                 {nss2, nss2Shard1},
             },
             nssIsViewfulTimeseries,
             {WriteOp(bulkRequest, 0), WriteOp(bulkRequest, 1), WriteOp(bulkRequest, 2)},
         }},
        {shardId2,
         {
             {
                 {nss2, nss2Shard2},
             },
             nssIsViewfulTimeseries,
             {WriteOp(bulkRequest, 1)},
         }},
    }};

    auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
    operationContext()->setLogicalSessionId(lsid);
    auto txnNumber = 0;
    operationContext()->setTxnNumber(txnNumber);

    auto future = launchAsync([&]() {
        MockRoutingContext rtx;
        WriteBatchExecutor executor(cmdRef);
        auto resps = executor.execute(operationContext(), rtx, {batch});

        ASSERT_TRUE(holds_alternative<SimpleWriteBatchResponse>(resps));
        auto& response = get<SimpleWriteBatchResponse>(resps);

        std::set<ShardId> expectedShardIds{shardId1, shardId2};
        ASSERT_EQ(2, response.shardResponses.size());
        for (auto& [shardId, response] : response.shardResponses) {
            ASSERT(expectedShardIds.contains(shardId));
            ASSERT(response.isOK());
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
                getNamespaceInfoEntry(
                    nss1, boost::none, nss1DbVersion, uuid1, nss1IsViewfulTimeseries),
                getNamespaceInfoEntry(
                    nss2, nss2ShardVersion1, boost::none, uuid2, nss2IsViewfulTimeseries),
            },
            lsid,
            txnNumber,
            operationContext()->getWriteConcern());
        return emptyBulkWriteCommandReplyObj;
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        assertBulkWriteRequest(
            request.cmdObj,
            {
                BSON("update" << 0 << "filter" << BSON("a" << 1) << "multi" << false << "updateMods"
                              << BSON("$set" << BSON("b" << 1)) << "upsert" << false),
            },
            {
                getNamespaceInfoEntry(
                    nss2, nss2ShardVersion2, boost::none, uuid2, nss2IsViewfulTimeseries),
            },
            lsid,
            txnNumber,
            operationContext()->getWriteConcern());
        return emptyBulkWriteCommandReplyObj;
    });

    future.default_timed_get();
}

TEST_F(WriteBatchExecutorTest, ExecuteSimpleWriteBatchSpecifiedWriteOptions) {
    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNTRACKED(), nss1DbVersion);

    // We create a bulkRequest with an insert op that runs against
    // the same namespace nss1 and targets shard1.
    BulkWriteCommandRequest bulkRequest({BulkWriteInsertOp(1, BSON("a" << 0))},
                                        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    // Assert that bulkRequest's write options are set to default values.
    ASSERT_EQ(false, bulkRequest.getBypassDocumentValidation());
    ASSERT_EQ(false, bulkRequest.getErrorsOnly());
    ASSERT_EQ(boost::none, bulkRequest.getLet());
    ASSERT_EQ(boost::none, bulkRequest.getComment());
    ASSERT_EQ(boost::none, bulkRequest.getMaxTimeMS());

    auto batch = SimpleWriteBatch{{{shardId1,
                                    {
                                        {{nss1, nss1Shard1}},
                                        nssIsViewfulTimeseries,
                                        {WriteOp(bulkRequest, 0)},
                                    }}}};
    auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
    operationContext()->setLogicalSessionId(lsid);
    auto txnNumber = 0;
    operationContext()->setTxnNumber(txnNumber);

    // Create a WriteCommandRef with write options that WriteBatchExecutor should attach to the
    // request.
    bulkRequest.setBypassDocumentValidation(true);
    bulkRequest.setLet(BSON("key" << "value"));
    bulkRequest.setErrorsOnly(true);
    bulkRequest.setComment(IDLAnyTypeOwned(BSON("key" << "value")["key"]));
    bulkRequest.setMaxTimeMS(25);
    WriteCommandRef cmdRef(bulkRequest);

    auto future = launchAsync([&]() {
        MockRoutingContext rtx;
        WriteBatchExecutor executor(cmdRef);
        auto resps = executor.execute(operationContext(), rtx, {batch});

        ASSERT_TRUE(holds_alternative<SimpleWriteBatchResponse>(resps));

        auto& response = get<SimpleWriteBatchResponse>(resps);

        std::set<ShardId> expectedShardIds{shardId1};
        ASSERT_EQ(1, response.shardResponses.size());
        for (auto& [shardId, response] : response.shardResponses) {
            ASSERT(expectedShardIds.contains(shardId));
            ASSERT(response.isOK());
        }
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        assertBulkWriteRequest(request.cmdObj,
                               {
                                   BSON("insert" << 0 << "document" << BSON("a" << 0)),
                               },
                               {
                                   getNamespaceInfoEntry(nss1,
                                                         boost::none /* shardVersion */,
                                                         nss1DbVersion,
                                                         boost::none /* collectionUUID */,
                                                         nss1IsViewfulTimeseries),
                               },
                               lsid,
                               txnNumber,
                               operationContext()->getWriteConcern(),
                               bulkRequest.getBypassDocumentValidation(),
                               bulkRequest.getErrorsOnly(),
                               bulkRequest.getLet(),
                               bulkRequest.getComment(),
                               bulkRequest.getMaxTimeMS());
        return emptyBulkWriteCommandReplyObj;
    });

    future.default_timed_get();
}

TEST_F(WriteBatchExecutorTest, ExecuteSimpleWriteBatchBulkOpOptions) {
    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNTRACKED(), nss1DbVersion);

    // We create a bulkRequest with an update op specifying hint, sort, and arrayFilters
    // options. It runs against the same namespace nss1 and targets shard1.
    auto arrayFiltersBSON = BSON("x.a" << BSON("$gt" << 2));
    auto sortBSON = BSON("a" << 1);
    auto hintBSON = BSON("_id" << 1);
    BulkWriteUpdateOp updateOp = BulkWriteUpdateOp(
        1, BSON("a" << 1), write_ops::UpdateModification(BSON("$set" << BSON("b" << 1))));
    updateOp.setArrayFilters(std::vector<BSONObj>{arrayFiltersBSON});
    updateOp.setSort(sortBSON);
    updateOp.setHint(hintBSON);
    BulkWriteCommandRequest bulkRequest({updateOp},
                                        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteCommandRef cmdRef(bulkRequest);

    auto batch = SimpleWriteBatch{{{shardId1,
                                    {
                                        {{nss1, nss1Shard1}},
                                        nssIsViewfulTimeseries,
                                        {WriteOp(bulkRequest, 0)},
                                    }}}};
    auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
    operationContext()->setLogicalSessionId(lsid);
    auto txnNumber = 0;
    operationContext()->setTxnNumber(txnNumber);

    auto future = launchAsync([&]() {
        MockRoutingContext rtx;
        WriteBatchExecutor executor(cmdRef);
        auto resps = executor.execute(operationContext(), rtx, {batch});

        ASSERT_TRUE(holds_alternative<SimpleWriteBatchResponse>(resps));

        auto& response = get<SimpleWriteBatchResponse>(resps);

        std::set<ShardId> expectedShardIds{shardId1};
        ASSERT_EQ(1, response.shardResponses.size());
        for (auto& [shardId, response] : response.shardResponses) {
            ASSERT(expectedShardIds.contains(shardId));
            ASSERT(response.isOK());
        }
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        assertBulkWriteRequest(
            request.cmdObj,
            {
                BSON("update" << 0 << "filter" << BSON("a" << 1) << "sort" << sortBSON << "multi"
                              << false << "updateMods" << BSON("$set" << BSON("b" << 1)) << "upsert"
                              << false << "arrayFilters" << BSON_ARRAY(arrayFiltersBSON) << "hint"
                              << hintBSON),
            },
            {
                getNamespaceInfoEntry(nss1,
                                      boost::none /* shardVersion */,
                                      nss1DbVersion,
                                      boost::none /* collectionUUID */,
                                      nss1IsViewfulTimeseries),
            },
            lsid,
            txnNumber,
            operationContext()->getWriteConcern());
        return emptyBulkWriteCommandReplyObj;
    });

    future.default_timed_get();
}

TEST_F(WriteBatchExecutorTest, ExecuteSimpleWriteBatchSetsStmtIds) {
    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNTRACKED(), nss1DbVersion);

    // We create a bulkRequest with two insert ops that have stmtIds.
    BulkWriteInsertOp insertOp = BulkWriteInsertOp(1, BSON("a" << 0));
    BulkWriteDeleteOp deleteOp = BulkWriteDeleteOp(1, BSON("a" << 2));
    BulkWriteCommandRequest bulkRequest({insertOp, deleteOp},
                                        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    std::vector<StmtId> stmtIds{0, 1};
    bulkRequest.setStmtIds(std::move(stmtIds));
    WriteCommandRef cmdRef(bulkRequest);

    auto batch = SimpleWriteBatch{{{shardId1,
                                    {
                                        {{nss1, nss1Shard1}},
                                        nssIsViewfulTimeseries,
                                        {WriteOp(bulkRequest, 0), WriteOp(bulkRequest, 1)},
                                    }}}};
    auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
    operationContext()->setLogicalSessionId(lsid);
    auto txnNumber = 0;
    operationContext()->setTxnNumber(txnNumber);

    auto future = launchAsync([&]() {
        WriteBatchExecutor executor(cmdRef);
        MockRoutingContext rtx;
        auto resps = executor.execute(operationContext(), rtx, {batch});

        ASSERT_TRUE(holds_alternative<SimpleWriteBatchResponse>(resps));

        auto& response = get<SimpleWriteBatchResponse>(resps);

        std::set<ShardId> expectedShardIds{shardId1};
        ASSERT_EQ(1, response.shardResponses.size());
        for (auto& [shardId, response] : response.shardResponses) {
            ASSERT(expectedShardIds.contains(shardId));
            ASSERT(response.isOK());
        }
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        assertBulkWriteRequest(
            request.cmdObj,
            {
                BSON("insert" << 0 << "document" << BSON("a" << 0)),
                BSON("delete" << 0 << "filter" << BSON("a" << 2) << "multi" << false),
            },
            {
                getNamespaceInfoEntry(nss1,
                                      boost::none /* shardVersion */,
                                      nss1DbVersion,
                                      boost::none /* collectionUUID */,
                                      nss1IsViewfulTimeseries),
            },
            lsid,
            txnNumber,
            operationContext()->getWriteConcern(),
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            boost::none,
            std::vector<StmtId>{0, 1});
        return emptyBulkWriteCommandReplyObj;
    });

    future.default_timed_get();
}

TEST_F(WriteBatchExecutorTest, ExecuteSimpleWriteBatchWithFindAndModifyRequest) {
    const DatabaseVersion nss1DbVersion(UUID::gen(), Timestamp(1, 0));
    const ShardEndpoint nss1Shard1(shardId1, ShardVersion::UNTRACKED(), nss1DbVersion);

    auto query = BSON("a" << 1);
    auto sort = BSON("s" << 1);
    auto remove = false;
    auto update = BSON("$set" << BSON("b" << 1));
    auto newParam = true;
    auto fields = BSON("f" << 1);
    auto upsert = true;
    auto bypassDocumentValidation = false;
    auto maxTimeMS = 1000;
    auto collation = BSON("locale" << "en_US" << "numericOrdering" << true);
    auto arrayFilters = BSON("x.a" << BSON("$gt" << 2));
    auto hint = BSON("h" << 1);
    auto comment = "comment";
    auto let = BSON("l" << 1);
    LegacyRuntimeConstants legacyRuntimeConstants(Date_t(), Timestamp(1764967761, 10));
    auto stmtId = 0;
    write_ops::FindAndModifyCommandRequest findAndModifyRequest(nss1);
    findAndModifyRequest.setQuery(query);
    findAndModifyRequest.setSort(sort);
    findAndModifyRequest.setRemove(remove);
    findAndModifyRequest.setUpdate(write_ops::UpdateModification(update));
    findAndModifyRequest.setNew(newParam);
    findAndModifyRequest.setFields(fields);
    findAndModifyRequest.setUpsert(upsert);
    findAndModifyRequest.setBypassDocumentValidation(bypassDocumentValidation);
    findAndModifyRequest.setMaxTimeMS(maxTimeMS);
    findAndModifyRequest.setCollation(collation);
    findAndModifyRequest.setArrayFilters(std::vector<BSONObj>{arrayFilters});
    findAndModifyRequest.setHint(hint);
    findAndModifyRequest.setComment(IDLAnyTypeOwned(BSON("key" << comment)["key"]));
    findAndModifyRequest.setLet(let);
    findAndModifyRequest.setLegacyRuntimeConstants(legacyRuntimeConstants);
    WriteCommandRef cmdRef(findAndModifyRequest);

    auto batch = SimpleWriteBatch{{{shardId1,
                                    {
                                        {{nss1, nss1Shard1}},
                                        nssIsViewfulTimeseries,
                                        {WriteOp(findAndModifyRequest)},
                                    }}}};
    auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
    operationContext()->setLogicalSessionId(lsid);
    auto txnNumber = 0;
    operationContext()->setTxnNumber(txnNumber);

    auto future = launchAsync([&]() {
        MockRoutingContext rtx;
        WriteBatchExecutor executor(cmdRef);
        auto resps = executor.execute(operationContext(), rtx, {batch});

        ASSERT_TRUE(holds_alternative<SimpleWriteBatchResponse>(resps));

        auto& response = get<SimpleWriteBatchResponse>(resps);

        std::set<ShardId> expectedShardIds{shardId1};
        ASSERT_EQ(1, response.shardResponses.size());
        for (auto& [shardId, response] : response.shardResponses) {
            ASSERT(expectedShardIds.contains(shardId));
            ASSERT(response.isOK());
        }
    });

    onCommandForPoolExecutor([&](const executor::RemoteCommandRequest& request) {
        BSONObjBuilder builder;
        nss1Shard1.shardVersion->serialize("", &builder);
        auto shardVersionBson = builder.obj().firstElement().Obj().getOwned();

        auto expectedCmdObj = BSON(
            "findAndModify" << nss1.makeTimeseriesBucketsNamespace().coll() << "query" << query
                            << "fields" << fields << "sort" << sort << "hint" << hint << "collation"
                            << collation << "arrayFilters" << BSON_ARRAY(arrayFilters) << "remove"
                            << remove << "update" << update << "lsid" << lsid.toBSON() << "upsert"
                            << upsert << "new" << newParam << "stmtId" << stmtId
                            << "bypassDocumentValidation" << bypassDocumentValidation << "let"
                            << let << "runtimeConstants" << legacyRuntimeConstants.toBSON()
                            << "maxTimeMS" << maxTimeMS << "comment" << comment << "txnNumber"
                            << txnNumber << "databaseVersion" << nss1DbVersion.toBSON()
                            << "shardVersion" << shardVersionBson << "readConcern" << BSONObj()
                            << "writeConcern" << operationContext()->getWriteConcern().toBSON()
                            << "isTimeseriesNamespace" << true);
        ASSERT_BSONOBJ_EQ_UNORDERED(expectedCmdObj, request.cmdObj);

        return emptyFindAndModifyCommandReplyObj;
    });

    future.default_timed_get();
}
}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
