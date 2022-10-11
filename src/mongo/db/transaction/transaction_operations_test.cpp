/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/transaction/transaction_operations.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

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

    // The getMutableOperationsForTransactionParticipant() method supports integration with
    // existing TransactionParticipant usage and OpObserver interfaces.
    auto* mutableOps = ops.getMutableOperationsForTransactionParticipant();
    ASSERT_EQ(mutableOps->size(), ops.numOperations());
    std::size_t mutableOpsTotalOperationBytes = 0;
    for (const auto& mutableOp : *mutableOps) {
        mutableOpsTotalOperationBytes +=
            repl::DurableOplogEntry::getDurableReplOperationSize(mutableOp);
    }
    ASSERT_EQ(mutableOpsTotalOperationBytes, ops.getTotalOperationBytes());

    // Use clear() to reset container state.
    ops.clear();
    ASSERT(ops.isEmpty());
    ASSERT_EQ(ops.numOperations(), 0);
    ASSERT_EQ(ops.getTotalOperationBytes(), 0);
}

TEST(TransactionOperationsTest, AddTransactionFailsOnDuplicateStatementIds) {
    TransactionOperations::TransactionOperation op1;
    std::vector<StmtId> stmtIds1 = {1, 2, 3};
    op1.setStatementIds(stdx::variant<StmtId, std::vector<StmtId>>(stmtIds1));

    TransactionOperations::TransactionOperation op2;
    std::vector<StmtId> stmtIds2 = {3, 4, 5};
    op2.setStatementIds(stdx::variant<StmtId, std::vector<StmtId>>(stmtIds2));

    TransactionOperations ops;
    ASSERT_OK(ops.addOperation(op1));
    ASSERT_EQ(static_cast<ErrorCodes::Error>(5875600), ops.addOperation(op2));
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

}  // namespace
}  // namespace mongo
