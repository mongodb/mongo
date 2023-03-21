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

#include "mongo/platform/basic.h"

#include <algorithm>
#include <vector>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_recipient_access_blocker.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_oplog_applier.h"
#include "mongo/db/repl/tenant_oplog_batcher.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using executor::TaskExecutor;
using executor::ThreadPoolExecutorTest;

namespace repl {

class TenantOplogApplierTestOpObserver : public OplogApplierImplOpObserver {
public:
    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj,
                             const boost::optional<repl::OpTime> preImageOpTime,
                             const boost::optional<repl::OpTime> postImageOpTime,
                             const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                             const boost::optional<OplogSlot> slot) final {
        MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(nss);
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(msgObj);
        oplogEntry.setObject2(o2MsgObj);
        oplogEntry.setPreImageOpTime(preImageOpTime);
        oplogEntry.setPostImageOpTime(postImageOpTime);
        oplogEntry.setPrevWriteOpTimeInTransaction(prevWriteOpTimeInTransaction);
        if (slot) {
            oplogEntry.setOpTime(*slot);
        } else {
            oplogEntry.setOpTime(getNextOpTime(opCtx));
        }
        const auto& recipientInfo = tenantMigrationInfo(opCtx);
        if (recipientInfo) {
            oplogEntry.setFromTenantMigration(recipientInfo->uuid);
        }
        stdx::lock_guard lk(_mutex);
        _entries.push_back(oplogEntry);
    }

    // Returns a vector of the oplog entries recorded, in optime order.
    std::vector<MutableOplogEntry> getEntries() {
        std::vector<MutableOplogEntry> entries;
        {
            stdx::lock_guard lk(_mutex);
            entries = _entries;
        }
        std::sort(entries.begin(),
                  entries.end(),
                  [](const MutableOplogEntry& a, const MutableOplogEntry& b) {
                      return a.getOpTime() < b.getOpTime();
                  });
        return entries;
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantOplogApplierTestOpObserver::_mutex");
    std::vector<MutableOplogEntry> _entries;
};

class TenantOplogApplierTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        // These defaults are generated from the repl_server_paremeters IDL file. Set them here
        // to start each test case from a clean state.
        tenantApplierBatchSizeBytes.store(kTenantApplierBatchSizeBytesDefault);
        tenantApplierBatchSizeOps.store(kTenantApplierBatchSizeOpsDefault);

        // Set up an OpObserver to track the documents OplogApplierImpl inserts.
        auto service = getServiceContext();
        auto opObserver = std::make_unique<TenantOplogApplierTestOpObserver>();
        _opObserver = opObserver.get();
        auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        opObserverRegistry->addObserver(std::move(opObserver));

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        executor::ThreadPoolMock::Options thread_pool_options;
        thread_pool_options.onCreateThread = [] {
            Client::initThread("TenantOplogApplier");
        };
        _executor = makeSharedThreadPoolTestExecutor(std::move(network), thread_pool_options);
        _executor->startup();
        _oplogBuffer.startup(nullptr);

        // Set up a replication coordinator and storage interface, needed for opObservers.
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
        repl::ReplicationCoordinator::set(
            service, std::make_unique<repl::ReplicationCoordinatorMock>(service));

        // Set up oplog collection. If the WT storage engine is used, the oplog collection is
        // expected to exist when fetching the next opTime (LocalOplogInfo::getNextOpTimes) to use
        // for a write.
        auto opCtx = cc().makeOperationContext();
        repl::createOplog(opCtx.get());

        MongoDSessionCatalog::set(
            service,
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        // Ensure that we are primary.
        auto replCoord = ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
    }

    void assertNoOpMatches(const OplogEntry& op, const MutableOplogEntry& noOp) {
        ASSERT_BSONOBJ_EQ(op.getEntry().toBSON(), *noOp.getObject2());
        ASSERT_EQ(op.getNss(), noOp.getNss());
        ASSERT_EQ(op.getUuid(), noOp.getUuid());
        ASSERT_EQ(_migrationUuid, noOp.getFromTenantMigration());
    }

    void pushOps(const std::vector<OplogEntry>& ops) {
        std::vector<BSONObj> bsonOps;
        for (const auto& op : ops) {
            bsonOps.push_back(op.getEntry().toBSON());
        }
        _oplogBuffer.push(nullptr, bsonOps.begin(), bsonOps.end());
    }

    std::shared_ptr<TenantOplogApplier> makeTenantMigrationOplogApplier(
        ThreadPool* writerPool,
        OpTime startApplyingAfterOpTime = OpTime(),
        OpTime cloneFinishedOpTime = OpTime(),
        boost::optional<NamespaceString> progressNss = boost::none,
        const bool isResuming = false) {
        return std::make_shared<TenantOplogApplier>(_migrationUuid,
                                                    MigrationProtocolEnum::kMultitenantMigrations,
                                                    _tenantId,
                                                    progressNss,
                                                    startApplyingAfterOpTime,
                                                    &_oplogBuffer,
                                                    _executor,
                                                    writerPool,
                                                    cloneFinishedOpTime,
                                                    isResuming);
    };

    std::shared_ptr<TenantOplogApplier> makeShardMergeOplogApplier(
        ThreadPool* writerPool,
        OpTime startApplyingAfterOpTime = OpTime(),
        OpTime cloneFinishedOpTime = OpTime()) {
        return std::make_shared<TenantOplogApplier>(_migrationUuid,
                                                    MigrationProtocolEnum::kShardMerge,
                                                    boost::none,
                                                    boost::none,
                                                    startApplyingAfterOpTime,
                                                    &_oplogBuffer,
                                                    _executor,
                                                    writerPool,
                                                    cloneFinishedOpTime,
                                                    false);
    };

    StorageInterface* getStorageInterface() {
        return StorageInterface::get(getServiceContext());
    }

protected:
    OplogBufferMock _oplogBuffer;
    executor::NetworkInterfaceMock* _net;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::string _tenantId = OID::gen().toString();
    DatabaseName _dbName = DatabaseName(TenantId(OID(_tenantId)), "test");
    UUID _migrationUuid = UUID::gen();
    TenantOplogApplierTestOpObserver* _opObserver;  // Owned by service context opObserverRegistry

private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};
};

// TODO SERVER-70007: Construct the DatabaseName object for these tests with the tenantId as the
// prefix.
TEST_F(TenantOplogApplierTest, NoOpsForSingleBatch) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(
        1,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo"),
        UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(
        2,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));
    pushOps(srcOps);

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    // Even if we wait for the first op in a batch, it is the last op we should be notified on.
    auto lastBatchTimes = applier->getNotificationForOpTime(srcOps.front().getOpTime()).get();
    ASSERT_EQ(srcOps.back().getOpTime(), lastBatchTimes.donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(2, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);
    assertNoOpMatches(srcOps[1], entries[1]);
    ASSERT_EQ(srcOps.size(), applier->getNumOpsApplied());
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, NoOpsForLargeBatch) {
    std::vector<OplogEntry> srcOps;
    // This should be big enough to use several threads to do the writing
    for (int i = 0; i < 64; i++) {
        srcOps.push_back(makeInsertOplogEntry(
            i + 1,
            NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo"),
            UUID::gen()));
    }
    pushOps(srcOps);

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    // Even if we wait for the first op in a batch, it is the last op we should be notified on.
    auto lastBatchTimes = applier->getNotificationForOpTime(srcOps.front().getOpTime()).get();
    ASSERT_EQ(srcOps.back().getOpTime(), lastBatchTimes.donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(srcOps.size(), entries.size());
    for (size_t i = 0; i < srcOps.size(); i++) {
        assertNoOpMatches(srcOps[i], entries[i]);
    }
    ASSERT_EQ(srcOps.size(), applier->getNumOpsApplied());
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, NoOpsForMultipleBatches) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(
        1,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo"),
        UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(
        2,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(
        3,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "baz"),
        UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(
        4,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bif"),
        UUID::gen()));

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());

    tenantApplierBatchSizeBytes.store(100 * 1024 /* bytes */);
    tenantApplierBatchSizeOps.store(2 /* ops */);

    ASSERT_OK(applier->startup());
    auto firstBatchFuture = applier->getNotificationForOpTime(srcOps[0].getOpTime());
    auto secondBatchFuture = applier->getNotificationForOpTime(srcOps[2].getOpTime());
    pushOps(srcOps);
    // We should see the last batch optime for each batch in our notifications.
    ASSERT_EQ(srcOps[1].getOpTime(), firstBatchFuture.get().donorOpTime);
    ASSERT_EQ(srcOps[3].getOpTime(), secondBatchFuture.get().donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(4, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);
    assertNoOpMatches(srcOps[1], entries[1]);
    assertNoOpMatches(srcOps[2], entries[2]);
    assertNoOpMatches(srcOps[3], entries[3]);
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, NoOpsForLargeTransaction) {
    std::vector<OplogEntry> innerOps1;
    innerOps1.push_back(makeInsertOplogEntry(
        11,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));
    innerOps1.push_back(makeInsertOplogEntry(
        12,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));
    std::vector<OplogEntry> innerOps2;
    innerOps2.push_back(makeInsertOplogEntry(
        21,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));
    innerOps2.push_back(makeInsertOplogEntry(
        22,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));
    std::vector<OplogEntry> innerOps3;
    innerOps3.push_back(makeInsertOplogEntry(
        31,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));
    innerOps3.push_back(makeInsertOplogEntry(
        32,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar"),
        UUID::gen()));

    // Makes entries with ts from range [2, 5).
    std::vector<OplogEntry> srcOps = makeMultiEntryTransactionOplogEntries(
        2, _dbName.db(), /* prepared */ false, {innerOps1, innerOps2, innerOps3});
    pushOps(srcOps);

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    // The first two ops should come in the first batch.
    auto firstBatchFuture = applier->getNotificationForOpTime(srcOps[0].getOpTime());
    ASSERT_EQ(srcOps[1].getOpTime(), firstBatchFuture.get().donorOpTime);
    // The last op is in a batch by itself.
    auto secondBatchFuture = applier->getNotificationForOpTime(srcOps[2].getOpTime());
    ASSERT_EQ(srcOps[2].getOpTime(), secondBatchFuture.get().donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(srcOps.size(), entries.size());
    for (size_t i = 0; i < srcOps.size(); i++) {
        assertNoOpMatches(srcOps[i], entries[i]);
    }
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, CommitUnpreparedTransaction_DataPartiallyApplied) {
    auto opCtx = cc().makeOperationContext();
    createCollectionWithUuid(opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    {
        DBDirectClient client(opCtx.get());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
    }
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto lsid = makeLogicalSessionId(opCtx.get());
    TxnNumber txnNum(0);

    const BSONObj doc1 = BSON("_id" << 1 << "data" << 1);
    const BSONObj doc2 = BSON("_id" << 2 << "data" << 2);

    auto partialOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        OpTime(Timestamp(1, 1), 1LL),
        nss,
        BSON("applyOps" << BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc1)) << "partialTxn"
                        << true),
        lsid,
        txnNum,
        {StmtId(0)},
        OpTime());

    auto commitOp = makeCommandOplogEntryWithSessionInfoAndStmtIds(
        OpTime(Timestamp(2, 1), 1LL),
        nss,
        BSON("applyOps" << BSON_ARRAY(makeInsertApplyOpsEntry(nss, uuid, doc2))),
        lsid,
        txnNum,
        {StmtId(1)},
        partialOp.getOpTime());

    ASSERT_OK(getStorageInterface()->insertDocument(opCtx.get(),
                                                    nss,
                                                    {doc1, commitOp.getOpTime().getTimestamp()},
                                                    commitOp.getOpTime().getTerm()));
    ASSERT_TRUE(docExists(opCtx.get(), nss, doc1));
    ASSERT_FALSE(docExists(opCtx.get(), nss, doc2));

    pushOps({partialOp, commitOp});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(commitOp.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    ASSERT_TRUE(docExists(opCtx.get(), nss, doc1));
    ASSERT_TRUE(docExists(opCtx.get(), nss, doc2));

    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_DatabaseMissing) {
    auto nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto entry = makeInsertOplogEntry(1, nss, UUID::gen());
    bool onInsertsCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>&) {
        if (onInsertsNss == nss) {
            onInsertsCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no database was available, the insert shouldn't actually happen.
    ASSERT_FALSE(onInsertsCalledForNss);
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_CollectionMissing) {
    auto opCtx = cc().makeOperationContext();
    createDatabase(opCtx.get(), _dbName.toString());
    auto nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto entry = makeInsertOplogEntry(1, nss, UUID::gen());
    bool onInsertsCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>&) {
        if (onInsertsNss == nss) {
            onInsertsCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no collection was available, the insert shouldn't actually happen.
    ASSERT_FALSE(onInsertsCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_InsertExisting) {
    auto opCtx = cc().makeOperationContext();
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(opCtx.get(),
                                                    nss,
                                                    {BSON("_id" << 1 << "data"
                                                                << "1")},
                                                    0));
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalledForNss = false;
    bool onUpdateCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>&) {
        if (onInsertsNss == nss) {
            onInsertsCalledForNss = true;
        }
    };
    _opObserver->onUpdateFn = [&](OperationContext* opCtx,
                                  const OplogUpdateEntryArgs& onUpdateArgs) {
        if (onUpdateArgs.coll->ns() == nss) {
            onUpdateCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();
    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // This insert gets converted to an upsert.
    ASSERT_FALSE(onInsertsCalledForNss);
    ASSERT_TRUE(onUpdateCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_UniqueKey_InsertExisting) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);

    // Create unique key index on the collection.
    auto indexKey = BSON("data" << 1);
    auto spec =
        BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                 << (indexKey.firstElementFieldNameStringData() + "_1") << "unique" << true);
    createIndex(opCtx.get(), nss, uuid, spec);

    ASSERT_OK(getStorageInterface()->insertDocument(
        opCtx.get(), nss, {BSON("_id" << 0 << "data" << 2)}, 0));
    // Insert an entry that conflicts with the existing document on the indexed field.
    auto entry =
        makeOplogEntry(repl::OpTypeEnum::kInsert, nss, uuid, BSON("_id" << 1 << "data" << 2));
    bool onInsertsCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>&) {
        if (onInsertsNss == nss) {
            onInsertsCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // The DuplicateKey error should be ignored and insert should succeed.
    ASSERT_TRUE(onInsertsCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_Success) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>& docs) {
        if (onInsertsNss == nss) {
            ASSERT_FALSE(onInsertsCalledForNss);
            onInsertsCalledForNss = true;
            // TODO Check that (onInsertsNss.dbName() == _dbName) once the OplogEntry deserializer
            // passes "tid" to the NamespaceString constructor
            ASSERT_EQUALS(onInsertsNss.dbName().db(), _dbName.toStringWithTenantId());
            ASSERT_EQUALS(onInsertsNss.coll(), "bar");
            ASSERT_EQUALS(1, docs.size());
            ASSERT_BSONOBJ_EQ(docs[0], entry.getObject());
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onInsertsCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInserts_Grouped) {
    // TODO(SERVER-50256): remove nss_workaround, which is used to work around a bug where
    // the first operation assigned to a worker cannot be grouped.
    NamespaceString nss_workaround(_dbName.toStringWithTenantId(), "a");
    NamespaceString nss1 =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    NamespaceString nss2 =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "baz");
    auto opCtx = cc().makeOperationContext();
    auto uuid1 = createCollectionWithUuid(opCtx.get(), nss1);
    auto uuid2 = createCollectionWithUuid(opCtx.get(), nss2);
    std::vector<OplogEntry> entries;
    bool onInsertsCalledNss1 = false;
    bool onInsertsCalledNss2 = false;
    // Despite the odd one in the middle, all the others should be grouped into a single insert.
    entries.push_back(makeInsertOplogEntry(1, nss1, uuid1));
    entries.push_back(makeInsertOplogEntry(2, nss1, uuid1));
    entries.push_back(makeInsertOplogEntry(3, nss1, uuid1));
    entries.push_back(makeInsertOplogEntry(4, nss2, uuid2));
    entries.push_back(makeInsertOplogEntry(5, nss1, uuid1));
    entries.push_back(makeInsertOplogEntry(6, nss1, uuid1));
    entries.push_back(makeInsertOplogEntry(7, nss1, uuid1));
    entries.push_back(makeInsertOplogEntry(8, nss_workaround, UUID::gen()));
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            if (nss == nss1) {
                ASSERT_FALSE(onInsertsCalledNss1);
                onInsertsCalledNss1 = true;
                ASSERT_EQUALS(6, docs.size());
                for (int i = 0; i < 3; i++) {
                    ASSERT_BSONOBJ_EQ(docs[i], entries[i].getObject());
                }
                for (int i = 3; i < 6; i++) {
                    ASSERT_BSONOBJ_EQ(docs[i], entries[i + 1].getObject());
                }
            } else if (nss == nss2) {
                ASSERT_EQUALS(nss2, nss);
                ASSERT_FALSE(onInsertsCalledNss2);
                onInsertsCalledNss2 = true;
                ASSERT_EQUALS(1, docs.size());
                ASSERT_BSONOBJ_EQ(docs[0], entries[3].getObject());
            }
        };
    pushOps(entries);
    // Make sure all ops end up in a single thread so they can be batched.
    auto writerPool = makeTenantMigrationWriterPool(1);

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entries.back().getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onInsertsCalledNss1);
    ASSERT_TRUE(onInsertsCalledNss2);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyUpdate_MissingDocument) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto entry = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                                nss,
                                uuid,
                                update_oplog_entry::makeDeltaOplogEntry(
                                    BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                BSON("_id" << 0));
    bool onInsertsCalledForNss = false;
    bool onUpdateCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>& docs) {
        if (onInsertsNss == nss) {
            onInsertsCalledForNss = true;
        }
    };
    _opObserver->onUpdateFn = [&](OperationContext* opCtx,
                                  const OplogUpdateEntryArgs& onUpdateArgs) {
        if (onUpdateArgs.coll->ns() == nss) {
            onUpdateCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Updates to missing documents should just be dropped, neither inserted nor updated.
    ASSERT_FALSE(onInsertsCalledForNss);
    ASSERT_FALSE(onUpdateCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyUpdate_Success) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto entry = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                                nss,
                                uuid,
                                update_oplog_entry::makeDeltaOplogEntry(
                                    BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                BSON("_id" << 0));
    bool onUpdateCalledForNss = false;
    _opObserver->onUpdateFn = [&](OperationContext* opCtx,
                                  const OplogUpdateEntryArgs& onUpdateArgs) {
        if (onUpdateArgs.coll->ns() == nss) {
            onUpdateCalledForNss = true;
            ASSERT_EQUALS(nss, onUpdateArgs.coll->ns());
            ASSERT_EQUALS(uuid, onUpdateArgs.coll->uuid());
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onUpdateCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_DatabaseMissing) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, nss, UUID::gen());
    bool onDeleteCalledForNss = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  StmtId,
                                  const OplogDeleteEntryArgs&) {
        if (coll->ns() == nss) {
            onDeleteCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no database was available, the delete shouldn't actually happen.
    ASSERT_FALSE(onDeleteCalledForNss);
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_CollectionMissing) {
    auto opCtx = cc().makeOperationContext();
    createDatabase(opCtx.get(), _dbName.toString());
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, nss, UUID::gen());
    bool onDeleteCalledForNss = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  StmtId,
                                  const OplogDeleteEntryArgs&) {
        if (coll->ns() == nss) {
            onDeleteCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no collection was available, the delete shouldn't actually happen.
    ASSERT_FALSE(onDeleteCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_DocumentMissing) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, nss, uuid, BSON("_id" << 0));
    bool onDeleteCalledForNss = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  StmtId,
                                  const OplogDeleteEntryArgs&) {
        if (coll->ns() == nss) {
            onDeleteCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since the document wasn't available, onDelete should not be called.
    ASSERT_FALSE(onDeleteCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_Success) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, nss, uuid, BSON("_id" << 0));
    bool onDeleteCalledForNss = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  StmtId,
                                  const OplogDeleteEntryArgs& args) {
        if (coll->ns() == nss) {
            onDeleteCalledForNss = true;
            ASSERT_TRUE(opCtx);
            ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
            ASSERT_TRUE(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
            ASSERT_TRUE(opCtx->writesAreReplicated());
            ASSERT_FALSE(args.fromMigrate);
            // TODO SERVER-70007 Check that (nss.dbName() == _dbName) once the OplogEntry
            // deserializer passes "tid" to the NamespaceString constructor
            ASSERT_EQUALS(nss.dbName().db(), _dbName.toStringWithTenantId());
            ASSERT_EQUALS(nss.coll(), "bar");
            ASSERT_EQUALS(uuid, coll->uuid());
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onDeleteCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCreateCollCommand_CollExisting) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto op = BSON("op"
                   << "c"
                   << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
                   << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << uuid);
    bool onCreateCollectionCalledForNss = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        if (collNss == nss) {
            onCreateCollectionCalledForNss = true;
        }
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since the collection already exists, onCreateCollection should not happen.
    ASSERT_FALSE(onCreateCollectionCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyRenameCollCommand_CollExisting) {
    NamespaceString nss1 =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo");
    NamespaceString nss2 =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss2);
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss1.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("renameCollection" << nss1.ns() << "to" << nss2.ns() << "stayTemp" << false)
             << "ts" << Timestamp(1, 1) << "ui" << uuid);
    bool onRenameCollectionCalledForNss = false;
    _opObserver->onRenameCollectionFn = [&](OperationContext* opCtx,
                                            const NamespaceString& fromColl,
                                            const NamespaceString& toColl,
                                            const boost::optional<UUID>& uuid,
                                            const boost::optional<UUID>& dropTargetUUID,
                                            std::uint64_t numRecords,
                                            bool stayTemp) {
        if (nss1 == fromColl) {
            onRenameCollectionCalledForNss = true;
        }
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since the collection already has the target name, onRenameCollection should not happen.
    ASSERT_FALSE(onRenameCollectionCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCreateCollCommand_Success) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool onCreateCollectionCalledForNss = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        if (collNss == nss) {
            onCreateCollectionCalledForNss = true;
            ASSERT_TRUE(opCtx);
            ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
            ASSERT_TRUE(opCtx->writesAreReplicated());
            ASSERT_EQUALS(nss, collNss);
        }
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onCreateCollectionCalledForNss);
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCreateIndexesCommand_Success) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "t");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("createIndexes" << nss.coll() << "v" << 2 << "key" << BSON("a" << 1) << "name"
                                     << "a_1")
             << "ts" << Timestamp(1, 1) << "ui" << uuid);
    bool onCreateIndexCalledForNss = false;
    _opObserver->onCreateIndexFn = [&](OperationContext* opCtx,
                                       const NamespaceString& collNss,
                                       const UUID& collUuid,
                                       BSONObj indexDoc,
                                       bool fromMigrate) {
        if (collNss == nss) {
            ASSERT_FALSE(onCreateIndexCalledForNss);
            onCreateIndexCalledForNss = true;
            ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(nss.dbName(), MODE_IX));
            ASSERT_TRUE(opCtx->writesAreReplicated());
            ASSERT_BSONOBJ_EQ(indexDoc,
                              BSON("v" << 2 << "key" << BSON("a" << 1) << "name"
                                       << "a_1"));
            ASSERT_EQUALS(nss, collNss);
            ASSERT_EQUALS(uuid, collUuid);
        }
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onCreateIndexCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyStartIndexBuildCommand_Failure) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "t");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto op = BSON("op"
                   << "c"
                   << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
                   << BSON("startIndexBuild" << nss.coll() << "v" << 2 << "key" << BSON("a" << 1)
                                             << "name"
                                             << "a_1")
                   << "ts" << Timestamp(1, 1) << "ui" << uuid);
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_EQUALS(opAppliedFuture.getNoThrow().getStatus().code(), 5434700);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCreateCollCommand_WrongNSS) {
    // Should not be able to apply a command in the wrong namespace.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("notmytenant", "t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool onCreateCollectionCalledForNss = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        if (collNss == nss) {
            onCreateCollectionCalledForNss = true;
        }
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_NOT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_FALSE(onCreateCollectionCalledForNss);
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCreateCollCommand_WrongNSS_Merge) {
    // Should not be able to apply a command in the wrong namespace.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("noTenantDB", "t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool onCreateCollectionCalledForNss = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        if (collNss == nss) {
            onCreateCollectionCalledForNss = true;
        }
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeShardMergeOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_EQ(opAppliedFuture.getNoThrow().getStatus().code(), ErrorCodes::InvalidTenantId);
    ASSERT_FALSE(onCreateCollectionCalledForNss);
    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDropIndexesCommand_IndexNotFound) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto op = BSON("op"
                   << "c"
                   << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
                   << BSON("dropIndexes" << nss.coll() << "index"
                                         << "a_1")
                   << "ts" << Timestamp(1, 1) << "ui" << uuid);
    bool onDropIndexCalledForNss = false;
    _opObserver->onDropIndexFn = [&](OperationContext* opCtx,
                                     const NamespaceString& onDropIndexNss,
                                     const boost::optional<UUID>& uuid,
                                     const std::string& indexName,
                                     const BSONObj& idxDescriptor) {
        onDropIndexCalledForNss = true;
    };

    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // The IndexNotFound error should be ignored and drop index should not happen.
    ASSERT_FALSE(onDropIndexCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCollModCommand_IndexNotFound) {
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto op = BSON("op"
                   << "c"
                   << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
                   << BSON("collMod" << nss.coll() << "index"
                                     << BSON("name"
                                             << "data_1"
                                             << "hidden" << true))
                   << "ts" << Timestamp(1, 1) << "ui" << uuid);
    bool onCollModCalledForNss = false;
    _opObserver->onCollModFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onCollModNss,
                                   const UUID& uuid,
                                   const BSONObj& collModCmd,
                                   const CollectionOptions& oldCollOptions,
                                   boost::optional<IndexCollModInfo> indexInfo) {
        if (onCollModNss == nss) {
            onCollModCalledForNss = true;
        }
    };

    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // The IndexNotFound error should be ignored and collMod should not happen.
    ASSERT_FALSE(onCollModCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCollModCommand_CollectionMissing) {
    auto opCtx = cc().makeOperationContext();
    createDatabase(opCtx.get(), _dbName.toString());
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    UUID uuid(UUID::gen());
    auto op = BSON("op"
                   << "c"
                   << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
                   << BSON("collMod" << nss.coll() << "index"
                                     << BSON("name"
                                             << "data_1"
                                             << "hidden" << true))
                   << "ts" << Timestamp(1, 1) << "ui" << uuid);
    bool onCollModCalledForNss = false;
    _opObserver->onCollModFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onCollModNss,
                                   const UUID& uuid,
                                   const BSONObj& collModCmd,
                                   const CollectionOptions& oldCollOptions,
                                   boost::optional<IndexCollModInfo> indexInfo) {
        if (onCollModNss == nss) {
            onCollModCalledForNss = true;
        }
    };

    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // The NamespaceNotFound error should be ignored and collMod should not happen.
    ASSERT_FALSE(onCollModCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCRUD_WrongNSS) {
    // Should not be able to apply a CRUD operation to a namespace not belonging to us.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("notmytenant", "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>& docs) {
        if (onInsertsNss == nss) {
            onInsertsCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_NOT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_FALSE(onInsertsCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCRUD_WrongNSS_Merge) {
    auto invalidTenant = TenantId(OID::gen());

    // Should not be able to apply a CRUD operation to a namespace not belonging to us.
    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(DatabaseName(invalidTenant, "test"), "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>& docs) {
        if (onInsertsNss == nss) {
            onInsertsCalledForNss = true;
        }
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeShardMergeOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_EQ(opAppliedFuture.getNoThrow().getStatus().code(), ErrorCodes::InvalidTenantId);
    ASSERT_FALSE(onInsertsCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCRUD_WrongUUID) {
    // Should not be able to apply a CRUD operation to a namespace not belonging to us, even if
    // we claim it does in the nss field.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("notmytenant", "bar");
    NamespaceString nss_to_apply = NamespaceString::createNamespaceString_forTest(_dbName, "bar");
    auto opCtx = cc().makeOperationContext();
    auto uuid = createCollectionWithUuid(opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss_to_apply, uuid);
    bool onInsertsCalledForNss = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& onInsertsNss,
                                   const std::vector<BSONObj>& docs) {
        onInsertsCalledForNss = true;
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_NOT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_FALSE(onInsertsCalledForNss);
    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyNoop_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, "foo"));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[0].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(1, entries.size());

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[0].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, entries[0].getOpTime());

    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenNoop_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[0].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(0, entries.size());

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[0].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, OpTime());

    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsertThenResumeTokenNoopInDifferentBatch_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(
        1,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo"),
        UUID::gen()));
    srcOps.push_back(makeNoopOplogEntry(2, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());

    tenantApplierBatchSizeBytes.store(100 * 1024 /* bytes */);
    tenantApplierBatchSizeOps.store(1 /* ops */);

    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[1].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(1, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[1].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, entries[0].getOpTime());

    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenNoopThenInsertInSameBatch_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, TenantMigrationRecipientService::kNoopMsg));
    srcOps.push_back(makeInsertOplogEntry(
        2,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo"),
        UUID::gen()));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[1].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(1, entries.size());
    assertNoOpMatches(srcOps[1], entries[0]);

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[1].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, entries[0].getOpTime());

    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenInsertThenNoopSameTimestamp_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(
        1,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo"),
        UUID::gen()));
    srcOps.push_back(makeNoopOplogEntry(1, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    ASSERT_EQ(srcOps[0].getOpTime(), srcOps[1].getOpTime());
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[1].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(1, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[1].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, entries[0].getOpTime());

    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenInsertThenNoop_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(
        1,
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "foo"),
        UUID::gen()));
    srcOps.push_back(makeNoopOplogEntry(2, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[1].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(1, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[1].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, entries[0].getOpTime());

    applier->shutdown();
    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_MultiKeyIndex) {
    auto opCtx = cc().makeOperationContext();
    createCollectionWithUuid(opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    NamespaceString indexedNss(_dbName.toStringWithTenantId(), "indexedColl");
    NamespaceString nonIndexedNss(_dbName.toStringWithTenantId(), "nonIndexedColl");
    auto indexedCollUUID = createCollectionWithUuid(opCtx.get(), indexedNss);
    createCollection(opCtx.get(), nonIndexedNss, CollectionOptions());

    // Create index on the collection.
    auto indexKey = BSON("val" << 1);
    auto spec = BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                         << "val_1");
    createIndex(opCtx.get(), indexedNss, indexedCollUUID, spec);

    const BSONObj multiKeyDoc = BSON("_id" << 1 << "val" << BSON_ARRAY(1 << 2));
    const BSONObj singleKeyDoc = BSON("_id" << 2 << "val" << 1);

    auto indexedOp =
        makeInsertDocumentOplogEntry(OpTime(Timestamp(1, 1), 1LL), indexedNss, multiKeyDoc);
    auto unindexedOp =
        makeInsertDocumentOplogEntry(OpTime(Timestamp(2, 1), 1LL), nonIndexedNss, singleKeyDoc);

    pushOps({indexedOp, unindexedOp});

    // Use a writer pool size of 1 to ensure that both ops from the batch are applied in the same
    // writer worker thread to ensure that the same opCtx is used.
    auto writerPool = makeTenantMigrationWriterPool(1);

    auto applier = makeTenantMigrationOplogApplier(writerPool.get());
    ASSERT_OK(applier->startup());

    auto opAppliedFuture = applier->getNotificationForOpTime(unindexedOp.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    ASSERT_TRUE(docExists(opCtx.get(), indexedNss, multiKeyDoc));
    ASSERT_TRUE(docExists(opCtx.get(), nonIndexedNss, singleKeyDoc));

    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, StoresOplogApplierProgress) {
    auto nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto entry1 = makeInsertOplogEntry(1, nss, UUID::gen());
    pushOps({entry1});
    auto writerPool = makeTenantMigrationWriterPool();

    auto progressNss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "progress");

    auto applier =
        makeTenantMigrationOplogApplier(writerPool.get(), OpTime(), OpTime(), progressNss);
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry1.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    auto opCtx = cc().makeOperationContext();
    auto progress = applier->getStoredProgress(opCtx.get());
    ASSERT_TRUE(progress.has_value());
    ASSERT_EQ(progress->getDonorOpTime(), entry1.getOpTime());

    auto entry2 = makeInsertOplogEntry(2, nss, UUID::gen());
    pushOps({entry2});

    opAppliedFuture = applier->getNotificationForOpTime(entry2.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    progress = applier->getStoredProgress(opCtx.get());
    ASSERT_TRUE(progress.has_value());
    ASSERT_EQ(progress->getDonorOpTime(), entry2.getOpTime());

    ASSERT_EQ(applier->getNumOpsApplied(), 2);

    applier->shutdown();
    _oplogBuffer.shutdown(opCtx.get());
    applier->join();
}

TEST_F(TenantOplogApplierTest, ResumesOplogApplierProgress) {
    auto nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");
    auto entry1 = makeInsertOplogEntry(1, nss, UUID::gen());
    auto entry2 = makeInsertOplogEntry(2, nss, UUID::gen());
    pushOps({entry1, entry2});
    auto writerPool = makeTenantMigrationWriterPool();

    auto progressNss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "progress");

    auto applier = makeTenantMigrationOplogApplier(
        writerPool.get(), OpTime(Timestamp(1, 0), 0), OpTime(Timestamp(1, 0), 0), progressNss);
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry1.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    opAppliedFuture = applier->getNotificationForOpTime(entry2.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    ASSERT_EQ(applier->getNumOpsApplied(), 2);

    {
        auto opCtx = cc().makeOperationContext();
        _oplogBuffer.clear(opCtx.get());
    }

    applier->shutdown();
    applier->join();

    auto entry3 = makeInsertOplogEntry(3, nss, UUID::gen());
    pushOps({entry1, entry2, entry3});

    applier = makeTenantMigrationOplogApplier(writerPool.get(),
                                              OpTime(Timestamp(1, 0), 0),
                                              OpTime(Timestamp(1, 0), 0),
                                              progressNss,
                                              true);
    ASSERT_OK(applier->startup());

    opAppliedFuture = applier->getNotificationForOpTime(entry3.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    ASSERT_EQ(applier->getNumOpsApplied(), 1);

    applier->shutdown();
    applier->join();

    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
}

TEST_F(TenantOplogApplierTest, ResumeOplogApplierDoesNotReApplyPreviouslyAppliedRetryableWrites) {
    {
        auto opCtx = cc().makeOperationContext();
        createCollectionWithUuid(opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
        DBDirectClient client(opCtx.get());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
    }

    auto getOplogEntryCount = [&]() {
        auto opCtx = cc().makeOperationContext();
        OplogInterfaceLocal oplog(opCtx.get());
        auto oplogIter = oplog.makeIterator();
        auto result = oplogIter->next();
        auto oplogEntryCount = 0;
        while (result.isOK()) {
            oplogEntryCount++;
            result = oplogIter->next();
        }
        return oplogEntryCount;
    };

    auto nss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "bar");

    const auto sessionId = makeLogicalSessionIdForTest();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(sessionId);
    sessionInfo.setTxnNumber(0);
    auto retryableWrite = makeOplogEntry({Timestamp(2, 0), 1},
                                         OpTypeEnum::kInsert,
                                         nss,
                                         BSON("_id" << 1),
                                         boost::none,
                                         sessionInfo,
                                         Date_t::now(),
                                         {0});

    pushOps({retryableWrite});

    auto writerPool = makeTenantMigrationWriterPool();

    auto progressNss =
        NamespaceString::createNamespaceString_forTest(_dbName.toStringWithTenantId(), "progress");

    auto applier = makeTenantMigrationOplogApplier(
        writerPool.get(), OpTime(Timestamp(1, 0), 0), OpTime(Timestamp(1, 0), 0), progressNss);
    ASSERT_OK(applier->startup());

    auto opAppliedFuture = applier->getNotificationForOpTime(retryableWrite.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    ASSERT_EQ(applier->getNumOpsApplied(), 1);

    // The retryable write noop entry should have been logged.
    ASSERT_EQ(getOplogEntryCount(), 1);

    applier->shutdown();
    applier->join();

    // Delete progress collection documents to simulate the loss or absensce of progress data.
    {
        auto opCtx = cc().makeOperationContext();
        auto storageInterface = StorageInterface::get(opCtx.get());
        ASSERT_OK(storageInterface->deleteDocuments(opCtx.get(),
                                                    progressNss,
                                                    boost::none,
                                                    StorageInterface::ScanDirection::kForward,
                                                    {},
                                                    BoundInclusion::kIncludeStartKeyOnly,
                                                    1U));
        _oplogBuffer.clear(opCtx.get());
    }

    pushOps({retryableWrite});

    // Create a new TenantOplogApplier with resume enabled. The first retryable write will be
    // re-applied.
    applier = makeTenantMigrationOplogApplier(writerPool.get(),
                                              OpTime(Timestamp(1, 0), 0),
                                              OpTime(Timestamp(1, 0), 0),
                                              progressNss,
                                              true);
    ASSERT_OK(applier->startup());

    opAppliedFuture = applier->getNotificationForOpTime(retryableWrite.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    ASSERT_EQ(applier->getNumOpsApplied(), 1);

    // The retryable write noop entry should not have been logged again, so the count should be the
    // same.
    ASSERT_EQ(getOplogEntryCount(), 1);

    applier->shutdown();
    applier->join();

    auto opCtx = cc().makeOperationContext();
    _oplogBuffer.shutdown(opCtx.get());
}

}  // namespace repl
}  // namespace mongo
