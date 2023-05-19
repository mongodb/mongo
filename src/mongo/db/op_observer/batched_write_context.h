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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/transaction/transaction_operations.h"

namespace mongo {

/**
 * Group multiple writes into a single applyOps entry.
 */

/**
 * This class is a decoration on the OperationContext holding context of writes that are logically
 * related with each other. It can be used to stage writes belonging to the same WriteUnitOfWork or
 * multi-document transaction. Currently only supports batching deletes in a WriteUnitOfWork.
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

    /**
     * Holds oplog data for operations which have been applied in the current batched
     * write context.
     */
    BatchedOperations _batchedOperations;
};

}  // namespace mongo
