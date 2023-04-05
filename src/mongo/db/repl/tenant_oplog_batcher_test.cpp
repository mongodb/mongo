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

#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/tenant_oplog_batcher.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

using executor::TaskExecutor;
using executor::ThreadPoolExecutorTest;

namespace repl {

class TenantOplogBatcherTest : public unittest::Test, public ScopedGlobalServiceContextForTest {
public:
    void setUp() override {
        unittest::Test::setUp();
        Client::initThread("TenantOplogBatcherTest");
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        executor::ThreadPoolMock::Options thread_pool_options;
        thread_pool_options.onCreateThread = [] {
            Client::initThread("TenantOplogBatcher");
        };
        _executor = makeSharedThreadPoolTestExecutor(std::move(network), thread_pool_options);
        _executor->startup();
        _oplogBuffer.startup(nullptr);
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
        Client::releaseCurrent();
    }

protected:
    TenantOplogBatcher::BatchLimits bigBatchLimits =
        TenantOplogBatcher::BatchLimits(1ULL << 32, 1ULL << 32);
    OplogBufferMock _oplogBuffer;
    executor::NetworkInterfaceMock* _net;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    UUID _migrationUuid = UUID::gen();

private:
    unittest::MinimumLoggedSeverityGuard _replicationSeverityGuard{
        logv2::LogComponent::kReplication, logv2::LogSeverity::Debug(1)};
};

/**
 * Returns string representation of std::vector<TenantOplogEntry>.
 */
std::string toString(const std::vector<TenantOplogEntry>& ops) {
    StringBuilder sb;
    sb << "[";
    for (const auto& op : ops) {
        sb << " " << op.entry.toStringForLogging() << "(" << op.expansionsEntry << ")";
    }
    sb << " ]";
    return sb.str();
}
std::string toString(TenantOplogBatch& batch) {
    StringBuilder sb;
    sb << toString(batch.ops);
    for (const auto& txn : batch.expansions) {
        sb << "\n" << toString(txn);
    }
    return sb.str();
}

constexpr auto dbName = "tenant_test"_sd;

TEST_F(TenantOplogBatcherTest, CannotRequestTwoBatchesAtOnce) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    // We just started, no batch should be available.
    ASSERT(!batchFuture.isReady());
    // Can't ask for the next batch until the current batch is done.
    ASSERT_THROWS(batcher->getNextBatch(bigBatchLimits), AssertionException);

    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, OplogBatcherGroupsCrudOps) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    // We just started, no batch should be available.
    ASSERT(!batchFuture.isReady());
    std::vector<BSONObj> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "foo"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    auto batch = batchFuture.get();
    batcher->shutdown();

    ASSERT_EQUALS(srcOps.size(), batch.ops.size()) << toString(batch);
    ASSERT(batch.expansions.empty());
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[0].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[1].expansionsEntry);

    batcher->join();
}

TEST_F(TenantOplogBatcherTest, OplogBatcherFailsOnPreparedApplyOps) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);

    std::vector<BSONObj> srcOps;
    srcOps.push_back(makeApplyOpsOplogEntry(1, true).getEntry().toBSON());
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());
    ASSERT_THROWS(batchFuture.get(), AssertionException);

    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, OplogBatcherFailsOnPreparedCommit) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);

    std::vector<BSONObj> srcOps;
    srcOps.push_back(
        makeCommitTransactionOplogEntry(1, dbName, true /* prepared */).getEntry().toBSON());
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());
    ASSERT_THROWS(batchFuture.get(), AssertionException);

    batcher->shutdown();
    batcher->join();
}

// We internally add the 'b' field during applyOps expansion; we need to remove it when we check to
// see that the expansion matches the expected test values input.
static DurableReplOperation stripB(const DurableReplOperation& withB) {
    DurableReplOperation withoutB(withB);
    withoutB.setUpsert(boost::none);
    return withoutB;
}

TEST_F(TenantOplogBatcherTest, GetNextApplierBatchGroupsUnpreparedApplyOpsOpWithOtherOps) {
    std::vector<OplogEntry> innerOps;
    std::vector<BSONObj> srcOps;
    innerOps.push_back(
        makeInsertOplogEntry(10, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    innerOps.push_back(
        makeInsertOplogEntry(11, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    srcOps.push_back(makeApplyOpsOplogEntry(1, false, innerOps).getEntry().toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());

    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());
    auto batch = batchFuture.get();
    batcher->shutdown();

    ASSERT_EQUALS(2, batch.ops.size()) << toString(batch);
    ASSERT_EQUALS(1, batch.expansions.size());
    ASSERT_EQUALS(2, batch.expansions[0].size());
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQUALS(0, batch.ops[0].expansionsEntry);
    ASSERT_BSONOBJ_EQ(innerOps[0].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][0].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps[1].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][1].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[1].expansionsEntry);

    batcher->join();
}

TEST_F(TenantOplogBatcherTest, GetNextApplierBatchGroupsMultipleTransactions) {
    std::vector<OplogEntry> innerOps1;
    std::vector<OplogEntry> innerOps2;
    std::vector<BSONObj> srcOps;
    innerOps1.push_back(
        makeInsertOplogEntry(10, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    innerOps1.push_back(
        makeInsertOplogEntry(11, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    innerOps2.push_back(
        makeInsertOplogEntry(20, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    innerOps2.push_back(
        makeInsertOplogEntry(21, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    srcOps.push_back(makeApplyOpsOplogEntry(1, false, innerOps1).getEntry().toBSON());
    srcOps.push_back(makeApplyOpsOplogEntry(2, false, innerOps2).getEntry().toBSON());

    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());
    auto batch = batchFuture.get();
    batcher->shutdown();

    ASSERT_EQUALS(2, batch.ops.size()) << toString(batch);
    ASSERT_EQUALS(2, batch.expansions.size());

    // First transaction.
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQUALS(0, batch.ops[0].expansionsEntry);
    ASSERT_EQUALS(2, batch.expansions[0].size());
    ASSERT_BSONOBJ_EQ(innerOps1[0].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][0].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps1[1].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][1].getDurableReplOperation()).toBSON());

    // Second transaction.
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQUALS(1, batch.ops[1].expansionsEntry);
    ASSERT_EQUALS(2, batch.expansions[1].size());
    ASSERT_BSONOBJ_EQ(innerOps2[0].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[1][0].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps2[1].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[1][1].getDurableReplOperation()).toBSON());

    batcher->join();
}

TEST_F(TenantOplogBatcherTest, GetNextApplierBatchChecksBatchLimitsForNumberOfOperations) {
    std::vector<BSONObj> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(4, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(5, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    // Set batch limits so that each batch contains a maximum of 'BatchLimit::ops'.
    auto limits = bigBatchLimits;
    limits.ops = 3U;
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(limits);

    // First batch: [insert, insert, insert]
    auto batch = batchFuture.get();
    ASSERT_EQUALS(3U, batch.ops.size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_BSONOBJ_EQ(srcOps[2], batch.ops[2].entry.getEntry().toBSON());

    // Second batch: [insert, insert]
    batchFuture = batcher->getNextBatch(limits);
    batch = batchFuture.get();
    ASSERT_EQUALS(2U, batch.ops.size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[3], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_BSONOBJ_EQ(srcOps[4], batch.ops[1].entry.getEntry().toBSON());
    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, GetNextApplierBatchChecksBatchLimitsForSizeOfOperations) {
    std::vector<BSONObj> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    // Set batch limits so that only the first two operations can fit into the first batch.
    auto limits = bigBatchLimits;
    limits.bytes = std::size_t(srcOps[0].objsize()) + std::size_t(srcOps[1].objsize());
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(limits);

    // First batch: [insert, insert]
    auto batch = batchFuture.get();
    ASSERT_EQUALS(2U, batch.ops.size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());

    // Second batch: [insert]
    batchFuture = batcher->getNextBatch(limits);
    batch = batchFuture.get();
    ASSERT_EQUALS(1U, batch.ops.size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[2], batch.ops[0].entry.getEntry().toBSON());
    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, LargeTransactionProcessedIndividuallyAndExpanded) {
    std::vector<BSONObj> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    std::vector<OplogEntry> innerOps1;
    innerOps1.push_back(
        makeInsertOplogEntry(11, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    innerOps1.push_back(
        makeInsertOplogEntry(12, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    std::vector<OplogEntry> innerOps2;
    innerOps2.push_back(
        makeInsertOplogEntry(21, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    innerOps2.push_back(
        makeInsertOplogEntry(22, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    std::vector<OplogEntry> innerOps3;
    innerOps3.push_back(
        makeInsertOplogEntry(31, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    innerOps3.push_back(
        makeInsertOplogEntry(32, NamespaceString::createNamespaceString_forTest(dbName, "bar")));

    // Makes entries with ts from range [2, 5).
    std::vector<OplogEntry> multiEntryTransaction = makeMultiEntryTransactionOplogEntries(
        2, dbName, /* prepared */ false, {innerOps1, innerOps2, innerOps3});
    for (const auto& entry : multiEntryTransaction) {
        srcOps.push_back(entry.getEntry().toBSON());
    }

    // Push one extra operation to ensure that the last oplog entry of a large transaction
    // is processed by itself.
    srcOps.push_back(
        makeInsertOplogEntry(5, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());

    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);

    // First batch: [insert, applyops, applyops].
    auto batch = batchFuture.get();
    ASSERT_EQUALS(3U, batch.ops.size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQ(-1, batch.ops[0].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQ(-1, batch.ops[1].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[2], batch.ops[2].entry.getEntry().toBSON());
    ASSERT_EQ(-1, batch.ops[2].expansionsEntry);
    // Partial applyops are not expanded.
    ASSERT(batch.expansions.empty());

    // Second batch: 6 inserts, with 3 transaction oplog entries.
    // The last oplog entry of a large transaction must be processed by itself; all transactions are
    // expanded.
    batchFuture = batcher->getNextBatch(bigBatchLimits);
    batch = batchFuture.get();
    ASSERT_EQUALS(1U, batch.expansions.size()) << toString(batch);
    ASSERT_EQUALS(6U, batch.expansions[0].size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[3], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQ(0, batch.ops[0].expansionsEntry);

    ASSERT_BSONOBJ_EQ(innerOps1[0].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][0].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps1[1].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][1].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps2[0].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][2].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps2[1].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][3].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps3[0].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][4].getDurableReplOperation()).toBSON());
    ASSERT_BSONOBJ_EQ(innerOps3[1].getDurableReplOperation().toBSON(),
                      stripB(batch.expansions[0][5].getDurableReplOperation()).toBSON());

    // Third batch: [insert]. This confirms that the last oplog entry of a large txn will be batched
    // individually.
    batchFuture = batcher->getNextBatch(bigBatchLimits);
    batch = batchFuture.get();
    ASSERT_EQUALS(1U, batch.ops.size()) << toString(batch);
    ASSERT(batch.expansions.empty());
    ASSERT_BSONOBJ_EQ(srcOps[4], batch.ops[0].entry.getEntry().toBSON());

    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, OplogBatcherRetreivesPreImageOutOfOrder) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    // We just started, no batch should be available.
    ASSERT(!batchFuture.isReady());
    std::vector<BSONObj> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, "preImage").getEntry().toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "foo"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeUpdateOplogEntry(3,
                             NamespaceString::createNamespaceString_forTest(dbName, "bar"),
                             UUID::gen(),
                             OpTime({1, 1}, 1))
            .getEntry()
            .toBSON());

    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    auto batch = batchFuture.get();
    batcher->shutdown();

    // Expect the pre-image to have been inserted twice.
    ASSERT_EQUALS(srcOps.size() + 1, batch.ops.size()) << toString(batch);
    ASSERT(batch.expansions.empty());
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[0].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[1].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[2].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[2].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[2], batch.ops[3].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[3].expansionsEntry);

    batcher->join();
}

TEST_F(TenantOplogBatcherTest, OplogBatcherRetreivesPostImageOutOfOrder) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    // We just started, no batch should be available.
    ASSERT(!batchFuture.isReady());
    std::vector<BSONObj> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, "postImage").getEntry().toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "foo"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeUpdateOplogEntry(3,
                             NamespaceString::createNamespaceString_forTest(dbName, "bar"),
                             UUID::gen(),
                             boost::none /* preImageOpTime */,
                             OpTime({1, 1}, 1))
            .getEntry()
            .toBSON());

    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    auto batch = batchFuture.get();
    batcher->shutdown();

    // Expect the post-image to have been inserted twice.
    ASSERT_EQUALS(srcOps.size() + 1, batch.ops.size()) << toString(batch);
    ASSERT(batch.expansions.empty());
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[0].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[1].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[2].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[2].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[2], batch.ops[3].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[3].expansionsEntry);

    batcher->join();
}

TEST_F(TenantOplogBatcherTest, GetNextApplierBatchRejectsZeroBatchOpsLimits) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    // bigBatchLimits is a legal batch limit.
    auto limits = bigBatchLimits;
    limits.ops = 0;
    ASSERT_THROWS_CODE(batcher->getNextBatch(limits), DBException, 4885607);

    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, OplogBatcherRetreivesPreImageBeforeBatchStart) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    std::vector<BSONObj> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, "preImage").getEntry().toBSON());
    srcOps.push_back(
        makeUpdateOplogEntry(2,
                             NamespaceString::createNamespaceString_forTest(dbName, "bar"),
                             UUID::gen(),
                             OpTime({1, 1}, 1))
            .getEntry()
            .toBSON());

    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());
    // Pull the preImage off the buffer.
    BSONObj preImagePopped;
    ASSERT(_oplogBuffer.tryPop(nullptr /* mock does not need opCtx */, &preImagePopped));
    // Start the batcher reading after the preImage has been removed.
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    ASSERT_BSONOBJ_EQ(preImagePopped, srcOps[0]);
    auto batch = batchFuture.get();
    batcher->shutdown();

    // Expect the pre-image to have been inserted.
    ASSERT_EQUALS(srcOps.size(), batch.ops.size()) << toString(batch);
    ASSERT(batch.expansions.empty());
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[0].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[1].expansionsEntry);
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, OplogBatcherRetreivesPostImageBeforeBatchStart) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    std::vector<BSONObj> srcOps;
    srcOps.push_back(makeNoopOplogEntry(1, "postImage").getEntry().toBSON());
    srcOps.push_back(
        makeUpdateOplogEntry(2,
                             NamespaceString::createNamespaceString_forTest(dbName, "bar"),
                             UUID::gen(),
                             boost::none /* preImageOpTime */,
                             OpTime({1, 1}, 1))
            .getEntry()
            .toBSON());

    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());
    // Pull the postImage off the buffer.
    BSONObj postImagePopped;
    ASSERT(_oplogBuffer.tryPop(nullptr /* mock does not need opCtx */, &postImagePopped));
    // Start the batcher reading after the preImage has been removed.
    auto batchFuture = batcher->getNextBatch(bigBatchLimits);
    ASSERT_BSONOBJ_EQ(postImagePopped, srcOps[0]);
    auto batch = batchFuture.get();
    batcher->shutdown();

    // Expect the post-image to have been inserted.
    ASSERT_EQUALS(srcOps.size(), batch.ops.size()) << toString(batch);
    ASSERT(batch.expansions.empty());
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[0].expansionsEntry);
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());
    ASSERT_EQUALS(-1, batch.ops[1].expansionsEntry);
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, GetNextApplierBatchRejectsZeroBatchSizeLimits) {
    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(), OpTime());
    ASSERT_OK(batcher->startup());
    // bigBatchLimits is a legal batch limit.
    auto limits = bigBatchLimits;
    limits.bytes = 0;
    ASSERT_THROWS_CODE(batcher->getNextBatch(limits), DBException, 4885601);

    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, ResumeOplogBatcherFromTimestamp) {
    std::vector<BSONObj> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(4, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(5, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(4, 1), OpTime());
    ASSERT_OK(batcher->startup());

    auto batchFuture = batcher->getNextBatch(bigBatchLimits);

    auto batch = batchFuture.get();
    ASSERT_EQUALS(1U, batch.ops.size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[4], batch.ops[0].entry.getEntry().toBSON());

    batcher->shutdown();
    batcher->join();
}

TEST_F(TenantOplogBatcherTest, ResumeOplogBatcherFromNonExistentTimestamp) {
    std::vector<BSONObj> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(4, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    srcOps.push_back(
        makeInsertOplogEntry(5, NamespaceString::createNamespaceString_forTest(dbName, "bar"))
            .getEntry()
            .toBSON());
    _oplogBuffer.push(nullptr, srcOps.cbegin(), srcOps.cend());

    auto batcher = std::make_shared<TenantOplogBatcher>(
        _migrationUuid, &_oplogBuffer, _executor, Timestamp(3, 1), OpTime());
    ASSERT_OK(batcher->startup());

    auto batchFuture = batcher->getNextBatch(bigBatchLimits);

    auto batch = batchFuture.get();
    ASSERT_EQUALS(2U, batch.ops.size()) << toString(batch);
    ASSERT_BSONOBJ_EQ(srcOps[0], batch.ops[0].entry.getEntry().toBSON());
    ASSERT_BSONOBJ_EQ(srcOps[1], batch.ops[1].entry.getEntry().toBSON());

    batcher->shutdown();
    batcher->join();
}

}  // namespace repl
}  // namespace mongo
