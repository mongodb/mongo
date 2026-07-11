// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/transaction/transaction_operations.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Group multiple writes into a single applyOps entry.
 */

/**
 * This class is a decoration on the OperationContext holding context of writes that are logically
 * related with each other. It can be used to stage writes belonging to the same WriteUnitOfWork or
 * multi-document transaction.
 */
class BatchedWriteContext {
public:
    using BatchedOperation = TransactionOperations::TransactionOperation;
    using BatchedOperations = TransactionOperations;

    static const OperationContext::Decoration<BatchedWriteContext> get;

    BatchedWriteContext();

    // No copy and no move
    BatchedWriteContext(const BatchedWriteContext&) = delete;
    BatchedWriteContext(BatchedWriteContext&&) = delete;
    BatchedWriteContext& operator=(const BatchedWriteContext&) = delete;
    BatchedWriteContext& operator=(BatchedWriteContext&&) = delete;

    bool writesAreBatched() const;
    void setWritesAreBatched(bool batched);

    /**
     * Asserts that DDL and CRUD operations are not mixed in the same batched write group.
     * Must only be called when writesAreBatched() is true.
     */
    void assertNoMixedBatchedOps(bool isDDL);

    /**
     * Adds a stored operation to the list of stored operations for the current WUOW.
     * It is illegal to add operations outside of a WUOW.
     * The stored operations must generate an applyOps entry that's within the max BSON size.
     * Anything larger will throw a TransactionTooLarge exception at commit.
     */
    void addBatchedOperation(OperationContext* opCtx, const BatchedOperation& operation);

    /**
     * Returns a pointer to the stored operations for the current WUOW.
     */
    TransactionOperations* getBatchedOperations(OperationContext* opCtx);
    void clearBatchedOperations(OperationContext* opCtx);

private:
    // Whether batching writes is enabled.
    bool _batchWrites = false;
    // Whether a DDL operation has occurred in the current batched write group.
    bool _ddlOperationOccurred = false;

    /**
     * Holds oplog data for operations which have been applied in the current batched
     * write context.
     */
    BatchedOperations _batchedOperations;
};

}  // namespace mongo
