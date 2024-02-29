/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <fmt/format.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/s/sharding_write_router.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/shard_cannot_refresh_due_to_locks_held_exception.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

template <typename Callable>
void runInTransaction(OperationContext* opCtx, Callable&& func) {
    auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 0;

    opCtx->setLogicalSessionId(sessionId);
    opCtx->setTxnNumber(txnNum);
    opCtx->setInMultiDocumentTransaction();

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    ASSERT(txnParticipant);
    txnParticipant.beginOrContinue(opCtx,
                                   {txnNum},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
    txnParticipant.unstashTransactionResources(opCtx, "SetDestinedRecipient");

    func();

    txnParticipant.commitUnpreparedTransaction(opCtx);
    txnParticipant.stashTransactionResources(opCtx);
}

class DestinedRecipientTest : public ShardServerTestFixtureWithCatalogCacheLoaderMock {
public:
    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.foo");
    const std::string kShardKey = "x";
    const std::vector<ShardType> kShardList = {ShardType(kMyShardName.toString(), "Host0:12345"),
                                               ShardType("shard1", "Host1:12345")};

    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheLoaderMock::setUp();

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        for (const auto& shard : kShardList) {
            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            HostAndPort host(shard.getHost());
            targeter->setConnectionStringReturnValue(ConnectionString(host));
            targeter->setFindHostReturnValue(host);
            targeterFactory()->addTargeterToReturn(ConnectionString(host), std::move(targeter));
        }
    }

    void tearDown() override {
        WaitForMajorityService::get(getServiceContext()).shutDown();

        ShardServerTestFixtureWithCatalogCacheLoaderMock::tearDown();
    }

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

        StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
            OperationContext* opCtx,
            repl::ReadConcernLevel readConcern,
            bool excludeDraining) override {
            return repl::OpTimeWith<std::vector<ShardType>>(_shards);
        }
        std::vector<CollectionType> getShardedCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          repl::ReadConcernLevel readConcernLevel,
                                                          const BSONObj& sort) override {
            return {};
        }

        std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   repl::ReadConcernLevel readConcernLevel,
                                                   const BSONObj& sort) override {
            return _colls;
        }

        void setCollections(std::vector<CollectionType> colls) {
            _colls = std::move(colls);
        }

        std::pair<CollectionType, std::vector<IndexCatalogType>>
        getCollectionAndShardingIndexCatalogEntries(
            OperationContext* opCtx,
            const NamespaceString& nss,
            const repl::ReadConcernArgs& readConcern) override {
            return std::make_pair(CollectionType(), std::vector<IndexCatalogType>());
        }

    private:
        const std::vector<ShardType> _shards;
        std::vector<CollectionType> _colls;
    };

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<StaticCatalogClient>(kShardList);
    }

protected:
    std::vector<ChunkType> createChunks(const OID& epoch,
                                        const UUID& uuid,
                                        const Timestamp& timestamp,
                                        const std::string& shardKey) {
        auto range1 = ChunkRange(BSON(shardKey << MINKEY), BSON(shardKey << 5));
        ChunkType chunk1(
            uuid, range1, ChunkVersion({epoch, timestamp}, {1, 0}), kShardList[0].getName());

        auto range2 = ChunkRange(BSON(shardKey << 5), BSON(shardKey << MAXKEY));
        ChunkType chunk2(
            uuid, range2, ChunkVersion({epoch, timestamp}, {1, 0}), kShardList[1].getName());

        return {chunk1, chunk2};
    }

    struct ReshardingEnv {
        ReshardingEnv(UUID uuid) : sourceUuid(std::move(uuid)) {}

        NamespaceString tempNss;
        UUID sourceUuid;
        ShardId destShard;
        ShardVersion version;
        DatabaseVersion dbVersion{UUID::gen(), Timestamp(1, 1)};
    };

    ReshardingEnv setupReshardingEnv(OperationContext* opCtx, bool refreshTempNss) {
        DBDirectClient client(opCtx);
        ASSERT(client.createCollection(NamespaceString::kSessionTransactionsTableNamespace));
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);
        Status status =
            createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll()));
        if (status != ErrorCodes::NamespaceExists) {
            uassertStatusOK(status);
        }

        ReshardingEnv env(CollectionCatalog::get(opCtx)->lookupUUIDByNSS(opCtx, kNss).value());
        env.destShard = kShardList[1].getName();
        CollectionGeneration gen(OID::gen(), Timestamp(1, 1));
        env.version = ShardVersionFactory::make(ChunkVersion(gen, {1, 0}),
                                                boost::optional<CollectionIndexes>(boost::none));
        env.tempNss = NamespaceString::createNamespaceString_forTest(
            kNss.db_forTest(),
            fmt::format("{}{}",
                        NamespaceString::kTemporaryReshardingCollectionPrefix,
                        env.sourceUuid.toString()));

        uassertStatusOK(createCollection(
            operationContext(), env.tempNss.dbName(), BSON("create" << env.tempNss.coll())));

        TypeCollectionReshardingFields reshardingFields;
        reshardingFields.setReshardingUUID(UUID::gen());
        reshardingFields.setDonorFields(TypeCollectionDonorFields{
            env.tempNss,
            BSON("y" << 1),
            {ShardId{kShardList[0].getName()}, ShardId{kShardList[1].getName()}}});
        reshardingFields.setState(CoordinatorStateEnum::kPreparingToDonate);

        CollectionType coll(kNss,
                            env.version.placementVersion().epoch(),
                            env.version.placementVersion().getTimestamp(),
                            Date_t::now(),
                            env.sourceUuid,
                            BSON(kShardKey << 1));
        coll.setAllowMigrations(false);

        getCatalogCacheLoaderMock()->setDatabaseRefreshReturnValue(
            DatabaseType(kNss.dbName(), kShardList[0].getName(), env.dbVersion));
        getCatalogCacheLoaderMock()->setCollectionRefreshValues(
            kNss,
            coll,
            createChunks(env.version.placementVersion().epoch(),
                         env.sourceUuid,
                         env.version.placementVersion().getTimestamp(),
                         kShardKey),
            reshardingFields);
        getCatalogCacheLoaderMock()->setCollectionRefreshValues(
            env.tempNss,
            coll,
            createChunks(env.version.placementVersion().epoch(),
                         env.sourceUuid,
                         env.version.placementVersion().getTimestamp(),
                         "y"),
            boost::none);

        ASSERT_OK(onDbVersionMismatchNoExcept(opCtx, kNss.dbName(), boost::none));
        forceShardFilteringMetadataRefresh(opCtx, kNss);

        if (refreshTempNss)
            forceShardFilteringMetadataRefresh(opCtx, env.tempNss);

        return env;
    }

    void writeDoc(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const BSONObj& doc,
                  const ReshardingEnv& env) {
        AutoGetCollection coll(opCtx, nss, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection_internal::insertDocument(opCtx, *coll, InsertStatement(doc), nullptr));
        wuow.commit();
    }

    void updateDoc(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const BSONObj& filter,
                   const BSONObj& update,
                   const ReshardingEnv& env) {
        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        Helpers::update(opCtx, coll, filter, update);
    }

    void deleteDoc(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const BSONObj& query,
                   const ReshardingEnv& env) {
        AutoGetCollection coll(opCtx, nss, MODE_IX);

        RecordId rid = Helpers::findOne(opCtx, coll.getCollection(), query);
        ASSERT(!rid.isNull());

        WriteUnitOfWork wuow(opCtx);
        OpDebug opDebug;
        collection_internal::deleteDocument(opCtx, *coll, kUninitializedStmtId, rid, &opDebug);
        wuow.commit();
    }

    repl::OplogEntry getLastOplogEntry(OperationContext* opCtx) {
        repl::OplogInterfaceLocal oplogInterface(opCtx);
        auto oplogIter = oplogInterface.makeIterator();

        const auto& doc = unittest::assertGet(oplogIter->next()).first;
        return unittest::assertGet(repl::OplogEntry::parse(doc));
    }
};

TEST_F(DestinedRecipientTest, TestGetDestinedRecipient) {
    auto opCtx = operationContext();
    auto env = setupReshardingEnv(opCtx, true);

    AutoGetCollection coll(opCtx, kNss, MODE_IX);
    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    ShardingWriteRouter shardingWriteRouter(opCtx, kNss);

    auto destShardId =
        shardingWriteRouter.getReshardingDestinedRecipient(BSON("x" << 2 << "y" << 10));
    ASSERT(destShardId);
    ASSERT_EQ(*destShardId, env.destShard);
}

TEST_F(DestinedRecipientTest, TestGetDestinedRecipientThrowsOnBlockedRefresh) {
    auto opCtx = operationContext();
    auto env = setupReshardingEnv(opCtx, false);

    AutoGetCollection coll(opCtx, kNss, MODE_IX);
    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);

    FailPointEnableBlock failPoint("blockCollectionCacheLookup");
    ASSERT_THROWS_WITH_CHECK(ShardingWriteRouter(opCtx, kNss),
                             ShardCannotRefreshDueToLocksHeldException,
                             [&](const ShardCannotRefreshDueToLocksHeldException& ex) {
                                 const auto refreshInfo =
                                     ex.extraInfo<ShardCannotRefreshDueToLocksHeldInfo>();
                                 ASSERT(refreshInfo);
                                 ASSERT_EQ(refreshInfo->getNss(), env.tempNss);
                             });
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnInserts) {
    auto opCtx = operationContext();
    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    writeDoc(opCtx, kNss, BSON("_id" << 0 << "x" << 2 << "y" << 10), env);

    auto entry = getLastOplogEntry(opCtx);
    auto recipShard = entry.getDestinedRecipient();

    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnInsertsInTransaction) {
    auto opCtx = operationContext();
    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    runInTransaction(
        opCtx, [&]() { writeDoc(opCtx, kNss, BSON("_id" << 0 << "x" << 2 << "y" << 10), env); });

    // Look for destined recipient in latest oplog entry. Since this write was done in a
    // transaction, the write operation will be embedded in an applyOps entry and needs to be
    // extracted.
    auto entry = getLastOplogEntry(opCtx);
    auto info = repl::ApplyOpsCommandInfo::parse(entry.getOperationToApply());

    auto ops = info.getOperations();
    auto replOp = repl::ReplOperation::parse(IDLParserContext("insertOp"), ops[0]);
    ASSERT_EQ(replOp.getNss(), kNss);

    auto recipShard = replOp.getDestinedRecipient();
    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnUpdates) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("_id" << 0 << "x" << 2 << "y" << 10 << "z" << 4));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    updateDoc(opCtx, kNss, BSON("_id" << 0), BSON("$set" << BSON("z" << 50)), env);

    auto entry = getLastOplogEntry(opCtx);
    auto recipShard = entry.getDestinedRecipient();

    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnMultiUpdates) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("x" << 0 << "y" << 10 << "z" << 4));
    client.insert(kNss, BSON("x" << 0 << "y" << 10 << "z" << 4));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(
        opCtx,
        kNss,
        ShardVersionFactory::make(ChunkVersion::IGNORED(), boost::none),
        env.dbVersion);
    client.update(
        kNss, BSON("x" << 0), BSON("$set" << BSON("z" << 5)), false /*upsert*/, true /*multi*/);

    auto entry = getLastOplogEntry(opCtx);
    auto recipShard = entry.getDestinedRecipient();

    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnUpdatesOutOfPlace) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("_id" << 0 << "x" << 2 << "y" << 10));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    updateDoc(opCtx, kNss, BSON("_id" << 0), BSON("$set" << BSON("z" << 50)), env);

    auto entry = getLastOplogEntry(opCtx);
    auto recipShard = entry.getDestinedRecipient();

    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnUpdatesInTransaction) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("_id" << 0 << "x" << 2 << "y" << 10 << "z" << 4));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    runInTransaction(opCtx, [&]() {
        updateDoc(opCtx, kNss, BSON("_id" << 0), BSON("$set" << BSON("z" << 50)), env);
    });

    // Look for destined recipient in latest oplog entry. Since this write was done in a
    // transaction, the write operation will be embedded in an applyOps entry and needs to be
    // extracted.
    auto entry = getLastOplogEntry(opCtx);
    auto info = repl::ApplyOpsCommandInfo::parse(entry.getOperationToApply());

    auto ops = info.getOperations();
    auto replOp = repl::ReplOperation::parse(IDLParserContext("insertOp"), ops[0]);
    ASSERT_EQ(replOp.getNss(), kNss);

    auto recipShard = replOp.getDestinedRecipient();
    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnDeletes) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("_id" << 0 << "x" << 2 << "y" << 10 << "z" << 4));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    deleteDoc(opCtx, kNss, BSON("_id" << 0), env);

    auto entry = getLastOplogEntry(opCtx);
    auto recipShard = entry.getDestinedRecipient();

    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestOpObserverSetsDestinedRecipientOnDeletesInTransaction) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("_id" << 0 << "x" << 2 << "y" << 10));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(
        opCtx, kNss, env.version /* shardVersion */, env.dbVersion /* databaseVersion */);
    runInTransaction(opCtx, [&]() { deleteDoc(opCtx, kNss, BSON("_id" << 0), env); });

    // Look for destined recipient in latest oplog entry. Since this write was done in a
    // transaction, the write operation will be embedded in an applyOps entry and needs to be
    // extracted.
    auto entry = getLastOplogEntry(opCtx);
    auto info = repl::ApplyOpsCommandInfo::parse(entry.getOperationToApply());

    auto ops = info.getOperations();
    auto replOp = repl::ReplOperation::parse(IDLParserContext("deleteOp"), ops[0]);
    ASSERT_EQ(replOp.getNss(), kNss);

    auto recipShard = replOp.getDestinedRecipient();
    ASSERT(recipShard);
    ASSERT_EQ(*recipShard, env.destShard);
}

TEST_F(DestinedRecipientTest, TestUpdateChangesOwningShardThrows) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("_id" << 0 << "x" << 2 << "y" << 2 << "z" << 4));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    ASSERT_THROWS(runInTransaction(opCtx,
                                   [&]() {
                                       updateDoc(opCtx,
                                                 kNss,
                                                 BSON("_id" << 0 << "x" << 2),
                                                 BSON("$set" << BSON("y" << 50)),
                                                 env);
                                   }),
                  ExceptionFor<ErrorCodes::WouldChangeOwningShard>);
}

TEST_F(DestinedRecipientTest, TestUpdateSameOwningShard) {
    auto opCtx = operationContext();

    DBDirectClient client(opCtx);
    client.insert(kNss, BSON("_id" << 0 << "x" << 2 << "y" << 2 << "z" << 4));

    auto env = setupReshardingEnv(opCtx, true);

    OperationShardingState::setShardRole(opCtx, kNss, env.version, env.dbVersion);
    runInTransaction(opCtx, [&]() {
        updateDoc(opCtx, kNss, BSON("_id" << 0 << "x" << 2), BSON("$set" << BSON("y" << 3)), env);
    });
}

}  // namespace
}  // namespace mongo
