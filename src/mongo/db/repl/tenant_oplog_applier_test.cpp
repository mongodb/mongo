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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <algorithm>
#include <boost/optional/optional_io.hpp>
#include <vector>

#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_oplog_applier.h"
#include "mongo/db/repl/tenant_oplog_batcher.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"

namespace mongo {

using executor::TaskExecutor;
using executor::ThreadPoolExecutorTest;

namespace repl {

class TenantOplogApplierTestOpObserver : public OplogApplierImplOpObserver {
public:
    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID> uuid,
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
        const auto& recipientInfo = tenantMigrationRecipientInfo(opCtx);
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
constexpr auto dbName = "tenant_test"_sd;

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
        thread_pool_options.onCreateThread = [] { Client::initThread("TenantOplogApplier"); };
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
        _opCtx = cc().makeOperationContext();
        repl::createOplog(_opCtx.get());

        // Ensure that we are primary.
        auto replCoord = ReplicationCoordinator::get(_opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
    }

    void assertNoOpMatches(const OplogEntry& op, const MutableOplogEntry& noOp) {
        ASSERT_BSONOBJ_EQ(op.getEntry().toBSON(), noOp.getObject());
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

    StorageInterface* getStorageInterface() {
        return StorageInterface::get(_opCtx->getServiceContext());
    }

protected:
    OplogBufferMock _oplogBuffer;
    executor::NetworkInterfaceMock* _net;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::string _tenantId = "tenant";
    UUID _migrationUuid = UUID::gen();
    ServiceContext::UniqueOperationContext _opCtx;
    TenantOplogApplierTestOpObserver* _opObserver;  // Owned by service context opObserverRegistry

private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
    unittest::MinimumLoggedSeverityGuard _tenantMigrationSeverityGuard{
        logv2::LogComponent::kTenantMigration, logv2::LogSeverity::Debug(1)};
};

TEST_F(TenantOplogApplierTest, NoOpsForSingleBatch) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo"), UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar"), UUID::gen()));
    pushOps(srcOps);

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    // Even if we wait for the first op in a batch, it is the last op we should be notified on.
    auto lastBatchTimes = applier->getNotificationForOpTime(srcOps.front().getOpTime()).get();
    ASSERT_EQ(srcOps.back().getOpTime(), lastBatchTimes.donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(2, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);
    assertNoOpMatches(srcOps[1], entries[1]);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, NoOpsForLargeBatch) {
    std::vector<OplogEntry> srcOps;
    // This should be big enough to use several threads to do the writing
    for (int i = 0; i < 64; i++) {
        srcOps.push_back(makeInsertOplogEntry(i + 1, NamespaceString(dbName, "foo"), UUID::gen()));
    }
    pushOps(srcOps);

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    // Even if we wait for the first op in a batch, it is the last op we should be notified on.
    auto lastBatchTimes = applier->getNotificationForOpTime(srcOps.front().getOpTime()).get();
    ASSERT_EQ(srcOps.back().getOpTime(), lastBatchTimes.donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(srcOps.size(), entries.size());
    for (size_t i = 0; i < srcOps.size(); i++) {
        assertNoOpMatches(srcOps[i], entries[i]);
    }
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, NoOpsForMultipleBatches) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo"), UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar"), UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(3, NamespaceString(dbName, "baz"), UUID::gen()));
    srcOps.push_back(makeInsertOplogEntry(4, NamespaceString(dbName, "bif"), UUID::gen()));

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());

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
    applier->join();
}

TEST_F(TenantOplogApplierTest, NoOpsForLargeTransaction) {
    std::vector<OplogEntry> innerOps1;
    innerOps1.push_back(makeInsertOplogEntry(11, NamespaceString(dbName, "bar"), UUID::gen()));
    innerOps1.push_back(makeInsertOplogEntry(12, NamespaceString(dbName, "bar"), UUID::gen()));
    std::vector<OplogEntry> innerOps2;
    innerOps2.push_back(makeInsertOplogEntry(21, NamespaceString(dbName, "bar"), UUID::gen()));
    innerOps2.push_back(makeInsertOplogEntry(22, NamespaceString(dbName, "bar"), UUID::gen()));
    std::vector<OplogEntry> innerOps3;
    innerOps3.push_back(makeInsertOplogEntry(31, NamespaceString(dbName, "bar"), UUID::gen()));
    innerOps3.push_back(makeInsertOplogEntry(32, NamespaceString(dbName, "bar"), UUID::gen()));

    // Makes entries with ts from range [2, 5).
    std::vector<OplogEntry> srcOps = makeMultiEntryTransactionOplogEntries(
        2, dbName, /* prepared */ false, {innerOps1, innerOps2, innerOps3});
    pushOps(srcOps);

    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
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
    // Make sure the no-ops got linked properly.
    ASSERT_EQ(OpTime(), entries[0].getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(entries[0].getOpTime(), entries[1].getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(entries[1].getOpTime(), entries[2].getPrevWriteOpTimeInTransaction());
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_DatabaseMissing) {
    auto entry = makeInsertOplogEntry(1, NamespaceString(dbName, "bar"), UUID::gen());
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString&,
                                   const std::vector<BSONObj>&) { onInsertsCalled = true; };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no database was available, the insert shouldn't actually happen.
    ASSERT_FALSE(onInsertsCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_CollectionMissing) {
    createDatabase(_opCtx.get(), dbName);
    auto entry = makeInsertOplogEntry(1, NamespaceString(dbName, "bar"), UUID::gen());
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString&,
                                   const std::vector<BSONObj>&) { onInsertsCalled = true; };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no collection was available, the insert shouldn't actually happen.
    ASSERT_FALSE(onInsertsCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_InsertExisting) {
    NamespaceString nss(dbName, "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(),
                                                    nss,
                                                    {BSON("_id" << 1 << "data"
                                                                << "1")},
                                                    0));
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalled = false;
    bool onUpdateCalled = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString&,
                                   const std::vector<BSONObj>&) { onInsertsCalled = true; };
    _opObserver->onUpdateFn = [&](OperationContext* opCtx, const OplogUpdateEntryArgs&) {
        onUpdateCalled = true;
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // This insert gets converted to an upsert.
    ASSERT_FALSE(onInsertsCalled);
    ASSERT_TRUE(onUpdateCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsert_Success) {
    NamespaceString nss(dbName, "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn =
        [&](OperationContext* opCtx, const NamespaceString& nss, const std::vector<BSONObj>& docs) {
            ASSERT_FALSE(onInsertsCalled);
            onInsertsCalled = true;
            ASSERT_EQUALS(nss.db(), dbName);
            ASSERT_EQUALS(nss.coll(), "bar");
            ASSERT_EQUALS(1, docs.size());
            ASSERT_BSONOBJ_EQ(docs[0], entry.getObject());
        };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onInsertsCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInserts_Grouped) {
    // TODO(SERVER-50256): remove nss_workaround, which is used to work around a bug where
    // the first operation assigned to a worker cannot be grouped.
    NamespaceString nss_workaround(dbName, "a");
    NamespaceString nss1(dbName, "bar");
    NamespaceString nss2(dbName, "baz");
    auto uuid1 = createCollectionWithUuid(_opCtx.get(), nss1);
    auto uuid2 = createCollectionWithUuid(_opCtx.get(), nss2);
    std::vector<OplogEntry> entries;
    bool onInsertsCalledNss1 = false;
    bool onInsertsCalledNss2 = false;
    // Despite the odd one in the middle, all the others should be grouped into a single inserrt.
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
            } else {
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

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entries.back().getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onInsertsCalledNss1);
    ASSERT_TRUE(onInsertsCalledNss2);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyUpdate_MissingDocument) {
    NamespaceString nss(dbName, "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto entry = makeOplogEntry(
        repl::OpTypeEnum::kUpdate, nss, uuid, BSON("$set" << BSON("a" << 1)), BSON("_id" << 0));
    bool onInsertsCalled = false;
    bool onUpdateCalled = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<BSONObj>& docs) { onInsertsCalled = true; };
    _opObserver->onUpdateFn = [&](OperationContext* opCtx, const OplogUpdateEntryArgs&) {
        onUpdateCalled = true;
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Updates to missing documents should just be dropped, neither inserted nor updated.
    ASSERT_FALSE(onInsertsCalled);
    ASSERT_FALSE(onUpdateCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyUpdate_Success) {
    NamespaceString nss(dbName, "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto entry = makeOplogEntry(
        repl::OpTypeEnum::kUpdate, nss, uuid, BSON("$set" << BSON("a" << 1)), BSON("_id" << 0));
    bool onUpdateCalled = false;
    _opObserver->onUpdateFn = [&](OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
        onUpdateCalled = true;
        ASSERT_EQUALS(nss, args.nss);
        ASSERT_EQUALS(uuid, args.uuid);
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onUpdateCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_DatabaseMissing) {
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, NamespaceString(dbName, "bar"), UUID::gen());
    bool onDeleteCalled = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const NamespaceString&,
                                  OptionalCollectionUUID,
                                  StmtId,
                                  bool,
                                  const boost::optional<BSONObj>&) { onDeleteCalled = true; };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no database was available, the delete shouldn't actually happen.
    ASSERT_FALSE(onDeleteCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_CollectionMissing) {
    createDatabase(_opCtx.get(), dbName);
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, NamespaceString(dbName, "bar"), UUID::gen());
    bool onDeleteCalled = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const NamespaceString&,
                                  OptionalCollectionUUID,
                                  StmtId,
                                  bool,
                                  const boost::optional<BSONObj>&) { onDeleteCalled = true; };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since no collection was available, the delete shouldn't actually happen.
    ASSERT_FALSE(onDeleteCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_DocumentMissing) {
    NamespaceString nss(dbName, "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, nss, uuid, BSON("_id" << 0));
    bool onDeleteCalled = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const NamespaceString&,
                                  OptionalCollectionUUID,
                                  StmtId,
                                  bool,
                                  const boost::optional<BSONObj>&) { onDeleteCalled = true; };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    // Since the document wasn't available, onDelete should not be called.
    ASSERT_FALSE(onDeleteCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyDelete_Success) {
    NamespaceString nss(dbName, "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    ASSERT_OK(getStorageInterface()->insertDocument(_opCtx.get(), nss, {BSON("_id" << 0)}, 0));
    auto entry = makeOplogEntry(OpTypeEnum::kDelete, nss, uuid, BSON("_id" << 0));
    bool onDeleteCalled = false;
    _opObserver->onDeleteFn = [&](OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  OptionalCollectionUUID observer_uuid,
                                  StmtId,
                                  bool fromMigrate,
                                  const boost::optional<BSONObj>& o) {
        onDeleteCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
        ASSERT_TRUE(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));
        ASSERT_TRUE(opCtx->writesAreReplicated());
        ASSERT_FALSE(fromMigrate);
        ASSERT_EQUALS(nss.db(), dbName);
        ASSERT_EQUALS(nss.coll(), "bar");
        ASSERT_EQUALS(uuid, observer_uuid);
    };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(onDeleteCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCommand_Success) {
    NamespaceString nss(dbName, "t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) {
        applyCmdCalled = true;
        ASSERT_TRUE(opCtx);
        ASSERT_TRUE(opCtx->lockState()->isDbLockedForMode(nss.db(), MODE_IX));
        ASSERT_TRUE(opCtx->writesAreReplicated());
        ASSERT_EQUALS(nss, collNss);
    };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_TRUE(applyCmdCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCommand_WrongNSS) {
    // Should not be able to apply a command in the wrong namespace.
    NamespaceString nss("notmytenant", "t");
    auto op =
        BSON("op"
             << "c"
             << "ns" << nss.getCommandNS().ns() << "wall" << Date_t() << "o"
             << BSON("create" << nss.coll()) << "ts" << Timestamp(1, 1) << "ui" << UUID::gen());
    bool applyCmdCalled = false;
    _opObserver->onCreateCollectionFn = [&](OperationContext* opCtx,
                                            const CollectionPtr&,
                                            const NamespaceString& collNss,
                                            const CollectionOptions&,
                                            const BSONObj&) { applyCmdCalled = true; };
    auto entry = OplogEntry(op);
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_NOT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_FALSE(applyCmdCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCRUD_WrongNSS) {
    // Should not be able to apply a CRUD operation to a namespace not belonging to us.
    NamespaceString nss("notmytenant", "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss, uuid);
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<BSONObj>& docs) { onInsertsCalled = true; };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_NOT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_FALSE(onInsertsCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyCRUD_WrongUUID) {
    // Should not be able to apply a CRUD operation to a namespace not belonging to us, even if
    // we claim it does in the nss field.
    NamespaceString nss("notmytenant", "bar");
    NamespaceString nss_to_apply(dbName, "bar");
    auto uuid = createCollectionWithUuid(_opCtx.get(), nss);
    auto entry = makeInsertOplogEntry(1, nss_to_apply, uuid);
    bool onInsertsCalled = false;
    _opObserver->onInsertsFn = [&](OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<BSONObj>& docs) { onInsertsCalled = true; };
    pushOps({entry});
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(entry.getOpTime());
    ASSERT_NOT_OK(opAppliedFuture.getNoThrow().getStatus());
    ASSERT_FALSE(onInsertsCalled);
    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyNoop_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, "foo"));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[0].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(1, entries.size());

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[0].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, entries[0].getOpTime());

    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenNoop_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
    ASSERT_OK(applier->startup());
    auto opAppliedFuture = applier->getNotificationForOpTime(srcOps[0].getOpTime());
    auto futureRes = opAppliedFuture.getNoThrow();

    auto entries = _opObserver->getEntries();
    ASSERT_EQ(0, entries.size());

    ASSERT_OK(futureRes.getStatus());
    ASSERT_EQUALS(futureRes.getValue().donorOpTime, srcOps[0].getOpTime());
    ASSERT_EQUALS(futureRes.getValue().recipientOpTime, OpTime());

    applier->shutdown();
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyInsertThenResumeTokenNoopInDifferentBatch_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo"), UUID::gen()));
    srcOps.push_back(makeNoopOplogEntry(2, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());

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
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenNoopThenInsertInSameBatch_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, TenantMigrationRecipientService::kNoopMsg));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "foo"), UUID::gen()));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
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
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenInsertThenNoopSameTimestamp_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo"), UUID::gen()));
    srcOps.push_back(makeNoopOplogEntry(1, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    ASSERT_EQ(srcOps[0].getOpTime(), srcOps[1].getOpTime());
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
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
    applier->join();
}

TEST_F(TenantOplogApplierTest, ApplyResumeTokenInsertThenNoop_Success) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo"), UUID::gen()));
    srcOps.push_back(makeNoopOplogEntry(2, TenantMigrationRecipientService::kNoopMsg));
    pushOps(srcOps);
    auto writerPool = makeTenantMigrationWriterPool();

    auto applier = std::make_shared<TenantOplogApplier>(
        _migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor, writerPool.get());
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
    applier->join();
}

}  // namespace repl
}  // namespace mongo
