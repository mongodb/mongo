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
#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
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

class TenantOplogApplierTestOpObserver : public OpObserverNoop {
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
        auto opCtx = cc().makeOperationContext();
        repl::setOplogCollectionName(service);
        repl::createOplog(opCtx.get());
    }

    void assertNoOpMatches(const OplogEntry& op, const MutableOplogEntry& noOp) {
        ASSERT_BSONOBJ_EQ(op.toBSON(), noOp.getObject());
        ASSERT_EQ(op.getNss(), noOp.getNss());
        ASSERT_EQ(op.getUuid(), noOp.getUuid());
        ASSERT_EQ(_migrationUuid, noOp.getFromTenantMigration());
    }

    void pushOps(const std::vector<OplogEntry>& ops) {
        std::vector<BSONObj> bsonOps;
        for (const auto& op : ops) {
            bsonOps.push_back(op.toBSON());
        }
        _oplogBuffer.push(nullptr, bsonOps.begin(), bsonOps.end());
    }

protected:
    OplogBufferMock _oplogBuffer;
    executor::NetworkInterfaceMock* _net;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::string _tenantId = "tenant";
    UUID _migrationUuid = UUID::gen();
    TenantOplogApplierTestOpObserver* _opObserver;  // Owned by service context opObserverRegistry

private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
};

TEST_F(TenantOplogApplierTest, NoOpsForSingleBatch) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo")));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    pushOps(srcOps);

    TenantOplogApplier applier(_migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor);
    ASSERT_OK(applier.startup());
    // Even if we wait for the first op in a batch, it is the last op we should be notified on.
    auto lastBatchTimes = applier.getNotificationForOpTime(srcOps.front().getOpTime()).get();
    ASSERT_EQ(srcOps.back().getOpTime(), lastBatchTimes.donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(2, entries.size());
    assertNoOpMatches(srcOps[0], entries[0]);
    assertNoOpMatches(srcOps[1], entries[1]);
    applier.shutdown();
    applier.join();
}

TEST_F(TenantOplogApplierTest, NoOpsForLargeBatch) {
    std::vector<OplogEntry> srcOps;
    // This should be big enough to use several threads to do the writing
    for (int i = 0; i < 64; i++) {
        srcOps.push_back(makeInsertOplogEntry(i + 1, NamespaceString(dbName, "foo")));
    }
    pushOps(srcOps);

    TenantOplogApplier applier(_migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor);
    ASSERT_OK(applier.startup());
    // Even if we wait for the first op in a batch, it is the last op we should be notified on.
    auto lastBatchTimes = applier.getNotificationForOpTime(srcOps.front().getOpTime()).get();
    ASSERT_EQ(srcOps.back().getOpTime(), lastBatchTimes.donorOpTime);
    auto entries = _opObserver->getEntries();
    ASSERT_EQ(srcOps.size(), entries.size());
    for (size_t i = 0; i < srcOps.size(); i++) {
        assertNoOpMatches(srcOps[i], entries[i]);
    }
    applier.shutdown();
    applier.join();
}

TEST_F(TenantOplogApplierTest, NoOpsForMultipleBatches) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo")));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    srcOps.push_back(makeInsertOplogEntry(3, NamespaceString(dbName, "baz")));
    srcOps.push_back(makeInsertOplogEntry(4, NamespaceString(dbName, "bif")));

    TenantOplogApplier applier(_migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor);
    TenantOplogBatcher::BatchLimits smallLimits(100 * 1024 /* bytes */, 2 /*ops*/);
    applier.setBatchLimits_forTest(smallLimits);
    ASSERT_OK(applier.startup());
    auto firstBatchFuture = applier.getNotificationForOpTime(srcOps[0].getOpTime());
    auto secondBatchFuture = applier.getNotificationForOpTime(srcOps[2].getOpTime());
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
    applier.shutdown();
    applier.join();
}

TEST_F(TenantOplogApplierTest, NoOpsForLargeTransaction) {
    std::vector<OplogEntry> innerOps1;
    innerOps1.push_back(makeInsertOplogEntry(11, NamespaceString(dbName, "bar")));
    innerOps1.push_back(makeInsertOplogEntry(12, NamespaceString(dbName, "bar")));
    std::vector<OplogEntry> innerOps2;
    innerOps2.push_back(makeInsertOplogEntry(21, NamespaceString(dbName, "bar")));
    innerOps2.push_back(makeInsertOplogEntry(22, NamespaceString(dbName, "bar")));
    std::vector<OplogEntry> innerOps3;
    innerOps3.push_back(makeInsertOplogEntry(31, NamespaceString(dbName, "bar")));
    innerOps3.push_back(makeInsertOplogEntry(32, NamespaceString(dbName, "bar")));

    // Makes entries with ts from range [2, 5).
    std::vector<OplogEntry> srcOps = makeMultiEntryTransactionOplogEntries(
        2, dbName, /* prepared */ false, {innerOps1, innerOps2, innerOps3});
    pushOps(srcOps);

    TenantOplogApplier applier(_migrationUuid, _tenantId, OpTime(), &_oplogBuffer, _executor);
    ASSERT_OK(applier.startup());
    // The first two ops should come in the first batch.
    auto firstBatchFuture = applier.getNotificationForOpTime(srcOps[0].getOpTime());
    ASSERT_EQ(srcOps[1].getOpTime(), firstBatchFuture.get().donorOpTime);
    // The last op is in a batch by itself.
    auto secondBatchFuture = applier.getNotificationForOpTime(srcOps[2].getOpTime());
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
    applier.shutdown();
    applier.join();
}

}  // namespace repl
}  // namespace mongo
