/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <algorithm>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/repl/oplog_applier_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
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
#include "mongo/db/transaction_resources.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {


namespace repl {

namespace {
const OpTime kDefaultCloneFinishedRecipientOpTime(Timestamp(1, 1), 1);
}  // namespace

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

class TenantOplogApplierMergeTest : public ServiceContextMongoDTest {
public:
    const TenantId kTenantId = TenantId(OID::gen());
    const UUID kMigrationUuid = UUID::gen();
    const DatabaseName kTenantDB = DatabaseName::createDatabaseName_forTest(kTenantId, "test");
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
            Client::initThread("TenantOplogApplier", getGlobalServiceContext()->getService());
        };
        _executor = makeThreadPoolTestExecutor(std::move(network), thread_pool_options);
        _executor->startup();
        _oplogBuffer.startup(nullptr);

        // Set up a replication coordinator and storage interface, needed for opObservers.
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
        repl::ReplicationCoordinator::set(
            service, std::make_unique<repl::ReplicationCoordinatorMock>(service));

        // Set up oplog collection. If the WT storage engine is used, the oplog collection is
        // expected to exist when fetching the next opTime (LocalOplogInfo::getNextOpTimes) to use
        // for a write.
        _opCtx = cc().makeOperationContext();
        repl::createOplog(_opCtx.get());

        MongoDSessionCatalog::set(
            service,
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        // Ensure that we are primary.
        auto replCoord = ReplicationCoordinator::get(_opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));

        auto recipientMtab =
            std::make_shared<TenantMigrationRecipientAccessBlocker>(service, kMigrationUuid);
        TenantMigrationAccessBlockerRegistry::get(service).add(kTenantId, std::move(recipientMtab));

        _workerPool = makeTenantMigrationWorkerPool(1);
        _applier = std::make_shared<TenantOplogApplier>(kMigrationUuid,
                                                        MigrationProtocolEnum::kShardMerge,
                                                        OpTime(),
                                                        kDefaultCloneFinishedRecipientOpTime,
                                                        boost::none,
                                                        &_oplogBuffer,
                                                        _executor,
                                                        _workerPool.get());
    }

    void tearDown() override {
        _applier->shutdown();
        _applier->join();

        _workerPool->shutdown();
        _workerPool->join();

        _executor->shutdown();
        _executor->join();

        _oplogBuffer.shutdown(_opCtx.get());

        TenantMigrationAccessBlockerRegistry::get(getGlobalServiceContext())
            .removeAccessBlockersForMigration(
                kMigrationUuid, TenantMigrationAccessBlocker::BlockerType::kRecipient);
    }

    void assertNoOpMatches(const OplogEntry& op, const MutableOplogEntry& noOp) {
        ASSERT_BSONOBJ_EQ(op.getEntry().toBSON(), *noOp.getObject2());
        ASSERT_EQ(op.getNss(), noOp.getNss());
        ASSERT_EQ(op.getUuid(), noOp.getUuid());
        ASSERT_EQ(kMigrationUuid, noOp.getFromTenantMigration());
    }

    void pushOps(const std::vector<OplogEntry>& ops) {
        std::vector<BSONObj> bsonOps;
        for (const auto& op : ops) {
            bsonOps.push_back(op.getEntry().toBSON());
        }
        _oplogBuffer.push(nullptr, bsonOps.begin(), bsonOps.end());
    }

    StorageInterface* getStorageInterface() {
        return StorageInterface::get(_opCtx->getServiceContext());
    }

protected:
    OplogBufferMock _oplogBuffer;
    executor::NetworkInterfaceMock* _net;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    ServiceContext::UniqueOperationContext _opCtx;
    TenantOplogApplierTestOpObserver* _opObserver;  // Owned by service context opObserverRegistry
    std::unique_ptr<ThreadPool> _workerPool;
    std::shared_ptr<TenantOplogApplier> _applier;


private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};
};

TEST_F(TenantOplogApplierMergeTest, NoOpsForSingleBatch) {
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "foo");
    const auto& uuid1 = createCollectionWithUuid(_opCtx.get(), nss1);
    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "bar");
    const auto& uuid2 = createCollectionWithUuid(_opCtx.get(), nss2);

    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, nss1, uuid1));
    srcOps.push_back(makeInsertOplogEntry(2, nss2, uuid2));
    pushOps(srcOps);
    ASSERT_OK(_applier->startup());
    // Even if we wait for the first op in a batch, it is the last op we should be notified on.
    auto lastBatchTimes = _applier->getNotificationForOpTime(srcOps.front().getOpTime()).get();
    ASSERT_EQ(srcOps.back().getOpTime(), lastBatchTimes.donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(2, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);
    assertNoOpMatches(srcOps[1], entries[1]);
    ASSERT_EQ(srcOps.size(), _applier->getNumOpsApplied());
}

TEST_F(TenantOplogApplierMergeTest, NoOpsForMultipleBatches) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "foo");
    const auto& uuid = createCollectionWithUuid(_opCtx.get(), nss);

    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, nss, uuid));
    srcOps.push_back(makeInsertOplogEntry(2, nss, uuid));
    srcOps.push_back(makeInsertOplogEntry(3, nss, uuid));
    srcOps.push_back(makeInsertOplogEntry(4, nss, uuid));

    tenantApplierBatchSizeBytes.store(100 * 1024 /* bytes */);
    tenantApplierBatchSizeOps.store(2 /* ops */);

    ASSERT_OK(_applier->startup());
    auto firstBatchFuture = _applier->getNotificationForOpTime(srcOps[0].getOpTime());
    auto secondBatchFuture = _applier->getNotificationForOpTime(srcOps[2].getOpTime());
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
}

TEST_F(TenantOplogApplierMergeTest, NoOpsForLargeTransaction) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "bar");
    const auto& uuid = createCollectionWithUuid(_opCtx.get(), nss);

    std::vector<OplogEntry> innerOps1;
    innerOps1.push_back(makeInsertOplogEntry(11, nss, uuid));
    innerOps1.push_back(makeInsertOplogEntry(12, nss, uuid));

    std::vector<OplogEntry> innerOps2;
    innerOps2.push_back(makeInsertOplogEntry(21, nss, uuid));
    innerOps2.push_back(makeInsertOplogEntry(22, nss, uuid));

    std::vector<OplogEntry> innerOps3;
    innerOps3.push_back(makeInsertOplogEntry(31, nss, uuid));
    innerOps3.push_back(makeInsertOplogEntry(32, nss, uuid));

    // Makes entries with ts from range [2, 5).
    std::vector<OplogEntry> srcOps = makeMultiEntryTransactionOplogEntries(
        2, kTenantDB, /* prepared */ false, {innerOps1, innerOps2, innerOps3});

    pushOps(srcOps);
    ASSERT_OK(_applier->startup());
    // The first two ops should come in the first batch.
    auto firstBatchFuture = _applier->getNotificationForOpTime(srcOps[0].getOpTime());
    ASSERT_EQ(srcOps[1].getOpTime(), firstBatchFuture.get().donorOpTime);
    // The last op is in a batch by itself.
    auto secondBatchFuture = _applier->getNotificationForOpTime(srcOps[2].getOpTime());
    ASSERT_EQ(srcOps[2].getOpTime(), secondBatchFuture.get().donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(srcOps.size(), entries.size());
    for (size_t i = 0; i < srcOps.size(); i++) {
        assertNoOpMatches(srcOps[i], entries[i]);
    }
}

TEST_F(TenantOplogApplierMergeTest, ApplyInsert_Success) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<BSONObj>& docs) {
        ASSERT_FALSE(onInsertsCalled);
        onInsertsCalled = true;
        // TODO Check that (nss.dbName() == kTenantDB) once the OplogEntry deserializer passes
        // "tid" to the NamespaceString constructor
        ASSERT_EQUALS(nss.dbName().toString_forTest(), kTenantDB.toStringWithTenantId_forTest());
        ASSERT_EQUALS(nss.coll(), "bar");
        ASSERT_EQUALS(1, docs.size());
        ASSERT_BSONOBJ_EQ(docs[0], entry.getObject());
    };
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onInsertsCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyInserts_Grouped) {
    NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "bar");
    NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "baz");
    auto uuid1 = createCollectionWithUuid(_opCtx.get(), nss1);
    auto uuid2 = createCollectionWithUuid(_opCtx.get(), nss2);

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
                ASSERT_FALSE(onInsertsCalledNss2);
                onInsertsCalledNss2 = true;
                ASSERT_EQUALS(1, docs.size());
                ASSERT_BSONOBJ_EQ(docs[0], entries[3].getObject());
            }
        };
    pushOps(entries);

    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entries.back().getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onInsertsCalledNss1);
    ASSERT_TRUE(onInsertsCalledNss2);
}

TEST_F(TenantOplogApplierMergeTest, ApplyUpdate_Success) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto entry = makeOplogEntry(repl::OpTypeEnum::kUpdate,
                                nss,
                                uuid,
                                update_oplog_entry::makeDeltaOplogEntry(
                                    BSON(doc_diff::kUpdateSectionFieldName << fromjson("{a: 1}"))),
                                BSON("_id" << 0));
    bool onUpdateCalled = false;
    _opObserver->onUpdateFn = [&](OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
        onUpdateCalled = true;
        ASSERT_EQUALS(nss, args.coll->ns());
        ASSERT_EQUALS(uuid, args.coll->uuid());
    };
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onUpdateCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyDelete_DatabaseMissing) {
    auto entry = makeOplogEntry(OpTypeEnum::kDelete,
                                NamespaceString::createNamespaceString_forTest(
                                    kTenantDB.toStringWithTenantId_forTest(), "bar"),
                                UUID::gen());
    bool onDeleteCalled = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const CollectionPtr&,
                                  StmtId,
                                  const BSONObj&,
                                  const OplogDeleteEntryArgs&) {
        onDeleteCalled = true;
    };
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no database was available, the delete shouldn't actually happen.
    ASSERT_FALSE(onDeleteCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyDelete_CollectionMissing) {
    createDatabase(_opCtx.get(), kTenantDB.toString_forTest());
    auto entry = makeOplogEntry(OpTypeEnum::kDelete,
                                NamespaceString::createNamespaceString_forTest(
                                    kTenantDB.toStringWithTenantId_forTest(), "bar"),
                                UUID::gen());
    bool onDeleteCalled = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const CollectionPtr&,
                                  StmtId,
                                  const BSONObj&,
                                  const OplogDeleteEntryArgs&) {
        onDeleteCalled = true;
    };
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no collection was available, the delete shouldn't actually happen.
    ASSERT_FALSE(onDeleteCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyDelete_Success) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, nss, uuid, BSON("_id" << 0));
    bool onDeleteCalled = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const CollectionPtr& coll,
                                  StmtId,
                                  const BSONObj&,
                                  const OplogDeleteEntryArgs& args) {
        onDeleteCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(nss, MODE_IX));
        ASSERT_TRUE(opCtx->writesAreReplicated());
        ASSERT_FALSE(args.fromMigrate);
        ASSERT_EQUALS(nss.dbName().toString_forTest(), kTenantDB.toStringWithTenantId_forTest());
        ASSERT_EQUALS(nss.coll(), "bar");
        ASSERT_EQUALS(uuid, coll->uuid());
    };
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onDeleteCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyCreateCollCommand_Success) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns_forTest() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_TRUE(opCtx->writesAreReplicated());
        ASSERT_EQUALS(nss, collNss);
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(applyCmdCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyCreateCollCommand_WrongNSS) {
    // Should not be able to apply a command in the wrong namespace.
    auto invalidTenantDB = DatabaseName::createDatabaseName_forTest(TenantId(OID::gen()), "test");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        invalidTenantDB.toStringWithTenantId_forTest(), "bar");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns_forTest() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        applyCmdCalled = true;
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    auto error = opAppliedFuture.getNoThrow().getStatus();

    ASSERT_EQ(error, ErrorCodes::InvalidTenantId);
    ASSERT_STRING_CONTAINS(error.reason(), "does not belong to a tenant being migrated");
    ASSERT_FALSE(applyCmdCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyCreateIndexesCommand_Success) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns_forTest() << "wall" << Date_t() << "o"
             << BSON("createIndexes" << nss.coll() << "v" << 2 << "key" << BSON("a" << 1) << "name"
                                     << "a_1")
             << "ts" << Timestamp(1, 1) << "ui" << uuid);
    bool applyCmdCalled = false;
    _opObserver->onCreateIndexFn = [&](OperationContext* opCtx,
                                       const NamespaceString& collNss,
                                       const UUID& collUuid,
                                       BSONObj indexDoc,
                                       bool fromMigrate) {
        ASSERT_FALSE(applyCmdCalled);
        applyCmdCalled = true;
        ASSERT_TRUE(shard_role_details::getLocker(opCtx)->isDbLockedForMode(nss.dbName(), MODE_IX));
        ASSERT_TRUE(opCtx->writesAreReplicated());
        ASSERT_BSONOBJ_EQ(indexDoc,
                          BSON("v" << 2 << "key" << BSON("a" << 1) << "name"
                                   << "a_1"));
        ASSERT_EQUALS(nss, collNss);
        ASSERT_EQUALS(uuid, collUuid);
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(applyCmdCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyStartIndexBuildCommand_Failure) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "t");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto op = BSON("op"
                   << "c"
                   << "ns" << nss.getCommandNS().ns_forTest() << "wall" << Date_t() << "o"
                   << BSON("startIndexBuild" << nss.coll() << "v" << 2 << "key" << BSON("a" << 1)
                                             << "name"
                                             << "a_1")
                   << "ts" << Timestamp(1, 1) << "ui" << uuid);
    auto entry = OplogEntry(op);
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_EQUALS(opAppliedFuture.getNoThrow().getStatus().code(), 5434700);
}

TEST_F(TenantOplogApplierMergeTest, ApplyCRUD_WrongNSS) {
    auto invalidTenantDB = DatabaseName::createDatabaseName_forTest(TenantId(OID::gen()), "test");

    // Should not be able to apply a CRUD operation to a namespace not belonging to us.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        invalidTenantDB.toStringWithTenantId_forTest(), "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            onInsertsCalled = true;
        };
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    auto error = opAppliedFuture.getNoThrow().getStatus();

    ASSERT_EQ(error, ErrorCodes::InvalidTenantId);
    ASSERT_STRING_CONTAINS(error.reason(), "does not belong to a tenant being migrated");
    ASSERT_FALSE(onInsertsCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyCRUD_WrongUUID) {
    // Should not be able to apply a CRUD operation to a namespace not belonging to us, even if
    // we claim it does in the nss field.
    auto notMyTenantDB = DatabaseName::createDatabaseName_forTest(TenantId(OID::gen()), "test");
    NamespaceString notMyTenantNss = NamespaceString::createNamespaceString_forTest(
        notMyTenantDB.toStringWithTenantId_forTest(), "bar");

    NamespaceString nss_to_apply = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), notMyTenantNss);
    auto entry = makeInsertOplogEntry(1, nss_to_apply, uuid);
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            onInsertsCalled = true;
        };
    pushOps({entry});
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(entry.getOpTime());
    auto error = opAppliedFuture.getNoThrow().getStatus();

    ASSERT_EQ(error, ErrorCodes::NamespaceNotFound);
    ASSERT_FALSE(onInsertsCalled);
}

TEST_F(TenantOplogApplierMergeTest, ApplyNoop_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, "foo"));
    pushOps(srcOps);
    ASSERT_OK(_applier->startup());
    auto opAppliedFuture = _applier->getNotificationForOpTime(srcOps[0].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(1, entries.size());

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[0].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, entries[0].getOpTime());
}

TEST_F(TenantOplogApplierMergeTest, ApplyInsert_MultiKeyIndex) {
    createCollectionWithUuid(_opCtx.get(), NamespaceString::kSessionTransactionsTableNamespace);
    NamespaceString indexedNss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "indexedColl");
    NamespaceString nonIndexedNss = NamespaceString::createNamespaceString_forTest(
        kTenantDB.toStringWithTenantId_forTest(), "nonIndexedColl");
    auto indexedCollUUID = createCollectionWithUuid(_opCtx.get(), indexedNss);
    createCollection(_opCtx.get(), nonIndexedNss, CollectionOptions());

    // Create index on the collection.
    auto indexKey = BSON("val" << 1);
    auto spec = BSON("v" << int(IndexDescriptor::kLatestIndexVersion) << "key" << indexKey << "name"
                         << "val_1");
    createIndex(_opCtx.get(), indexedNss, indexedCollUUID, spec);

    const BSONObj multiKeyDoc = BSON("_id" << 1 << "val" << BSON_ARRAY(1 << 2));
    const BSONObj singleKeyDoc = BSON("_id" << 2 << "val" << 1);

    auto indexedOp =
        makeInsertDocumentOplogEntry(OpTime(Timestamp(1, 1), 1LL), indexedNss, multiKeyDoc);
    auto unindexedOp =
        makeInsertDocumentOplogEntry(OpTime(Timestamp(2, 1), 1LL), nonIndexedNss, singleKeyDoc);

    pushOps({indexedOp, unindexedOp});

    ASSERT_OK(_applier->startup());

    auto opAppliedFuture = _applier->getNotificationForOpTime(unindexedOp.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());

    ASSERT_TRUE(docExists(_opCtx.get(), indexedNss, multiKeyDoc));
    ASSERT_TRUE(docExists(_opCtx.get(), nonIndexedNss, singleKeyDoc));
}

}  // namespace repl
}  // namespace mongo
