/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");
const std::string kShardKey = "X";
const BSONObj kShardKeyPattern{BSON(kShardKey << 1)};
const ConnectionString kDonorConnStr =
    ConnectionString::forReplicaSet("Donor",
                                    {HostAndPort("DonorHost1:1234"),
                                     HostAndPort{"DonorHost2:1234"},
                                     HostAndPort{"DonorHost3:1234"}});
const ConnectionString kRecipientConnStr =
    ConnectionString::forReplicaSet("Recipient",
                                    {HostAndPort("RecipientHost1:1234"),
                                     HostAndPort("RecipientHost2:1234"),
                                     HostAndPort("RecipientHost3:1234")});

class MigrationChunkClonerSourceLegacyTest : public ShardServerTestFixture {
protected:
    MigrationChunkClonerSourceLegacyTest() : ShardServerTestFixture(Options{}.useMockClock(true)) {}

    void setUp() override {
        ShardServerTestFixture::setUp();

        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        // TODO: SERVER-26919 set the flag on the mock repl coordinator just for the window where it
        // actually needs to bypass the op observer.
        replicationCoordinator()->alwaysAllowWrites(true);

        _client.emplace(operationContext());

        {
            auto donorShard = assertGet(
                shardRegistry()->getShard(operationContext(), kDonorConnStr.getSetName()));
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setConnectionStringReturnValue(kDonorConnStr);
            RemoteCommandTargeterMock::get(donorShard->getTargeter())
                ->setFindHostReturnValue(kDonorConnStr.getServers()[0]);
        }

        {
            auto recipientShard = assertGet(
                shardRegistry()->getShard(operationContext(), kRecipientConnStr.getSetName()));
            RemoteCommandTargeterMock::get(recipientShard->getTargeter())
                ->setConnectionStringReturnValue(kRecipientConnStr);
            RemoteCommandTargeterMock::get(recipientShard->getTargeter())
                ->setFindHostReturnValue(kRecipientConnStr.getServers()[0]);
        }

        _lsid = makeLogicalSessionId(operationContext());
    }

    void tearDown() override {
        _client.reset();

        ShardServerTestFixture::tearDown();
    }

    /**
     * Returns the DBDirectClient instance to use for writes to the database.
     */
    DBDirectClient* client() {
        invariant(_client);
        return _client.get_ptr();
    }

    /**
     * Inserts the specified docs in 'kNss' and ensures the insert succeeded.
     */
    void insertDocsInShardedCollection(const std::vector<BSONObj>& docs) {
        if (docs.empty())
            return;

        auto response = client()->insertAcknowledged(kNss.ns(), docs);
        ASSERT_OK(getStatusFromWriteCommandReply(response));
        ASSERT_GT(response["n"].Int(), 0);
    }

    void deleteDocsInShardedCollection(BSONObj query) {
        auto response = client()->removeAcknowledged(kNss.ns(), query);
        ASSERT_OK(getStatusFromWriteCommandReply(response));
        ASSERT_GT(response["n"].Int(), 0);
    }

    void updateDocsInShardedCollection(BSONObj filter, BSONObj updated) {
        auto response = client()->updateAcknowledged(kNss.ns(), filter, updated);
        ASSERT_OK(getStatusFromWriteCommandReply(response));
        ASSERT_GT(response["n"].Int(), 0);
    }

    /**
     * Creates a collection, which contains an index corresponding to kShardKeyPattern and inserts
     * the specified initial documents.
     */
    void createShardedCollection(const std::vector<BSONObj>& initialDocs) {
        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(operationContext());
            uassertStatusOK(createCollection(
                operationContext(), kNss.db().toString(), BSON("create" << kNss.coll())));
        }

        const auto uuid = [&] {
            AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
            return autoColl.getCollection()->uuid();
        }();

        [&] {
            const OID epoch = OID::gen();
            const Timestamp timestamp(1);

            auto rt = RoutingTableHistory::makeNew(
                kNss,
                uuid,
                kShardKeyPattern,
                nullptr,
                false,
                epoch,
                timestamp,
                boost::none /* timeseriesFields */,
                boost::none /* resharding Fields */,
                boost::none /* chunkSizeBytes */,
                true,
                {ChunkType{uuid,
                           ChunkRange{BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)},
                           ChunkVersion({epoch, timestamp}, {1, 0}),
                           ShardId("dummyShardId")}});

            AutoGetDb autoDb(operationContext(), kNss.dbName(), MODE_IX);
            Lock::CollectionLock collLock(operationContext(), kNss, MODE_IX);
            CollectionShardingRuntime::get(operationContext(), kNss)
                ->setFilteringMetadata(
                    operationContext(),
                    CollectionMetadata(
                        ChunkManager(ShardId("dummyShardId"),
                                     DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                                     makeStandaloneRoutingTableHistory(std::move(rt)),
                                     boost::none),
                        ShardId("dummyShardId")));
        }();

        client()->createIndex(kNss.ns(), kShardKeyPattern);
        insertDocsInShardedCollection(initialDocs);
    }

    /**
     * Shortcut to create BSON represenation of a moveChunk request for the specified range with
     * fixed kDonorConnStr and kRecipientConnStr, respectively.
     */
    static ShardsvrMoveRange createMoveRangeRequest(const ChunkRange& chunkRange) {
        ShardsvrMoveRange req(kNss);
        req.setEpoch(OID::gen());
        req.setFromShard(ShardId(kDonorConnStr.getSetName()));
        req.setMaxChunkSizeBytes(1024);
        req.getMoveRangeRequestBase().setToShard(ShardId(kRecipientConnStr.getSetName()));
        req.getMoveRangeRequestBase().setMin(chunkRange.getMin());
        req.getMoveRangeRequestBase().setMax(chunkRange.getMax());
        return req;
    }

    /**
     * Instantiates a BSON object in which both "_id" and "X" are set to value.
     */
    static BSONObj createCollectionDocument(int value) {
        return BSON("_id" << value << "X" << value);
    }

    /**
     * Instantiates a BSON object with different "_id" and "X" values.
     */
    static BSONObj createCollectionDocumentForUpdate(int id, int value) {
        return BSON("_id" << id << "X" << value);
    }

    /**
     * Instantiates a BSON object with objsize close to size.
     */
    static BSONObj createSizedCollectionDocument(int id, long long size) {
        std::string value(size, 'x');
        return BSON("_id" << id << "X" << id << "Y" << value);
    }

protected:
    LogicalSessionId _lsid;
    TxnNumber _txnNumber{0};

private:
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() = default;

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {

                ShardType donorShard;
                donorShard.setName(kDonorConnStr.getSetName());
                donorShard.setHost(kDonorConnStr.toString());

                ShardType recipientShard;
                recipientShard.setName(kRecipientConnStr.getSetName());
                recipientShard.setHost(kRecipientConnStr.toString());

                return repl::OpTimeWith<std::vector<ShardType>>({donorShard, recipientShard});
            }
        };

        return std::make_unique<StaticCatalogClient>();
    }

    boost::optional<DBDirectClient> _client;
};

TEST_F(MigrationChunkClonerSourceLegacyTest, CorrectDocumentsFetched) {
    const std::vector<BSONObj> contents = {createCollectionDocument(99),
                                           createCollectionDocument(100),
                                           createCollectionDocument(199),
                                           createCollectionDocument(200)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSourceLegacy cloner(req,
                                            WriteConcernOptions(),
                                            kShardKeyPattern,
                                            kDonorConnStr,
                                            kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    // Ensure the initial clone documents are available
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(2, arrBuilder.arrSize());

            const auto arr = arrBuilder.arr();
            ASSERT_BSONOBJ_EQ(contents[1], arr[0].Obj());
            ASSERT_BSONOBJ_EQ(contents[2], arr[1].Obj());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }
    }

    // Insert some documents in the chunk range to be included for migration
    insertDocsInShardedCollection({createCollectionDocument(150)});
    insertDocsInShardedCollection({createCollectionDocument(151)});

    // Insert some documents which are outside of the chunk range and should not be included for
    // migration
    insertDocsInShardedCollection({createCollectionDocument(90)});
    insertDocsInShardedCollection({createCollectionDocument(210)});

    // Normally the insert above and the onInsert/onDelete callbacks below will happen under the
    // same lock and write unit of work
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        WriteUnitOfWork wuow(operationContext());

        cloner.onInsertOp(operationContext(), createCollectionDocument(90), {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(150), {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(151), {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(210), {});

        cloner.onDeleteOp(operationContext(), createCollectionDocument(80), {}, {});
        cloner.onDeleteOp(operationContext(), createCollectionDocument(199), {}, {});
        cloner.onDeleteOp(operationContext(), createCollectionDocument(220), {}, {});

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }

        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(2U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(150), modsObj["reload"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(createCollectionDocument(151), modsObj["reload"].Array()[1].Obj());

            // The legacy chunk cloner cannot filter out deletes because we don't preserve the shard
            // key on delete
            ASSERT_EQ(3U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 80), modsObj["deleted"].Array()[0].Obj());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 199), modsObj["deleted"].Array()[1].Obj());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 220), modsObj["deleted"].Array()[2].Obj());
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}


TEST_F(MigrationChunkClonerSourceLegacyTest, RemoveDuplicateDocuments) {
    const std::vector<BSONObj> contents = {createCollectionDocument(100),
                                           createCollectionDocument(199)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSourceLegacy cloner(req,
                                            WriteConcernOptions(),
                                            kShardKeyPattern,
                                            kDonorConnStr,
                                            kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    // Ensure the initial clone documents are available
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(2, arrBuilder.arrSize());

            const auto arr = arrBuilder.arr();
            ASSERT_BSONOBJ_EQ(contents[0], arr[0].Obj());
            ASSERT_BSONOBJ_EQ(contents[1], arr[1].Obj());
        }
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        deleteDocsInShardedCollection(createCollectionDocument(100));
        insertDocsInShardedCollection({createCollectionDocument(100)});
        deleteDocsInShardedCollection(createCollectionDocument(100));

        updateDocsInShardedCollection(createCollectionDocument(199),
                                      createCollectionDocumentForUpdate(199, 198));
        updateDocsInShardedCollection(createCollectionDocumentForUpdate(199, 198),
                                      createCollectionDocumentForUpdate(199, 197));

        WriteUnitOfWork wuow(operationContext());

        cloner.onDeleteOp(operationContext(), createCollectionDocument(100), {}, {});
        cloner.onInsertOp(operationContext(), createCollectionDocument(100), {});
        cloner.onDeleteOp(operationContext(), createCollectionDocument(100), {}, {});

        cloner.onUpdateOp(operationContext(),
                          createCollectionDocument(199),
                          createCollectionDocumentForUpdate(199, 198),
                          {},
                          {});
        cloner.onUpdateOp(operationContext(),
                          createCollectionDocument(199),
                          createCollectionDocumentForUpdate(199, 197),
                          {},
                          {});

        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(1U, modsObj["reload"].Array().size());
            ASSERT_BSONOBJ_EQ(createCollectionDocumentForUpdate(199, 197),
                              modsObj["reload"].Array()[0].Obj());
            ASSERT_EQ(1U, modsObj["deleted"].Array().size());
            ASSERT_BSONOBJ_EQ(BSON("_id" << 100), modsObj["deleted"].Array()[0].Obj());
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}


TEST_F(MigrationChunkClonerSourceLegacyTest, OneLargeDocumentTransferMods) {
    const std::vector<BSONObj> contents = {createCollectionDocument(1)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 1), BSON("X" << 100)));
    MigrationChunkClonerSourceLegacy cloner(req,
                                            WriteConcernOptions(),
                                            kShardKeyPattern,
                                            kDonorConnStr,
                                            kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(1, arrBuilder.arrSize());
        }
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);
        BSONObj insertDoc =
            createSizedCollectionDocument(2, BSONObjMaxUserSize - kFixedCommandOverhead + 2 * 1024);
        insertDocsInShardedCollection({insertDoc});
        WriteUnitOfWork wuow(operationContext());
        cloner.onInsertOp(operationContext(), insertDoc, {});
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));

            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(1, modsObj["reload"].Array().size());
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}

TEST_F(MigrationChunkClonerSourceLegacyTest, ManySmallDocumentsTransferMods) {
    const std::vector<BSONObj> contents = {createCollectionDocument(1)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 1), BSON("X" << 1000000)));
    MigrationChunkClonerSourceLegacy cloner(req,
                                            WriteConcernOptions(),
                                            kShardKeyPattern,
                                            kDonorConnStr,
                                            kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
        });

        ASSERT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
        futureStartClone.default_timed_get();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(1, arrBuilder.arrSize());
        }
    }

    long long numDocuments = 0;
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IX);

        std::vector<BSONObj> insertDocs;
        long long totalSize = 0;
        long long id = 2;
        while (true) {
            BSONObj add = createSizedCollectionDocument(id++, 4 * 1024);
            // The overhead for a BSONObjBuilder with 4KB documents is ~ 22 * 1024, so this is the
            // max documents to fit in one batch
            if (totalSize + add.objsize() > BSONObjMaxUserSize - kFixedCommandOverhead - 22 * 1024)
                break;
            insertDocs.push_back(add);
            totalSize += add.objsize();
            numDocuments++;
            insertDocsInShardedCollection({add});
        }

        WriteUnitOfWork wuow(operationContext());
        for (BSONObj add : insertDocs) {
            cloner.onInsertOp(operationContext(), add, {});
        }
        wuow.commit();
    }

    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);
        {
            BSONObjBuilder modsBuilder;
            ASSERT_OK(cloner.nextModsBatch(operationContext(), &modsBuilder));
            const auto modsObj = modsBuilder.obj();
            ASSERT_EQ(modsObj["reload"].Array().size(), numDocuments);
        }
    }

    auto futureCommit = launchAsync([&]() {
        onCommand([&](const RemoteCommandRequest& request) { return BSON("ok" << true); });
    });

    ASSERT_OK(cloner.commitClone(operationContext()));
    futureCommit.default_timed_get();
}

TEST_F(MigrationChunkClonerSourceLegacyTest, CollectionNotFound) {
    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSourceLegacy cloner(req,
                                            WriteConcernOptions(),
                                            kShardKeyPattern,
                                            kDonorConnStr,
                                            kRecipientConnStr.getServers()[0]);

    ASSERT_NOT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceLegacyTest, ShardKeyIndexNotFound) {
    {
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            operationContext());
        uassertStatusOK(createCollection(
            operationContext(), kNss.db().toString(), BSON("create" << kNss.coll())));
    }

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSourceLegacy cloner(req,
                                            WriteConcernOptions(),
                                            kShardKeyPattern,
                                            kDonorConnStr,
                                            kRecipientConnStr.getServers()[0]);

    ASSERT_NOT_OK(cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber));
    cloner.cancelClone(operationContext());
}

TEST_F(MigrationChunkClonerSourceLegacyTest, FailedToEngageRecipientShard) {
    const std::vector<BSONObj> contents = {createCollectionDocument(99),
                                           createCollectionDocument(100),
                                           createCollectionDocument(199),
                                           createCollectionDocument(200)};

    createShardedCollection(contents);

    const ShardsvrMoveRange req =
        createMoveRangeRequest(ChunkRange(BSON("X" << 100), BSON("X" << 200)));
    MigrationChunkClonerSourceLegacy cloner(req,
                                            WriteConcernOptions(),
                                            kShardKeyPattern,
                                            kDonorConnStr,
                                            kRecipientConnStr.getServers()[0]);

    {
        auto futureStartClone = launchAsync([&]() {
            onCommand([&](const RemoteCommandRequest& request) {
                return Status(ErrorCodes::NetworkTimeout,
                              "Did not receive confirmation from donor");
            });
        });

        auto startCloneStatus =
            cloner.startClone(operationContext(), UUID::gen(), _lsid, _txnNumber);
        ASSERT_EQ(ErrorCodes::NetworkTimeout, startCloneStatus.code());
        futureStartClone.default_timed_get();
    }

    // Ensure that if the recipient tries to fetch some documents, the cloner won't crash
    {
        AutoGetCollection autoColl(operationContext(), kNss, MODE_IS);

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(2, arrBuilder.arrSize());
        }

        {
            BSONArrayBuilder arrBuilder;
            ASSERT_OK(
                cloner.nextCloneBatch(operationContext(), autoColl.getCollection(), &arrBuilder));
            ASSERT_EQ(0, arrBuilder.arrSize());
        }
    }

    // Cancel clone should not send a cancellation request to the donor because we failed to engage
    // it (see comment in the startClone method)
    cloner.cancelClone(operationContext());
}

}  // namespace
}  // namespace mongo
