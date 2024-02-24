/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include <limits>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_batcher_test_fixture.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {
namespace {

constexpr std::size_t kTestOplogBufferSize = 64 * 1024 * 1024;

/**
 * Minimal implementation of OplogApplier for testing.
 * executor::TaskExecutor is required only to test startup().
 */
class OplogApplierMock : public OplogApplier {
    OplogApplierMock(const OplogApplierMock&) = delete;
    OplogApplierMock& operator=(const OplogApplierMock&) = delete;

public:
    explicit OplogApplierMock(OplogBuffer* oplogBuffer);

    void _run(OplogBuffer* oplogBuffer) final;
    StatusWith<OpTime> _applyOplogBatch(OperationContext* opCtx, std::vector<OplogEntry> ops) final;
    void scheduleWritesToOplogAndChangeCollection(OperationContext* opCtx,
                                                  StorageInterface* storageInterface,
                                                  ThreadPool* writerPool,
                                                  const std::vector<OplogEntry>& ops,
                                                  bool skipWritesToOplog) override;
};

OplogApplierMock::OplogApplierMock(OplogBuffer* oplogBuffer)
    : OplogApplier(nullptr,
                   oplogBuffer,
                   nullptr,
                   OplogApplier::Options(OplogApplication::Mode::kSecondary)) {}

void OplogApplierMock::_run(OplogBuffer* oplogBuffer) {}

StatusWith<OpTime> OplogApplierMock::_applyOplogBatch(OperationContext* opCtx,
                                                      std::vector<OplogEntry> ops) {
    return OpTime();
}

void OplogApplierMock::scheduleWritesToOplogAndChangeCollection(OperationContext* opCtx,
                                                                StorageInterface* storageInterface,
                                                                ThreadPool* writerPool,
                                                                const std::vector<OplogEntry>& ops,
                                                                bool skipWritesToOplog) {}

class OplogApplierTest : public ServiceContextTest {
public:
    void setUp() override;
    void tearDown() override;
    virtual OperationContext* opCtx() {
        return _opCtxHolder.get();
    }


protected:
    std::unique_ptr<OplogBuffer> _buffer;
    std::unique_ptr<OplogApplier> _applier;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    OplogApplier::BatchLimits _limits;
};

void OplogApplierTest::setUp() {
    _buffer = std::make_unique<OplogBufferBlockingQueue>(kTestOplogBufferSize, nullptr);
    _applier = std::make_unique<OplogApplierMock>(_buffer.get());
    _opCtxHolder = makeOperationContext();

    _limits.bytes = std::numeric_limits<decltype(_limits.bytes)>::max();
    _limits.ops = std::numeric_limits<decltype(_limits.ops)>::max();
}

void OplogApplierTest::tearDown() {
    _limits = {};
    _opCtxHolder = {};
    _applier = {};
    _buffer = {};
}

const DatabaseName dbName = DatabaseName::createDatabaseName_forTest(boost::none, "test"_sd);

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsBadValueIfAnyOplogEntryHasWrongVersion) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagReduceMajorityWriteLatency", false);
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    auto secondEntry =
        makeInsertOplogEntry(2,
                             NamespaceString::createNamespaceString_forTest(dbName, "bar"),
                             boost::none,
                             OplogEntry::kOplogVersion - 1);
    srcOps.push_back(secondEntry);
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch = _applier->getNextApplierBatch(opCtx(), _limits);
    ASSERT_EQUALS(ErrorCodes::BadValue, batch.getStatus().code());
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsCrudOps) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(srcOps.size(), batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsUnpreparedApplyOpsOpWithOtherOps) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeApplyOpsOplogEntry(1, false));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsSystemDotViewsOpInOwnBatch) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString::makeSystemDotViewsNamespace(dbName)));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsServerConfigurationOpInOwnBatch) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString::kServerConfigurationNamespace));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsConfigReshardingDonorOpInOwnBatch) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString::kDonorReshardingOperationsNamespace));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsUnpreparedCommitTransactionOpWithOtherOps) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeCommitTransactionOplogEntry(1, dbName, false, 3));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsPreparedApplyOpsOrPreparedCommits) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(makeApplyOpsOplogEntry(1, true /* prepare */));
    srcOps.push_back(makeApplyOpsOplogEntry(2, true /* prepare */));
    srcOps.push_back(makeCommitTransactionOplogEntry(3, dbName, true /* prepared */));
    srcOps.push_back(makeAbortTransactionOplogEntry(4, dbName));
    srcOps.push_back(makeApplyOpsOplogEntry(5, true /* prepare */));
    srcOps.push_back(makeApplyOpsOplogEntry(6, true /* prepare */));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Prepares can be batched together.
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);

    // Prepared commit or abort must start a new batch with commits or aborts only.
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[2], batch[0]);
    ASSERT_EQUALS(srcOps[3], batch[1]);

    // Prepares can be batched together.
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[4], batch[0]);
    ASSERT_EQUALS(srcOps[5], batch[1]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsCrudOpsWithPreparedApplyOpsOrPreparedCommits) {
    std::vector<OplogEntry> srcOps;
    auto nss = NamespaceString::createNamespaceString_forTest(dbName, "bar");
    srcOps.push_back(makeInsertOplogEntry(1, nss));
    srcOps.push_back(makeApplyOpsOplogEntry(2, true /* prepare */));
    srcOps.push_back(makeApplyOpsOplogEntry(3, true /* prepare */));
    srcOps.push_back(makeCommitTransactionOplogEntry(4, dbName, true /* prepared */));
    srcOps.push_back(makeInsertOplogEntry(5, nss));
    srcOps.push_back(makeAbortTransactionOplogEntry(6, dbName));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Prepares can be batched together with normal CRUDs.
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(3U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
    ASSERT_EQUALS(srcOps[2], batch[2]);

    // Prepared commit must start a new batch.
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[3], batch[0]);

    // CRUD op cannot be in the same batch with the previous prepared commit.
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[4], batch[0]);

    // Prepared abort must start a new batch.
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[5], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsDBCheckWithCrudOps) {
    std::vector<OplogEntry> srcOps;
    auto nss = NamespaceString::createNamespaceString_forTest(dbName, "bar");
    srcOps.push_back(makeInsertOplogEntry(1, nss));
    // The DBCheck oplog shouldn't be batched with any preceding oplog, but it can be batched with
    // any subsequent oplog that is batchable.
    srcOps.push_back(makeDBCheckBatchEntry(2, nss));
    srcOps.push_back(makeApplyOpsOplogEntry(3, false /* prepare */));
    srcOps.push_back(makeInsertOplogEntry(4, nss));
    srcOps.push_back(makeApplyOpsOplogEntry(5, true /* prepare */));

    // The DBCheck oplog shouldn't be batched with the previous DBCheck oplog.
    srcOps.push_back(makeDBCheckBatchEntry(6, nss));
    srcOps.push_back(makeInsertOplogEntry(7, nss));

    // DBCheck oplog shouldn't be batched with a prepared commit that follows it as prepared commit
    // must start a new batch.
    srcOps.push_back(makeCommitTransactionOplogEntry(8, dbName, true /* prepared */));

    // DBCheck oplog shouldn't be batched with a prepared commit that precedes it.
    srcOps.push_back(makeDBCheckBatchEntry(9, nss));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // 1st batch: [insert]
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);

    // 2nd batch: [DBCheck, Unprepared ApplyOps, Insert, Prepared ApplyOps]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(4U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[1], batch[0]);
    ASSERT_EQUALS(srcOps[2], batch[1]);
    ASSERT_EQUALS(srcOps[3], batch[2]);
    ASSERT_EQUALS(srcOps[4], batch[3]);

    // 3rd batch:  [DBCheck, Insert]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[5], batch[0]);
    ASSERT_EQUALS(srcOps[6], batch[1]);

    // 4th batch: [Commit]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[7], batch[0]);

    // 5th batch: [Dbcheck]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[8], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchChecksBatchLimitsForNumberOfOperations) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(4, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(5, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Set batch limits so that each batch contains a maximum of 'BatchLimit::ops'.
    _limits.ops = 3U;

    // First batch: [insert, insert, insert]
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(3U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
    ASSERT_EQUALS(srcOps[2], batch[2]);

    // Second batch: [insert, insert]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[3], batch[0]);
    ASSERT_EQUALS(srcOps[4], batch[1]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchChecksBatchLimitsForSizeOfOperations) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Set batch limits so that only the first two operations can fit into the first batch.
    _limits.bytes =
        std::size_t(srcOps[0].getRawObjSizeBytes()) + std::size_t(srcOps[1].getRawObjSizeBytes());

    // First batch: [insert, insert]
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);

    // Second batch: [insert]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[2], batch[0]);
}

TEST_F(OplogApplierTest,
       GetNextApplierBatchChecksBatchLimitsUsingEmbededCountInUnpreparedCommitTransactionOp1) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(makeCommitTransactionOplogEntry(2, dbName, false, 3));
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Set batch limits so that commit transaction entry has to go into next batch as the only entry
    // after taking into account the embedded op count.
    _limits.ops = 3U;

    // First batch: [insert]
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);

    // Second batch: [commit]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[1], batch[0]);
}

TEST_F(OplogApplierTest,
       GetNextApplierBatchChecksBatchLimitsUsingEmbededCountInUnpreparedCommitTransactionOp2) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(makeCommitTransactionOplogEntry(3, dbName, false, 3));
    srcOps.push_back(
        makeInsertOplogEntry(4, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Set batch limits so that commit transaction entry has to go into next batch after taking into
    // account embedded op count.
    _limits.ops = 4U;

    // First batch: [insert, insert]
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);

    // Second batch: [commit, insert]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[2], batch[0]);
    ASSERT_EQUALS(srcOps[3], batch[1]);
}

TEST_F(OplogApplierTest,
       GetNextApplierBatchChecksBatchLimitsUsingEmbededCountInUnpreparedCommitTransactionOp3) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(makeCommitTransactionOplogEntry(2, dbName, false, 5));
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Set batch limits so that commit transaction entry goes into its own batch because its
    // embedded count exceeds the batch limit for ops.
    _limits.ops = 4U;

    // First batch: [insert]
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);

    // Second batch: [commit]
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[1], batch[0]);
}

TEST_F(OplogApplierTest, LastOpInLargeTransactionIsProcessedIndividually) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "bar")));

    // Makes entries with ts from range [2, 5).
    std::vector<OplogEntry> multiEntryTransaction =
        makeMultiEntryTransactionOplogEntries(2, dbName, /* prepared */ false, /* num entries*/ 3);
    for (const auto& entry : multiEntryTransaction) {
        srcOps.push_back(entry);
    }

    // Push one extra operation to ensure that the last oplog entry of a large transaction
    // is processed by itself.
    srcOps.push_back(
        makeInsertOplogEntry(5, NamespaceString::createNamespaceString_forTest(dbName, "bar")));

    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    // Set large enough batch limit to ensure that batcher is not batching because of limit, but
    // rather because it encountered the final oplog entry of a large transaction.
    _limits.ops = 10U;

    // First batch: [insert, applyOps, applyOps]
    auto batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(3U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
    ASSERT_EQUALS(srcOps[2], batch[2]);

    // Second batch: [applyOps]. The last oplog entry of a large transaction must be processed by
    // itself.
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[3], batch[0]);

    // Third batch: [insert]. The this confirms that the last oplog entry of a large txn will be
    // batched individually.
    batch = unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits)).getBatch();
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[4], batch[0]);
}

class OplogApplierDelayTest : public OplogApplierTest {
public:
    void setUp() override {
        OplogApplierTest::setUp();

        // Avoid any issues due to a clock exactly at 0 (e.g. dates being default Date_t());
        _mockClock = std::make_shared<ClockSourceMock>();
        _mockClock->advance(Milliseconds(60000));

        auto service = getServiceContext();
        service->setFastClockSource(std::make_unique<SharedClockSourceAdapter>(_mockClock));
        service->setPreciseClockSource(std::make_unique<SharedClockSourceAdapter>(_mockClock));

        // Use a smaller limit for these tests.
        _limits.ops = 3;
    }

    // Wait for the opCtx to be waited on, or for killWaits() to be run.
    bool waitForWait() {
        while (!_failWaits.load()) {
            if (opCtx()->isWaitingForConditionOrInterrupt())
                return true;
            sleepmillis(1);
        }
        return false;
    }

    // Ends any waitForWait calls.  Used to turn some potential hangs into outright failures.
    void killWaits() {
        _failWaits.store(true);
    }

protected:
    std::shared_ptr<ClockSourceMock> _mockClock;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    AtomicWord<bool> _failWaits{false};

private:
    std::string _origThreadName;
};

TEST_F(OplogApplierDelayTest, GetNextApplierBatchReturnsEmptyBatchImmediately) {
    auto batch =
        unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits, Milliseconds(10)));
    ASSERT_EQ(0, batch.count());
}

TEST_F(OplogApplierDelayTest, GetNextApplierBatchReturnsFullBatchImmediately) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    srcOps.push_back(
        makeInsertOplogEntry(2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
    srcOps.push_back(
        makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "baz")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    auto batch =
        unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits, Milliseconds(10)));
    ASSERT_EQ(3, batch.count());
}

TEST_F(OplogApplierDelayTest, GetNextApplierBatchWaitsForBatchToFill) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    stdx::thread insertThread([this, &srcOps] {
        ASSERT(waitForWait());
        {
            FailPointEnableBlock peekFailPoint("oplogBatcherPauseAfterSuccessfulPeek");
            srcOps.clear();
            srcOps.push_back(makeInsertOplogEntry(
                2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
            _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());
            peekFailPoint->waitForTimesEntered(peekFailPoint.initialTimesEntered() + 1);
            _mockClock->advance(Milliseconds(5));
        }
        ASSERT(waitForWait());
        srcOps.clear();
        srcOps.push_back(
            makeInsertOplogEntry(3, NamespaceString::createNamespaceString_forTest(dbName, "baz")));
        _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());
    });
    auto batch =
        unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits, Milliseconds(10)));
    ASSERT_EQ(3, batch.count());
    killWaits();
    insertThread.join();
}

TEST_F(OplogApplierDelayTest, GetNextApplierBatchWaitsForBatchToTimeout) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    stdx::thread insertThread([this, &srcOps] {
        ASSERT(waitForWait());
        {
            FailPointEnableBlock peekFailPoint("oplogBatcherPauseAfterSuccessfulPeek");
            srcOps.clear();
            srcOps.push_back(makeInsertOplogEntry(
                2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
            _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());
            peekFailPoint->waitForTimesEntered(peekFailPoint.initialTimesEntered() + 1);
            _mockClock->advance(Milliseconds(5));
        }
        ASSERT(waitForWait());
        _mockClock->advance(Milliseconds(5));
    });
    auto batch =
        unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits, Milliseconds(10)));
    ASSERT_EQ(2, batch.count());
    killWaits();
    insertThread.join();
}

// Makes sure that interrupting the batch while waiting does interrupt the timeout,
// but does not throw or lose any data.
TEST_F(OplogApplierDelayTest, GetNextApplierBatchInterrupted) {
    std::vector<OplogEntry> srcOps;
    srcOps.push_back(
        makeInsertOplogEntry(1, NamespaceString::createNamespaceString_forTest(dbName, "foo")));
    _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());

    stdx::thread insertThread([this, &srcOps] {
        ASSERT(waitForWait());
        {
            FailPointEnableBlock peekFailPoint("oplogBatcherPauseAfterSuccessfulPeek");
            srcOps.clear();
            srcOps.push_back(makeInsertOplogEntry(
                2, NamespaceString::createNamespaceString_forTest(dbName, "bar")));
            _applier->enqueue(opCtx(), srcOps.cbegin(), srcOps.cend());
            peekFailPoint->waitForTimesEntered(peekFailPoint.initialTimesEntered() + 1);
            _mockClock->advance(Milliseconds(5));
        }
        ASSERT(waitForWait());
        opCtx()->markKilled(ErrorCodes::InterruptedAtShutdown);
    });
    auto batch =
        unittest::assertGet(_applier->getNextApplierBatch(opCtx(), _limits, Milliseconds(10)));
    ASSERT_EQ(2, batch.count());
    ASSERT_EQ(ErrorCodes::InterruptedAtShutdown, opCtx()->checkForInterruptNoAssert());
    killWaits();
    insertThread.join();
}

}  // namespace
}  // namespace repl
}  // namespace mongo
