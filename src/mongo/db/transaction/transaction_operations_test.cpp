// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/transaction/transaction_operations.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <string>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

constexpr auto kOplogEntryCountLimit = std::numeric_limits<std::size_t>::max();
constexpr auto kOplogEntrySizeLimitBytes = static_cast<std::size_t>(BSONObjMaxUserSize);
const auto kWallClockTime = Date_t::now();

// Placeholder TransactionOperations::LogApplyOpsFn implementation.
auto doNothingLogApplyOpsFn = [](repl::MutableOplogEntry* oplogEntry,
                                 bool,
                                 bool,
                                 std::vector<StmtId>,
                                 WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
    return repl::OpTime();
};

// Builds a minimal insert operation for packer tests, optionally tagged with its own record id
// and/or an atomic-group record id.
TransactionOperations::TransactionOperation makeInsertOpForTest(
    int id, boost::optional<RecordId> recordId, boost::optional<RecordId> groupRecordId) {
    TransactionOperations::TransactionOperation op;
    op.setOpType(repl::OpTypeEnum::kInsert);
    op.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op.setObject(BSON("_id" << id));
    if (recordId) {
        op.setRecordId(*recordId);
    }
    if (groupRecordId) {
        op.setGroupRecordId(*groupRecordId);
    }
    return op;
}

TEST(TransactionOperationsTest, Basic) {
    TransactionOperations ops;
    ASSERT(ops.isEmpty());
    ASSERT_EQ(ops.numOperations(), 0);
    ASSERT_EQ(ops.getTotalOperationBytes(), 0);

    TransactionOperations::TransactionOperation op;
    auto opSize = repl::DurableOplogEntry::getDurableReplOperationSize(op);
    ASSERT_GTE(opSize, 1U);
    ASSERT_OK(ops.addOperation(op));
    ASSERT_FALSE(ops.isEmpty());
    ASSERT_EQ(ops.numOperations(), 1U);

    // Empty pre-images and post-images do not count towards operation size.
    ASSERT(op.getPreImage().isEmpty());
    ASSERT(op.getPostImage().isEmpty());
    ASSERT_EQ(ops.getTotalOperationBytes(), opSize);

    // The getOperationsForOpObserver() method supports integration with
    // existing BatchedWriteContext, TransactionParticipant usage and OpObserver
    // interfaces.
    const auto& replOps = ops.getOperationsForOpObserver();
    ASSERT_EQ(replOps.size(), ops.numOperations());
    std::size_t replOpsTotalOperationBytes = 0;
    for (const auto& replOp : replOps) {
        replOpsTotalOperationBytes += repl::DurableOplogEntry::getDurableReplOperationSize(replOp);
    }
    ASSERT_EQ(replOpsTotalOperationBytes, ops.getTotalOperationBytes());

    // Use clear() to reset container state.
    ops.clear();
    ASSERT(ops.isEmpty());
    ASSERT_EQ(ops.numOperations(), 0);
    ASSERT_EQ(ops.getTotalOperationBytes(), 0);
}

TEST(TransactionOperationsTest, AddTransactionFailsOnDuplicateStatementIds) {
    TransactionOperations::TransactionOperation op1;
    std::vector<StmtId> stmtIds1 = {1, 2, 3};
    op1.setStatementIds(stmtIds1);

    TransactionOperations::TransactionOperation op2;
    std::vector<StmtId> stmtIds2 = {3, 4, 5};
    op2.setStatementIds(stmtIds2);

    TransactionOperations::TransactionOperation op3;
    std::vector<StmtId> stmtIds3 = {3};
    op3.setStatementIds(stmtIds3);

    TransactionOperations::TransactionOperation op4;
    std::vector<StmtId> stmtIds4 = {4, 5, 7, 8};
    op4.setStatementIds(stmtIds4);

    TransactionOperations::TransactionOperation op5;
    std::vector<StmtId> stmtIds5 = {6, 7, 8, 9};
    op5.setStatementIds(stmtIds5);

    TransactionOperations::TransactionOperation op6;
    std::vector<StmtId> stmtIds6 = {6, 9};
    op6.setStatementIds(stmtIds6);

    TransactionOperations ops;
    ASSERT_OK(ops.addOperation(op1));
    ASSERT_EQ(5875600, ops.addOperation(op2).code());

    // Make sure failing to add 3,4,5 left 3 as in-use.
    ASSERT_EQ(5875600, ops.addOperation(op3).code());

    // Make sure failing to add 3,4,5 left 4 and 5 as available.
    ASSERT_OK(ops.addOperation(op4));

    // Make sure we detect a collision in a non-first item in the new statement ID list.
    ASSERT_EQ(5875600, ops.addOperation(op5).code());

    // Make sure can still add 6 and 9, which weren't added by the failed op5.
    ASSERT_OK(ops.addOperation(op6));
}

TEST(TransactionOperationsTest, GetNumberOfOperationsWithStatementIds) {
    TransactionOperations ops;

    // No operations.
    ASSERT_EQ(ops.getNumberOfOperationsWithStatementIds(), 0);

    // A single operation carrying no statement ids.
    TransactionOperations::TransactionOperation noStmtIdOp;
    ASSERT_OK(ops.addOperation(noStmtIdOp));
    ASSERT_EQ(ops.getNumberOfOperationsWithStatementIds(), 0);

    // A single operation carrying one statement id.
    ops.clear();
    TransactionOperations::TransactionOperation oneStmtIdOp;
    oneStmtIdOp.setStatementIds(std::vector<StmtId>{0});
    ASSERT_OK(ops.addOperation(oneStmtIdOp));
    ASSERT_EQ(ops.getNumberOfOperationsWithStatementIds(), 1);

    // A single operation carrying multiple statement ids still counts as one statement-bearing
    // operation.
    ops.clear();
    TransactionOperations::TransactionOperation multiStmtIdOp;
    multiStmtIdOp.setStatementIds(std::vector<StmtId>{0, 1, 2});
    ASSERT_OK(ops.addOperation(multiStmtIdOp));
    ASSERT_EQ(ops.getNumberOfOperationsWithStatementIds(), 1);

    // One statement-bearing operation alongside operations carrying no statement ids.
    ops.clear();
    TransactionOperations::TransactionOperation sideOp;
    TransactionOperations::TransactionOperation userOp;
    userOp.setStatementIds(std::vector<StmtId>{3});
    ASSERT_OK(ops.addOperation(sideOp));
    ASSERT_OK(ops.addOperation(userOp));
    ASSERT_EQ(ops.getNumberOfOperationsWithStatementIds(), 1);

    // Two operations that each carry statement ids.
    ops.clear();
    TransactionOperations::TransactionOperation firstUserOp;
    firstUserOp.setStatementIds(std::vector<StmtId>{0});
    TransactionOperations::TransactionOperation secondUserOp;
    secondUserOp.setStatementIds(std::vector<StmtId>{1});
    ASSERT_OK(ops.addOperation(firstUserOp));
    ASSERT_OK(ops.addOperation(secondUserOp));
    ASSERT_EQ(ops.getNumberOfOperationsWithStatementIds(), 2);
}

TEST(TransactionOperationsTest, AddTransactionIncludesPreImageStatistics) {
    TransactionOperations ops;

    // The size of 'op1' is added to the total byte count but it does not have
    // the additional criteria to be added to 'numberOfPrePostImages'.
    // See SERVER-58694.
    TransactionOperations::TransactionOperation op1;
    op1.setPreImage(BSON("a" << 123));
    ASSERT_OK(ops.addOperation(op1));
    ASSERT_EQ(ops.getTotalOperationBytes(),
              repl::DurableOplogEntry::getDurableReplOperationSize(op1) +
                  static_cast<std::size_t>(op1.getPreImage().objsize()));
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 0);

    // Set "pre-image for retryable writes" flag to include the pre-image in
    // the pre/post image count.
    TransactionOperations::TransactionOperation op3;
    op3.setPreImage(BSON("c" << 123));
    op3.setPreImageRecordedForRetryableInternalTransaction();
    ASSERT_OK(ops.addOperation(op3));
    ASSERT_EQ(ops.getTotalOperationBytes(),
              repl::DurableOplogEntry::getDurableReplOperationSize(op1) +
                  static_cast<std::size_t>(op1.getPreImage().objsize()) +
                  repl::DurableOplogEntry::getDurableReplOperationSize(op3) +
                  static_cast<std::size_t>(op3.getPreImage().objsize()));
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 1U);

    // Pre/post image counter should be reset after clear().
    ops.clear();
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 0);
}

TEST(TransactionOperationsTest, AddTransactionIncludesPostImageStatistics) {
    TransactionOperations ops;

    TransactionOperations::TransactionOperation op1;
    op1.setPostImage(BSON("a" << 123));
    ASSERT_OK(ops.addOperation(op1));
    ASSERT_EQ(ops.getTotalOperationBytes(),
              repl::DurableOplogEntry::getDurableReplOperationSize(op1) +
                  static_cast<std::size_t>(op1.getPostImage().objsize()));
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 1U);

    // Pre/post image counter should be reset after clear().
    ops.clear();
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 0);
}

TEST(TransactionOperationsTest, AddTransactionIncludesPreAndPostImageStatistics) {
    TransactionOperations ops;

    TransactionOperations::TransactionOperation op1;
    op1.setPreImage(BSON("a" << 123));
    op1.setPreImageRecordedForRetryableInternalTransaction();
    ASSERT_OK(ops.addOperation(op1));
    ASSERT_EQ(ops.getTotalOperationBytes(),
              repl::DurableOplogEntry::getDurableReplOperationSize(op1) +
                  static_cast<std::size_t>(op1.getPreImage().objsize()));
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 1U);

    TransactionOperations::TransactionOperation op2;
    op2.setPostImage(BSON("b" << 123));
    ASSERT_OK(ops.addOperation(op2));
    ASSERT_EQ(ops.getTotalOperationBytes(),
              repl::DurableOplogEntry::getDurableReplOperationSize(op1) +
                  static_cast<std::size_t>(op1.getPreImage().objsize()) +
                  repl::DurableOplogEntry::getDurableReplOperationSize(op2) +
                  static_cast<std::size_t>(op2.getPostImage().objsize()));
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 2U);

    // Pre/post image counter should be reset after clear().
    ops.clear();
    ASSERT_EQ(ops.getNumberOfPrePostImagesToWrite(), 0);
}

TEST(TransactionOperationsTest, AddTransactionEnforceTotalOperationSizeLimit) {
    TransactionOperations::TransactionOperation op1;
    auto opSize1 = repl::DurableOplogEntry::getDurableReplOperationSize(op1);

    TransactionOperations::TransactionOperation op2;
    auto opSize2 = repl::DurableOplogEntry::getDurableReplOperationSize(op2);

    auto sizeLimit = opSize1 + opSize2 - 1;
    TransactionOperations ops;
    ASSERT_OK(ops.addOperation(op1, sizeLimit));
    ASSERT_EQ(ErrorCodes::TransactionTooLarge, ops.addOperation(op2, sizeLimit));
}

TEST(TransactionOperationsTest, GetCollectionUUIDsIgnoresNoopOperations) {
    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kCommand);
    op1.setUuid(UUID::gen());

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);
    op2.setUuid(UUID::gen());

    // This operation's UUID will not be included in the getCollectionUUIDs() result.
    TransactionOperations::TransactionOperation op3;
    op3.setOpType(repl::OpTypeEnum::kNoop);
    op3.setUuid(UUID::gen());

    // This operation has no UUID and is added to ensure operations without UUIDs
    // are handled properly.
    TransactionOperations::TransactionOperation op4;
    op4.setOpType(repl::OpTypeEnum::kDelete);
    ASSERT_FALSE(op4.getUuid());

    TransactionOperations ops;
    ASSERT_OK(ops.addOperation(op1));
    ASSERT_OK(ops.addOperation(op2));
    ASSERT_OK(ops.addOperation(op3));
    ASSERT_OK(ops.addOperation(op4));

    auto uuids = ops.getCollectionUUIDs();
    ASSERT_EQ(uuids.size(), 2U);
    ASSERT(uuids.count(*op1.getUuid()));
    ASSERT(uuids.count(*op2.getUuid()));
}

TEST(TransactionOperationsTest, GetApplyOpsInfoEmptyOps) {
    TransactionOperations ops;
    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    ASSERT_EQ(info.applyOpsEntries.size(), 0);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 0);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);
}

DEATH_TEST(TransactionOperationsTestDeathTest,
           LogOplogEntriesInsufficientSlots,
           "Wrong number of oplogSlots reserved") {
    TransactionOperations ops;
    TransactionOperations::TransactionOperation op;
    op.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                  // required for DurableReplOperation::serialize()
    op.setObject(BSON("_id" << 1));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op));
    auto applyOpsInfo = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                            kOplogEntrySizeLimitBytes,
                                            /*prepare=*/false);
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    ops.logOplogEntries(/*oplogSlots=*/{},
                        applyOpsInfo,
                        kWallClockTime,
                        WriteUnitOfWork::OplogEntryGroupType::kDontGroup,
                        doNothingLogApplyOpsFn,
                        &imageToWrite);
}

DEATH_TEST(TransactionOperationsTestDeathTest,
           LogOplogEntriesTooManySlots,
           "Wrong number of oplogSlots reserved") {
    TransactionOperations ops;
    TransactionOperations::TransactionOperation op;
    op.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                  // required for DurableReplOperation::serialize()
    op.setObject(BSON("_id" << 1));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op));
    auto applyOpsInfo = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                            kOplogEntrySizeLimitBytes,
                                            /*prepare=*/false);
    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 1), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(1, 2), /*term=*/1LL});
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    ops.logOplogEntries(oplogSlots,
                        applyOpsInfo,
                        kWallClockTime,
                        WriteUnitOfWork::OplogEntryGroupType::kDontGroup,
                        doNothingLogApplyOpsFn,
                        &imageToWrite);
}

TEST(TransactionOperationsTest, GetApplyOpsInfoReturnsOneEntryContainingTwoOperations) {
    TransactionOperations ops;

    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op1.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                   // required for DurableReplOperation::serialize()
    op1.setObject(BSON("_id" << 1));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op2.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                   // required for DurableReplOperation::serialize()
    op2.setObject(BSON("_id" << 2));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op2));

    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);

    ASSERT_EQ(info.numberOfOplogSlotsRequired, 1U);
    ASSERT_EQ(info.applyOpsEntries.size(), 1U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    ASSERT_EQ(info.applyOpsEntries[0].oplogSlotIndex, 0);
    ASSERT_EQ(info.applyOpsEntries[0].operations.size(), 2U);
    ASSERT_BSONOBJ_EQ(info.applyOpsEntries[0].operations[0], op1.toBSON());
    ASSERT_BSONOBJ_EQ(info.applyOpsEntries[0].operations[1], op2.toBSON());
}

TEST(TransactionOperationsTest, GetApplyOpsInfoKeepsARecordsOperationsInOneEntry) {
    TransactionOperations ops;
    // Two records, each with a collection op (own record id) and a side op (atomic-group record
    // id), laid out contiguously as groupByRecordId would produce.
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(1, RecordId(1), boost::none)));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(2, boost::none, RecordId(1))));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(3, RecordId(2), boost::none)));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(4, boost::none, RecordId(2))));

    // At most three operations per entry: without grouping, record 2's group would straddle the
    // boundary; with respectAtomicGroups it rolls whole into the second entry.
    auto info = ops.getApplyOpsInfo(/*oplogEntryCountLimit=*/3U,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false,
                                    /*respectAtomicGroups=*/true);

    ASSERT_EQ(info.applyOpsEntries.size(), 2U);
    EXPECT_EQ(info.applyOpsEntries[0].operations.size(), 2U);
    EXPECT_EQ(info.applyOpsEntries[1].operations.size(), 2U);
}

TEST(TransactionOperationsTest, GetApplyOpsInfoWithoutAtomicGroupsMaySplitARecord) {
    TransactionOperations ops;
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(1, RecordId(1), boost::none)));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(2, boost::none, RecordId(1))));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(3, RecordId(2), boost::none)));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(4, boost::none, RecordId(2))));

    // Default respectAtomicGroups=false: entries fill by count, splitting record 2's group.
    auto info = ops.getApplyOpsInfo(/*oplogEntryCountLimit=*/3U,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);

    ASSERT_EQ(info.applyOpsEntries.size(), 2U);
    EXPECT_EQ(info.applyOpsEntries[0].operations.size(), 3U);
    EXPECT_EQ(info.applyOpsEntries[1].operations.size(), 1U);
}

TEST(TransactionOperationsTest, GetApplyOpsInfoThrowsOnOversizedGroup) {
    TransactionOperations ops;
    // A single record whose two operations together exceed the size limit.
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(1, RecordId(1), boost::none)));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(2, boost::none, RecordId(1))));

    const auto oneOpSize = repl::DurableOplogEntry::getDurableReplOperationSize(
        makeInsertOpForTest(1, RecordId(1), boost::none));
    ASSERT_THROWS_CODE(ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                           /*oplogEntrySizeLimitBytes=*/oneOpSize,
                                           /*prepare=*/false,
                                           /*respectAtomicGroups=*/true),
                       DBException,
                       ErrorCodes::TransactionTooLarge);
}

TEST(TransactionOperationsTest, GroupByRecordIdMakesGroupsContiguous) {
    TransactionOperations ops;
    // Staging order stages all side ops before all collection ops, interleaving the records.
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(1, boost::none, RecordId(1))));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(2, boost::none, RecordId(2))));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(3, RecordId(1), boost::none)));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(4, RecordId(2), boost::none)));

    ops.groupByRecordId();

    auto keyOf = [](const repl::ReplOperation& op) {
        return op.getGroupRecordId() ? *op.getGroupRecordId() : *op.getRecordId();
    };
    const auto& grouped = ops.getOperationsForOpObserver();
    ASSERT_EQ(grouped.size(), 4U);
    EXPECT_EQ(keyOf(grouped[0]), RecordId(1));
    EXPECT_EQ(keyOf(grouped[1]), RecordId(1));
    EXPECT_EQ(keyOf(grouped[2]), RecordId(2));
    EXPECT_EQ(keyOf(grouped[3]), RecordId(2));
}

TEST(TransactionOperationsTest, GroupByRecordIdEmitsGroupsInFirstAppearanceOrder) {
    TransactionOperations ops;
    // Record 2's side op appears before record 1's, so record 2's group must be emitted first --
    // groups follow first-appearance (collection-write) order, not record-id value order.
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(1, boost::none, RecordId(2))));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(2, boost::none, RecordId(1))));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(3, RecordId(2), boost::none)));
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(4, RecordId(1), boost::none)));

    ops.groupByRecordId();

    auto keyOf = [](const repl::ReplOperation& op) {
        return op.getGroupRecordId() ? *op.getGroupRecordId() : *op.getRecordId();
    };
    const auto& grouped = ops.getOperationsForOpObserver();
    ASSERT_EQ(grouped.size(), 4U);
    EXPECT_EQ(keyOf(grouped[0]), RecordId(2));
    EXPECT_EQ(keyOf(grouped[1]), RecordId(2));
    EXPECT_EQ(keyOf(grouped[2]), RecordId(1));
    EXPECT_EQ(keyOf(grouped[3]), RecordId(1));
}

TEST(TransactionOperationsTest, GroupByRecordIdMergesMultipleCollectionOpsOnOneRecord) {
    TransactionOperations ops;
    // Two collection ops (own record id) touch record 1 in one batch, each with its own side op.
    // All four operations must collapse into a single contiguous group.
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(1, boost::none, RecordId(1))));  // side, op A
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(2, RecordId(1), boost::none)));  // collection A
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(3, boost::none, RecordId(1))));  // side, op B
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(4, RecordId(1), boost::none)));  // collection B

    ops.groupByRecordId();

    auto keyOf = [](const repl::ReplOperation& op) {
        return op.getGroupRecordId() ? *op.getGroupRecordId() : *op.getRecordId();
    };
    const auto& grouped = ops.getOperationsForOpObserver();
    ASSERT_EQ(grouped.size(), 4U);
    for (const auto& op : grouped) {
        EXPECT_EQ(keyOf(op), RecordId(1));
    }
    // Order within the merged group is preserved from staging.
    EXPECT_EQ(grouped[0].getObject()["_id"].numberInt(), 1);
    EXPECT_EQ(grouped[1].getObject()["_id"].numberInt(), 2);
    EXPECT_EQ(grouped[2].getObject()["_id"].numberInt(), 3);
    EXPECT_EQ(grouped[3].getObject()["_id"].numberInt(), 4);
}

TEST(TransactionOperationsTest, GroupByRecordIdLeavesOperationsWithoutARecordInPlace) {
    TransactionOperations ops;
    // Operations belonging to no record stay put as singletons, only the grouped ops move together.
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(1, boost::none, RecordId(1))));  // record 1 side
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(2, boost::none, boost::none)));  // no record
    ASSERT_OK(ops.addOperation(makeInsertOpForTest(3, RecordId(1), boost::none)));  // record 1 coll

    ops.groupByRecordId();

    const auto& grouped = ops.getOperationsForOpObserver();
    ASSERT_EQ(grouped.size(), 3U);
    // Record 1's two ops are contiguous; the record-less op keeps its relative position after them.
    EXPECT_EQ(grouped[0].getObject()["_id"].numberInt(), 1);
    EXPECT_EQ(grouped[1].getObject()["_id"].numberInt(), 3);
    EXPECT_EQ(grouped[2].getObject()["_id"].numberInt(), 2);
}

TEST(TransactionOperationsTest, GetApplyOpsInfoRespectsOperationCountLimit) {
    TransactionOperations ops;

    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op1.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                   // required for DurableReplOperation::serialize()
    op1.setObject(BSON("_id" << 1));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op2.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                   // required for DurableReplOperation::serialize()
    op2.setObject(BSON("_id" << 2));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op2));

    // Restrict each applyOps entry to holding at most one operation.
    auto info = ops.getApplyOpsInfo(
        /*oplogEntryCountLimit=*/1U,
        /*oplogEntrySizeLimitBytes=*/static_cast<std::size_t>(BSONObjMaxUserSize),
        /*prepare=*/false);

    ASSERT_EQ(info.numberOfOplogSlotsRequired, 2U);
    ASSERT_EQ(info.applyOpsEntries.size(), 2U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    // Check first applyOps entry.
    ASSERT_EQ(info.applyOpsEntries[0].oplogSlotIndex, 0);
    ASSERT_EQ(info.applyOpsEntries[0].operations.size(), 1U);
    ASSERT_BSONOBJ_EQ(info.applyOpsEntries[0].operations[0], op1.toBSON());

    // Check second applyOps entry.
    ASSERT_EQ(info.applyOpsEntries[1].oplogSlotIndex, 1);
    ASSERT_EQ(info.applyOpsEntries[1].operations.size(), 1U);
    ASSERT_BSONOBJ_EQ(info.applyOpsEntries[1].operations[0], op2.toBSON());
}

TEST(TransactionOperationsTest, GetApplyOpsInfoRespectsOperationSizeLimit) {
    TransactionOperations ops;

    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op1.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                   // required for DurableReplOperation::serialize()
    op1.setObject(BSON("_id" << 1));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op2.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                   // required for DurableReplOperation::serialize()
    op2.setObject(BSON("_id" << 2));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op2));

    // Restrict each applyOps entry to holding at most one operation.
    auto info = ops.getApplyOpsInfo(
        /*oplogEntryCountLimit=*/100U,
        /*oplogEntrySizeLimitBytes=*/repl::DurableOplogEntry::getDurableReplOperationSize(op1) +
            TransactionOperations::ApplyOpsInfo::kBSONArrayElementOverhead,
        /*prepare=*/false);

    ASSERT_EQ(info.numberOfOplogSlotsRequired, 2U);
    ASSERT_EQ(info.applyOpsEntries.size(), 2U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    // Check first applyOps entry.
    ASSERT_EQ(info.applyOpsEntries[0].oplogSlotIndex, 0);
    ASSERT_EQ(info.applyOpsEntries[0].operations.size(), 1U);
    ASSERT_BSONOBJ_EQ(info.applyOpsEntries[0].operations[0], op1.toBSON());

    // Check second applyOps entry.
    ASSERT_EQ(info.applyOpsEntries[1].oplogSlotIndex, 1);
    ASSERT_EQ(info.applyOpsEntries[1].operations.size(), 1U);
    ASSERT_BSONOBJ_EQ(info.applyOpsEntries[1].operations[0], op2.toBSON());
}

TEST(TransactionOperationsTest, GetApplyOpsInfoAssignsPreImageSlotBeforeOperation) {
    TransactionOperations ops;

    // Setting the "needs retry image" flag on 'op' forces getApplyOpsInfo()
    // to request an additional slot, which will not be available due to an
    // insufficiently sized 'oplogSlots' array.
    TransactionOperations::TransactionOperation op;
    op.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
    op.setOpType(repl::OpTypeEnum::kInsert);  // required for DurableReplOperation::serialize()
    op.setNss(NamespaceString::createNamespaceString_forTest(
        "test.t"));                  // required for DurableReplOperation::serialize()
    op.setObject(BSON("_id" << 1));  // required for DurableReplOperation::serialize()
    ASSERT_OK(ops.addOperation(op));

    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);

    ASSERT_EQ(info.numberOfOplogSlotsRequired, 2U);
    ASSERT_EQ(info.applyOpsEntries.size(), 1U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 1U);
    ASSERT_FALSE(info.prepare);

    ASSERT_EQ(info.applyOpsEntries[0].oplogSlotIndex, 1);
    ASSERT_EQ(info.applyOpsEntries[0].operations.size(), 1U);
    ASSERT_BSONOBJ_EQ(info.applyOpsEntries[0].operations[0], op.toBSON());
}

TEST(TransactionOperationsTest, LogOplogEntriesDoesNothingOnEmptyOperations) {
    TransactionOperations ops;
    std::vector<OplogSlot> oplogSlots;
    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 0);
    ASSERT_EQ(info.applyOpsEntries.size(), 0);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    auto brokenLogApplyOpsFn = [](repl::MutableOplogEntry*,
                                  bool,
                                  bool,
                                  std::vector<StmtId>,
                                  WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
        FAIL("logApplyOps() should not be called");
        return repl::OpTime();
    };
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    auto numEntries = ops.logOplogEntries(oplogSlots,
                                          info,
                                          kWallClockTime,
                                          WriteUnitOfWork::kDontGroup,
                                          brokenLogApplyOpsFn,
                                          &imageToWrite);
    ASSERT_EQ(numEntries, 0);
}

TEST(TransactionOperationsTest, LogOplogEntriesSingleOperation) {
    TransactionOperations ops;

    // The Tenant ID contained in the generated applyOps oplog entry should match that
    // of the first operation.
    unittest::ServerParameterGuard featureFlagController("featureFlagRequireTenantID", true);
    unittest::ServerParameterGuard multitenancySupportController("multitenancySupport", true);
    auto tenant = TenantId(OID::gen());

    // Add a small operation. This should be packed into a single applyOps entry.
    TransactionOperations::TransactionOperation op;
    op.setOpType(repl::OpTypeEnum::kInsert);
    op.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op.setObject(BSON("_id" << 1 << "x" << 1));
    op.setTid(tenant);
    std::vector<StmtId> stmtIds = {1};
    op.setStatementIds(stmtIds);
    ASSERT_OK(ops.addOperation(op));

    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 1U);
    ASSERT_EQ(info.applyOpsEntries.size(), 1U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});

    // Check applyOps oplog entry to ensure it has all the basic details.
    auto logApplyOpsFn = [op,
                          oplogSlots](repl::MutableOplogEntry* entry,
                                      bool firstOp,
                                      bool lastOp,
                                      std::vector<StmtId> stmtIdsWritten,
                                      WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
        ASSERT(entry) << "tried to log null applyOps oplog entry";
        ASSERT_EQ(entry->getOpType(), repl::OpTypeEnum::kCommand);
        ASSERT_EQ(
            entry->getNss(),
            NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS());
        ASSERT_EQ(entry->getOpTime(), oplogSlots[0]);
        const auto& prevWriteOpTime = entry->getPrevWriteOpTimeInTransaction();
        ASSERT(prevWriteOpTime);
        ASSERT(prevWriteOpTime->isNull());
        ASSERT_EQ(entry->getWallClockTime(), kWallClockTime);
        ASSERT_BSONOBJ_EQ(entry->getObject(), BSON("applyOps" << BSON_ARRAY(op.toBSON())));
        auto tid = entry->getTid();
        ASSERT(tid) << entry->toBSON();
        ASSERT_EQ(*tid, *op.getTid());

        ASSERT(firstOp);
        ASSERT(lastOp);
        ASSERT_EQ(stmtIdsWritten.size(), 1U);
        ASSERT_EQ(stmtIdsWritten.front(), op.getStatementIds()[0]);
        return oplogSlots.back();
    };
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    auto numEntries = ops.logOplogEntries(oplogSlots,
                                          info,
                                          kWallClockTime,
                                          WriteUnitOfWork::kDontGroup,
                                          logApplyOpsFn,
                                          &imageToWrite);
    ASSERT_EQ(numEntries, 1U);
}

TEST(TransactionOperationsTest, LogOplogEntriesMultipleOperationsCommitUnpreparedTransaction) {
    TransactionOperations ops;

    // The Tenant ID contained in the generated applyOps oplog entry should match that
    // of the first operation.
    unittest::ServerParameterGuard featureFlagController("featureFlagRequireTenantID", true);
    unittest::ServerParameterGuard multitenancySupportController("multitenancySupport", true);
    auto tenant = TenantId(OID::gen());

    // Add three operations. This helps us check fields for the first, middle, and last entries
    // in the applyOps chain.
    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);
    op1.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op1.setObject(BSON("_id" << 1 << "x" << 1));
    op1.setTid(tenant);
    std::vector<StmtId> stmtIds1 = {1};
    op1.setStatementIds(stmtIds1);
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);
    op2.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op2.setObject(BSON("_id" << 2 << "x" << 2));
    op2.setTid(tenant);
    std::vector<StmtId> stmtIds2 = {2};
    op2.setStatementIds(stmtIds2);
    ASSERT_OK(ops.addOperation(op2));

    TransactionOperations::TransactionOperation op3;
    op3.setOpType(repl::OpTypeEnum::kInsert);
    op3.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op3.setObject(BSON("_id" << 3 << "x" << 3));
    op3.setTid(tenant);
    std::vector<StmtId> stmtIds3 = {3};
    op3.setStatementIds(stmtIds3);
    ASSERT_OK(ops.addOperation(op3));

    auto info = ops.getApplyOpsInfo(1U,  // one operation per applyOps entry
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 3U);
    ASSERT_EQ(info.applyOpsEntries.size(), 3U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(2, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(3, 0), /*term=*/1LL});

    // Check applyOps oplog entry to ensure it has all the basic details.
    std::size_t numEntriesLogged = 0;
    auto logApplyOpsFn = [&numEntriesLogged, oplogSlots, ops](
                             repl::MutableOplogEntry* entry,
                             bool firstOp,
                             bool lastOp,
                             std::vector<StmtId> stmtIdsWritten,
                             WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
        ASSERT(entry) << "tried to log null applyOps oplog entry";

        ASSERT_EQ(entry->getOpType(), repl::OpTypeEnum::kCommand);
        ASSERT_EQ(
            entry->getNss(),
            NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS());

        auto expectedOpTime = oplogSlots[numEntriesLogged];
        ASSERT_EQ(entry->getOpTime(), expectedOpTime);

        // First entry should have a null op time. Following entries should match the result
        // this function returned previously.
        const auto& prevWriteOpTime = entry->getPrevWriteOpTimeInTransaction();
        ASSERT(prevWriteOpTime);
        if (numEntriesLogged == 0) {
            ASSERT(prevWriteOpTime->isNull());
        } else {
            ASSERT_EQ(*prevWriteOpTime, oplogSlots[numEntriesLogged - 1]);
        }

        ASSERT_EQ(entry->getWallClockTime(), kWallClockTime);

        auto op = ops.getOperationsForTest()[numEntriesLogged];
        auto tid = entry->getTid();
        ASSERT(tid) << entry->toBSON();
        ASSERT_EQ(*tid, *op.getTid());

        // Last operation in applyOps chain is formatted slightly differently from
        // preceding entries.
        if (numEntriesLogged == (ops.numOperations() - 1U)) {
            ASSERT_BSONOBJ_EQ(entry->getObject(),
                              BSON("applyOps" << BSON_ARRAY(op.toBSON()) << "count"
                                              << static_cast<long long>(ops.numOperations())));
        } else {
            ASSERT_BSONOBJ_EQ(entry->getObject(),
                              BSON("applyOps" << BSON_ARRAY(op.toBSON()) << "partialTxn" << true));
        }

        if (numEntriesLogged == 0) {
            ASSERT(firstOp);
            ASSERT_FALSE(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 0);
        } else if (numEntriesLogged < (ops.numOperations() - 1U)) {
            ASSERT_FALSE(firstOp);
            ASSERT_FALSE(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 0);
        } else {
            ASSERT_FALSE(firstOp);
            ASSERT(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 3U);
            ASSERT_EQ(stmtIdsWritten[0], ops.getOperationsForTest()[0].getStatementIds()[0]);
            ASSERT_EQ(stmtIdsWritten[1], ops.getOperationsForTest()[1].getStatementIds()[0]);
            ASSERT_EQ(stmtIdsWritten[2], ops.getOperationsForTest()[2].getStatementIds()[0]);
        }

        numEntriesLogged++;
        return expectedOpTime;
    };
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    auto numEntries = ops.logOplogEntries(oplogSlots,
                                          info,
                                          kWallClockTime,
                                          WriteUnitOfWork::kDontGroup,
                                          logApplyOpsFn,
                                          &imageToWrite);
    ASSERT_EQ(numEntries, 3U);
}

TEST(TransactionOperationsTest, LogOplogEntriesMultipleOperationsPreparedTransaction) {
    TransactionOperations ops;

    // The Tenant ID contained in the generated applyOps oplog entry should match that
    // of the first operation.
    unittest::ServerParameterGuard featureFlagController("featureFlagRequireTenantID", true);
    unittest::ServerParameterGuard multitenancySupportController("multitenancySupport", true);
    auto tenant = TenantId(OID::gen());

    // Add three operations. This helps us check fields for the first, middle, and last entries
    // in the applyOps chain.
    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);
    op1.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op1.setObject(BSON("_id" << 1 << "x" << 1));
    op1.setTid(tenant);
    std::vector<StmtId> stmtIds1 = {1};
    op1.setStatementIds(stmtIds1);
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);
    op2.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op2.setObject(BSON("_id" << 2 << "x" << 2));
    op2.setTid(tenant);
    std::vector<StmtId> stmtIds2 = {2};
    op2.setStatementIds(stmtIds2);
    ASSERT_OK(ops.addOperation(op2));

    TransactionOperations::TransactionOperation op3;
    op3.setOpType(repl::OpTypeEnum::kInsert);
    op3.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op3.setObject(BSON("_id" << 3 << "x" << 3));
    op3.setTid(tenant);
    std::vector<StmtId> stmtIds3 = {3};
    op3.setStatementIds(stmtIds3);
    ASSERT_OK(ops.addOperation(op3));

    auto info = ops.getApplyOpsInfo(1U,  // one operation per applyOps entry
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/true);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 3U);
    ASSERT_EQ(info.applyOpsEntries.size(), 3U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(2, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(3, 0), /*term=*/1LL});

    // Check applyOps oplog entry to ensure it has all the basic details.
    std::size_t numEntriesLogged = 0;
    auto logApplyOpsFn = [&numEntriesLogged, oplogSlots, ops](
                             repl::MutableOplogEntry* entry,
                             bool firstOp,
                             bool lastOp,
                             std::vector<StmtId> stmtIdsWritten,
                             WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
        ASSERT(entry) << "tried to log null applyOps oplog entry";
        ASSERT_EQ(entry->getOpType(), repl::OpTypeEnum::kCommand);
        ASSERT_EQ(
            entry->getNss(),
            NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS());

        auto expectedOpTime = oplogSlots[numEntriesLogged];
        ASSERT_EQ(entry->getOpTime(), expectedOpTime);

        ASSERT_EQ(entry->getOpTime(), oplogSlots[numEntriesLogged]);

        // First entry should have a null op time. Following entries should match the result
        // this function returned previously.
        const auto& prevWriteOpTime = entry->getPrevWriteOpTimeInTransaction();
        ASSERT(prevWriteOpTime);
        if (numEntriesLogged == 0) {
            ASSERT(prevWriteOpTime->isNull());
        } else {
            ASSERT_EQ(*prevWriteOpTime, oplogSlots[numEntriesLogged - 1]);
        }

        ASSERT_EQ(entry->getWallClockTime(), kWallClockTime);

        auto op = ops.getOperationsForTest()[numEntriesLogged];
        auto tid = entry->getTid();
        ASSERT(tid) << entry->toBSON();
        ASSERT_EQ(*tid, *op.getTid());

        // Last operation in applyOps chain is formatted slightly differently from
        // preceding entries.
        if (numEntriesLogged == (ops.numOperations() - 1U)) {
            ASSERT_BSONOBJ_EQ(entry->getObject(),
                              BSON("applyOps" << BSON_ARRAY(op.toBSON()) << "prepare" << true
                                              << "count"
                                              << static_cast<long long>(ops.numOperations())));
        } else {
            ASSERT_BSONOBJ_EQ(entry->getObject(),
                              BSON("applyOps" << BSON_ARRAY(op.toBSON()) << "partialTxn" << true));
        }

        if (numEntriesLogged == 0) {
            ASSERT(firstOp);
            ASSERT_FALSE(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 0);
        } else if (numEntriesLogged < (ops.numOperations() - 1U)) {
            ASSERT_FALSE(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 0);
        } else {
            ASSERT_FALSE(firstOp);
            ASSERT(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 3U);
            ASSERT_EQ(stmtIdsWritten[0], ops.getOperationsForTest()[0].getStatementIds()[0]);
            ASSERT_EQ(stmtIdsWritten[1], ops.getOperationsForTest()[1].getStatementIds()[0]);
            ASSERT_EQ(stmtIdsWritten[2], ops.getOperationsForTest()[2].getStatementIds()[0]);
        }

        numEntriesLogged++;
        return expectedOpTime;
    };
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    auto numEntries = ops.logOplogEntries(oplogSlots,
                                          info,
                                          kWallClockTime,
                                          WriteUnitOfWork::kDontGroup,
                                          logApplyOpsFn,
                                          &imageToWrite);
    ASSERT_EQ(numEntries, 3U);
}

TEST(TransactionOperationsTest, LogOplogEntriesMultipleOperationsRetryableWrite) {
    TransactionOperations ops;

    // The Tenant ID contained in the generated applyOps oplog entry should match that
    // of the first operation.
    unittest::ServerParameterGuard featureFlagController("featureFlagRequireTenantID", true);
    unittest::ServerParameterGuard multitenancySupportController("multitenancySupport", true);
    auto tenant = TenantId(OID::gen());

    // Add three operations. This helps us check fields for the first, middle, and last entries
    // in the applyOps chain.
    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);
    op1.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op1.setObject(BSON("_id" << 1 << "x" << 1));
    op1.setTid(tenant);
    std::vector<StmtId> stmtIds1 = {1};
    op1.setStatementIds(stmtIds1);
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);
    op2.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op2.setObject(BSON("_id" << 2 << "x" << 2));
    op2.setTid(tenant);
    std::vector<StmtId> stmtIds2 = {2};
    op2.setStatementIds(stmtIds2);
    ASSERT_OK(ops.addOperation(op2));

    TransactionOperations::TransactionOperation op3;
    op3.setOpType(repl::OpTypeEnum::kInsert);
    op3.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op3.setObject(BSON("_id" << 3 << "x" << 3));
    op3.setTid(tenant);
    std::vector<StmtId> stmtIds3 = {3};
    op3.setStatementIds(stmtIds3);
    ASSERT_OK(ops.addOperation(op3));

    auto info = ops.getApplyOpsInfo(1U,  // one operation per applyOps entry
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 3U);
    ASSERT_EQ(info.applyOpsEntries.size(), 3U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(2, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(3, 0), /*term=*/1LL});

    // Check applyOps oplog entry to ensure it has all the basic details.
    std::size_t numEntriesLogged = 0;
    auto logApplyOpsFn = [&numEntriesLogged, oplogSlots, ops](
                             repl::MutableOplogEntry* entry,
                             bool firstOp,
                             bool lastOp,
                             std::vector<StmtId> stmtIdsWritten,
                             WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
        ASSERT(entry) << "tried to log null applyOps oplog entry";

        ASSERT_EQ(entry->getOpType(), repl::OpTypeEnum::kCommand);
        ASSERT_EQ(
            entry->getNss(),
            NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS());

        auto expectedOpTime = oplogSlots[numEntriesLogged];
        ASSERT_EQ(entry->getOpTime(), expectedOpTime);

        // First entry should have a null op time. Following entries should match the result
        // this function returned previously.
        const auto& prevWriteOpTime = entry->getPrevWriteOpTimeInTransaction();
        ASSERT(prevWriteOpTime);
        if (numEntriesLogged == 0) {
            ASSERT(prevWriteOpTime->isNull());
        } else {
            ASSERT_EQ(*prevWriteOpTime, oplogSlots[numEntriesLogged - 1]);
        }

        ASSERT_EQ(entry->getWallClockTime(), kWallClockTime);

        auto op = ops.getOperationsForTest()[numEntriesLogged];
        auto tid = entry->getTid();
        ASSERT(tid) << entry->toBSON();
        ASSERT_EQ(*tid, *op.getTid());

        // We expect neither "count" nor "partialTxn", as these applyOps should not be treated as a
        // single atomic operation.
        ASSERT_BSONOBJ_EQ(entry->getObject(), BSON("applyOps" << BSON_ARRAY(op.toBSON())));

        // Statement ids should be present on all applyOps.
        if (numEntriesLogged == 0) {
            ASSERT(firstOp);
            ASSERT_FALSE(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 1U);
            ASSERT_EQ(stmtIdsWritten[0], ops.getOperationsForTest()[0].getStatementIds()[0]);
        } else if (numEntriesLogged < (ops.numOperations() - 1U)) {
            ASSERT_FALSE(firstOp);
            ASSERT_FALSE(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 1U);
            ASSERT_EQ(stmtIdsWritten[0], ops.getOperationsForTest()[1].getStatementIds()[0]);
        } else {
            ASSERT_FALSE(firstOp);
            ASSERT(lastOp);
            ASSERT_EQ(stmtIdsWritten.size(), 1U);
            ASSERT_EQ(stmtIdsWritten[0], ops.getOperationsForTest()[2].getStatementIds()[0]);
        }

        numEntriesLogged++;
        return expectedOpTime;
    };
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    auto numEntries = ops.logOplogEntries(oplogSlots,
                                          info,
                                          kWallClockTime,
                                          WriteUnitOfWork::kGroupForPossiblyRetryableOperations,
                                          logApplyOpsFn,
                                          &imageToWrite);
    ASSERT_EQ(numEntries, 3U);
}

DEATH_TEST(TransactionOperationsTestDeathTest,
           LogOplogEntriesInsufficientApplyOpsEntries,
           "Not enough \\\"applyOps\\\" entries") {
    TransactionOperations ops;
    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 0);
    ASSERT_EQ(info.applyOpsEntries.size(), 0);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    // Insert extra operation to ensure logOplogEntries() catches mismatch
    // between operations contained in 'ops' and 'ApplyOpsInfo::applyOpsEntries'.
    TransactionOperations::TransactionOperation op;
    ASSERT_OK(ops.addOperation(op));

    std::vector<OplogSlot> oplogSlots;
    // This should set off a tripwire assertion.
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    ops.logOplogEntries(oplogSlots,
                        info,
                        kWallClockTime,
                        WriteUnitOfWork::kDontGroup,
                        doNothingLogApplyOpsFn,
                        &imageToWrite);
}

// During normal operation, the grouping of operations passed to logOplogEntries()
// should have been sized appropriately by getApplyOpsInfo() to not exceed the
// internal limits for BSONObjBuilder and BSONArrayBuilder.
// In the unlikely case that we exceed the BSONObj limits, logOplogEntries() should throw
// a TransactionTooLarge exception.
TEST(TransactionOperationsTest,
     LogOplogEntriesThrowsTransactionToolargeIfSingleEntrySizeLimitExceeded) {
    TransactionOperations ops;

    // Add two large 15 MB operations.
    for (int i = 0; i < 2; i++) {
        TransactionOperations::TransactionOperation op;
        op.setOpType(repl::OpTypeEnum::kInsert);
        op.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
        op.setObject(BSON("_id" << i << "x" << std::string(15 * 1024 * 1024, 'x')));
        ASSERT_OK(ops.addOperation(op));
    }

    // Provide a size limit that is twice what the BSONObjBuilder can accommodate
    // to getApplyOps() so that both large operations will be allocated to the
    // same applyOps entry.
    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes * 2,
                                    /*prepare=*/false);
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 1U);
    ASSERT_EQ(info.applyOpsEntries.size(), 1U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 0);
    ASSERT_FALSE(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    ASSERT_THROWS(ops.logOplogEntries(oplogSlots,
                                      info,
                                      kWallClockTime,
                                      WriteUnitOfWork::kDontGroup,
                                      doNothingLogApplyOpsFn,
                                      &imageToWrite),
                  ExceptionFor<ErrorCodes::TransactionTooLarge>);
}

TEST(TransactionOperationsTest, LogOplogEntriesExtractsPreImage) {
    TransactionOperations ops;

    // Add a small operation with a pre images.
    TransactionOperations::TransactionOperation op;
    op.setOpType(repl::OpTypeEnum::kInsert);
    op.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op.setObject(BSON("_id" << 1 << "x" << 1));
    op.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
    op.setPreImage(BSON("_id" << 1));
    ASSERT_OK(ops.addOperation(op));

    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    // getApplyOpsInfos() expects 2 slots to be used because of pre/post images.
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 2U);
    ASSERT_EQ(info.applyOpsEntries.size(), 1U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 1U);
    ASSERT_FALSE(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(2, 0), /*term=*/1LL});

    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    auto writeOpTime = oplogSlots.back();
    auto logApplyOps = [writeOpTime](repl::MutableOplogEntry*,
                                     bool firstOp,
                                     bool lastOp,
                                     std::vector<StmtId> stmtIdsWritten,
                                     WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
        return writeOpTime;
    };
    ASSERT_EQ(ops.logOplogEntries(oplogSlots,
                                  info,
                                  kWallClockTime,
                                  WriteUnitOfWork::kDontGroup,
                                  logApplyOps,
                                  &imageToWrite),
              info.numberOfOplogSlotsRequired);

    // Check image bundle.
    // Timestamp in image bundle should be based on optime returned by 'logApplyOps'.
    ASSERT(imageToWrite);
    ASSERT(imageToWrite->imageKind == repl::RetryImageEnum::kPreImage);
    ASSERT_BSONOBJ_EQ(imageToWrite->imageDoc, op.getPreImage());
    ASSERT_EQ(imageToWrite->timestamp, writeOpTime.getTimestamp());
}

TEST(TransactionOperationsTest, LogOplogEntriesExtractsPostImage) {
    TransactionOperations ops;

    // Add a small operation with a pre images.
    TransactionOperations::TransactionOperation op;
    op.setOpType(repl::OpTypeEnum::kInsert);
    op.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op.setObject(BSON("_id" << 1 << "x" << 1));
    op.setNeedsRetryImage(repl::RetryImageEnum::kPostImage);
    op.setPostImage(BSON("_id" << 1));
    ASSERT_OK(ops.addOperation(op));

    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    // getApplyOpsInfos() expects 2 slots to be used because of pre/post images.
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 2U);
    ASSERT_EQ(info.applyOpsEntries.size(), 1U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 1U);
    ASSERT_FALSE(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(2, 0), /*term=*/1LL});

    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    auto writeOpTime = oplogSlots.back();
    auto logApplyOps = [writeOpTime](repl::MutableOplogEntry*,
                                     bool firstOp,
                                     bool lastOp,
                                     std::vector<StmtId> stmtIdsWritten,
                                     WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat) {
        return writeOpTime;
    };
    ASSERT_EQ(ops.logOplogEntries(oplogSlots,
                                  info,
                                  kWallClockTime,
                                  WriteUnitOfWork::kDontGroup,
                                  logApplyOps,
                                  &imageToWrite),
              info.numberOfOplogSlotsRequired);

    // Check image bundle.
    // Timestamp in image bundle should be based on optime returned by 'logApplyOps'.
    ASSERT(imageToWrite);
    ASSERT(imageToWrite->imageKind == repl::RetryImageEnum::kPostImage);
    ASSERT_BSONOBJ_EQ(imageToWrite->imageDoc, op.getPostImage());
    ASSERT_EQ(imageToWrite->timestamp, writeOpTime.getTimestamp());
}

// Refer to small transaction test case in retryable_findAndModify_validation.js.
TEST(TransactionOperationsTest, LogOplogEntriesMultiplePrePostImagesInSameEntry) {
    TransactionOperations ops;

    // Add two small operations with pre/post images.
    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);
    op1.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op1.setObject(BSON("_id" << 1 << "x" << 1));
    op1.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
    op1.setPreImage(BSON("_id" << 1));
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);
    op2.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op2.setObject(BSON("_id" << 2 << "x" << 2));
    op2.setNeedsRetryImage(repl::RetryImageEnum::kPostImage);
    op2.setPostImage(BSON("_id" << 2));
    ASSERT_OK(ops.addOperation(op2));

    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    // getApplyOpsInfos() expects 2 slots to be used because of pre/post images.
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 2U);
    ASSERT_EQ(info.applyOpsEntries.size(), 1U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 2U);
    ASSERT_FALSE(info.prepare);

    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(2, 0), /*term=*/1LL});

    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    ASSERT_THROWS_CODE(ops.logOplogEntries(oplogSlots,
                                           info,
                                           kWallClockTime,
                                           WriteUnitOfWork::kDontGroup,
                                           doNothingLogApplyOpsFn,
                                           &imageToWrite),
                       AssertionException,
                       6054001);
}

// Refer to large transaction test case in retryable_findAndModify_validation.js.
TEST(TransactionOperationsTest, LogOplogEntriesMultiplePrePostImagesInDifferentEntries) {
    TransactionOperations ops;

    // Add two large 15MB operations with pre/post images.
    TransactionOperations::TransactionOperation op1;
    op1.setOpType(repl::OpTypeEnum::kInsert);
    op1.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op1.setObject(BSON("_id" << 1 << "x" << std::string(15 * 1024 * 1024, 'x')));
    op1.setNeedsRetryImage(repl::RetryImageEnum::kPreImage);
    op1.setPreImage(BSON("_id" << 1));
    ASSERT_OK(ops.addOperation(op1));

    TransactionOperations::TransactionOperation op2;
    op2.setOpType(repl::OpTypeEnum::kInsert);
    op2.setNss(NamespaceString::createNamespaceString_forTest("test.t"));
    op2.setObject(BSON("_id" << 2 << "x" << std::string(15 * 1024 * 1024, 'x')));
    op2.setNeedsRetryImage(repl::RetryImageEnum::kPostImage);
    op2.setPostImage(BSON("_id" << 2));
    ASSERT_OK(ops.addOperation(op2));

    auto info = ops.getApplyOpsInfo(kOplogEntryCountLimit,
                                    kOplogEntrySizeLimitBytes,
                                    /*prepare=*/false);
    // getApplyOpsInfos() expects four slots to be used because of pre/post images and
    // multiple applyOps entries.
    ASSERT_EQ(info.numberOfOplogSlotsRequired, 4U);
    ASSERT_EQ(info.applyOpsEntries.size(), 2U);
    ASSERT_EQ(info.numOperationsWithNeedsRetryImage, 2U);
    ASSERT_FALSE(info.prepare);

    // Need four oplog slots because getApplyOpsInfo() assigned one for each
    // applyOps entry and one for each image.
    std::vector<OplogSlot> oplogSlots;
    oplogSlots.push_back(OplogSlot{Timestamp(1, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(2, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(3, 0), /*term=*/1LL});
    oplogSlots.push_back(OplogSlot{Timestamp(4, 0), /*term=*/1LL});

    boost::optional<TransactionOperations::TransactionOperation::ImageBundle> imageToWrite;
    ASSERT_THROWS_CODE(ops.logOplogEntries(oplogSlots,
                                           info,
                                           kWallClockTime,
                                           WriteUnitOfWork::kDontGroup,
                                           doNothingLogApplyOpsFn,
                                           &imageToWrite),
                       AssertionException,
                       6054002);
}

}  // namespace
}  // namespace mongo
