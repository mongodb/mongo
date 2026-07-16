// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/batched_write_context.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {

// This test fixture provides access to a properly initialized global service context to test the
// BatchedWriteContext class and its interaction with WriteUnitOfWork. For batched write
// interactions with the oplog, see BatchedWriteOutputsTest.
class BatchedWriteContextTest : public ServiceContextMongoDTest {};

TEST_F(BatchedWriteContextTest, TestBatchingCondition) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    auto& bwc = BatchedWriteContext::get(opCtx);
    ASSERT(!bwc.writesAreBatched());
    bwc.setWritesAreBatched(true);
    ASSERT(bwc.writesAreBatched());
    bwc.setWritesAreBatched(false);
    ASSERT(!bwc.writesAreBatched());
}

using BatchedWriteContextTestDeathTest = BatchedWriteContextTest;
DEATH_TEST_REGEX_F(BatchedWriteContextTestDeathTest,
                   TestDoesNotSupportAddingBatchedOperationWhileWritesAreNotBatched,
                   "Invariant failure.*_batchWrites") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    ASSERT(!bwc.writesAreBatched());

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
}

DEATH_TEST_REGEX_F(BatchedWriteContextTestDeathTest,
                   TestDoesNotSupportAddingBatchedOperationOutsideOfWUOW,
                   "Invariant failure.*inAWriteUnitOfWork()") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    auto& bwc = BatchedWriteContext::get(opCtx);
    // Need to explicitly set writes are batched to simulate op observer starting batched write.
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
}

DEATH_TEST_REGEX_F(BatchedWriteContextTestDeathTest,
                   TestCannotGroupDDLOperation,
                   "Invariant failure.*getOpType.*repl::OpTypeEnum::kDelete.*kInsert.*kUpdate") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    // Need to explicitly set writes are batched to simulate op observer starting batched write.
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "other", "coll");
    repl::ReplOperation op;
    op.setOpType(repl::OpTypeEnum::kCommand);

    op.setTid(nss.tenantId());
    op.setNss(nss.getCommandNS());
    op.setObject(
        repl::MutableOplogEntry::makeCreateCollObject(nss, CollectionOptions(), BSON("v" << 2)));
    bwc.addBatchedOperation(opCtx, op);
}

DEATH_TEST_REGEX_F(BatchedWriteContextTestDeathTest,
                   TestDoesNotSupportMultiDocTxn,
                   "Invariant failure.*!opCtx->inMultiDocumentTransaction()") {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();
    opCtx->setInMultiDocumentTransaction();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    // Need to explicitly set writes are batched to simulate op observer starting batched write.
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
}

TEST_F(BatchedWriteContextTest, TestAcceptedBatchOperationsSucceeds) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();
    auto& bwc = BatchedWriteContext::get(opCtx);

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    // Need to explicitly set writes are batched to simulate op observer
    bwc.setWritesAreBatched(true);

    BatchedWriteContext::BatchedOperations* ops = bwc.getBatchedOperations(opCtx);
    ASSERT(ops->isEmpty());

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);
    EXPECT_FALSE(ops->isEmpty());
    EXPECT_EQ(ops->numOperations(), 1U);

    op = repl::MutableOplogEntry::makeInsertOperation(
        nss, UUID::gen(), BSON("a" << 0), BSON("_id" << 1));
    bwc.addBatchedOperation(opCtx, op);
    EXPECT_EQ(ops->numOperations(), 2U);

    op = repl::MutableOplogEntry::makeInsertOperation(
        nss, UUID::gen(), BSON("a" << 1), BSON("_id" << 2));
    bwc.addBatchedOperation(opCtx, op);
    EXPECT_EQ(ops->numOperations(), 3U);

    op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    op.setChangeStreamPreImageRecordingMode(
        repl::ReplOperation::ChangeStreamPreImageRecordingMode::kPreImagesCollection);
    bwc.addBatchedOperation(opCtx, op);
    EXPECT_EQ(ops->numOperations(), 4U);

    // Batched write committing is handled outside of the batched write context and involves the
    // oplog so it is not necessary to commit the WriteUnitOfWork in this test.
    bwc.clearBatchedOperations(opCtx);
    ASSERT(ops->isEmpty());
}
TEST_F(BatchedWriteContextTest, TestDDLSucceedsWithEmptyBatch) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    bwc.setWritesAreBatched(true);

    // DDL assertion should succeed when no CRUD ops are in the batch.
    bwc.assertNoMixedBatchedOps(/*isDDL=*/true);
}

TEST_F(BatchedWriteContextTest, TestCRUDSucceedsWithNoPriorDDL) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    bwc.setWritesAreBatched(true);

    // CRUD assertion should succeed when no DDL has occurred.
    bwc.assertNoMixedBatchedOps(/*isDDL=*/false);
}

TEST_F(BatchedWriteContextTest, TestDDLFailsWithCRUDOpsInBatch) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    bwc.setWritesAreBatched(true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    auto op = repl::MutableOplogEntry::makeDeleteOperation(nss, UUID::gen(), BSON("_id" << 0));
    bwc.addBatchedOperation(opCtx, op);

    // DDL assertion should fail because CRUD ops are already in the batch.
    ASSERT_THROWS_WITH_CHECK(
        bwc.assertNoMixedBatchedOps(/*isDDL=*/true), DBException, [](const DBException& ex) {
            EXPECT_EQ(ex.code(), 12073500);
            assertionCount.tripwire.subtractAndFetch(1);
        });
}

TEST_F(BatchedWriteContextTest, TestCRUDFailsAfterDDL) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    bwc.setWritesAreBatched(true);

    // Mark that a DDL operation occurred.
    bwc.assertNoMixedBatchedOps(/*isDDL=*/true);

    // CRUD assertion should fail because DDL has occurred.
    ASSERT_THROWS_WITH_CHECK(
        bwc.assertNoMixedBatchedOps(/*isDDL=*/false), DBException, [](const DBException& ex) {
            EXPECT_EQ(ex.code(), 12073501);
            assertionCount.tripwire.subtractAndFetch(1);
        });
}

TEST_F(BatchedWriteContextTest, TestClearResetsDDLFlag) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    auto& bwc = BatchedWriteContext::get(opCtx);
    bwc.setWritesAreBatched(true);

    // Mark DDL occurred.
    bwc.assertNoMixedBatchedOps(/*isDDL=*/true);

    // Clear should reset the DDL flag.
    bwc.clearBatchedOperations(opCtx);

    // CRUD should now succeed.
    bwc.assertNoMixedBatchedOps(/*isDDL=*/false);
}

TEST_F(BatchedWriteContextTest, AtomicOperationGroupStampsStagedOperations) {
    auto opCtxRaii = makeOperationContext();
    auto opCtx = opCtxRaii.get();
    auto& bwc = BatchedWriteContext::get(opCtx);

    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    bwc.setWritesAreBatched(true);
    EXPECT_FALSE(bwc.hasAtomicOperationGroups());

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    const RecordId recordId(42);

    // An operation staged inside an AtomicOperationGroup is stamped with the group's record.
    {
        BatchedWriteContext::AtomicOperationGroup group(opCtx, recordId);
        auto op = repl::MutableOplogEntry::makeInsertOperation(
            nss, UUID::gen(), BSON("a" << 0), BSON("_id" << 0));
        bwc.addBatchedOperation(opCtx, op);
    }
    // An operation staged outside any group is not stamped.
    auto op2 = repl::MutableOplogEntry::makeInsertOperation(
        nss, UUID::gen(), BSON("a" << 1), BSON("_id" << 1));
    bwc.addBatchedOperation(opCtx, op2);

    const auto& staged = bwc.getBatchedOperations(opCtx)->getOperationsForOpObserver();
    ASSERT_EQ(staged.size(), 2U);
    ASSERT_TRUE(staged[0].getGroupRecordId().has_value());
    EXPECT_EQ(*staged[0].getGroupRecordId(), recordId);
    EXPECT_FALSE(staged[1].getGroupRecordId().has_value());
    EXPECT_TRUE(bwc.hasAtomicOperationGroups());

    // clear() resets grouping state.
    bwc.clearBatchedOperations(opCtx);
    EXPECT_FALSE(bwc.hasAtomicOperationGroups());
}

}  // namespace
}  // namespace mongo
