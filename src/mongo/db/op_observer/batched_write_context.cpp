// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/batched_write_context.h"

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <boost/optional/optional.hpp>

namespace mongo {
const OperationContext::Decoration<BatchedWriteContext> BatchedWriteContext::get =
    OperationContext::declareDecoration<BatchedWriteContext>();

BatchedWriteContext::BatchedWriteContext() {}

void BatchedWriteContext::assertNoMixedBatchedOps(bool isDDL) {
    if (isDDL) {
        tassert(12073500,
                "DDL operation should not be in a batched write group "
                "that already contains CRUD operations",
                _batchedOperations.numOperations() == 0);
        _ddlOperationOccurred = true;
    } else {
        tassert(12073501,
                "CRUD operation should not be added to a batched write group "
                "that already contains a DDL operation",
                !_ddlOperationOccurred);
    }
}

void BatchedWriteContext::addBatchedOperation(OperationContext* opCtx, BatchedOperation operation) {
    invariant(_batchWrites);
    assertNoMixedBatchedOps(/*isDDL=*/false);

    // Current support is limited to only insert, update, delete, and container operations. No
    // multi-doc transactions.
    invariant(operation.getOpType() == repl::OpTypeEnum::kDelete ||
              operation.getOpType() == repl::OpTypeEnum::kInsert ||
              operation.getOpType() == repl::OpTypeEnum::kUpdate ||
              operation.getOpType() == repl::OpTypeEnum::kContainerInsert ||
              operation.getOpType() == repl::OpTypeEnum::kContainerUpdate ||
              operation.getOpType() == repl::OpTypeEnum::kContainerDelete);
    invariant(!opCtx->inMultiDocumentTransaction());
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    if (_currentGroupRecordId) {
        // Stamp the operation with its record so the packer keeps a record's operations in one
        // entry.
        operation.setGroupRecordId(*_currentGroupRecordId);
        _hasAtomicOperationGroups = true;
    }
    invariantStatusOK(_batchedOperations.addOperation(std::move(operation)));
}

BatchedWriteContext::AtomicOperationGroup::AtomicOperationGroup(OperationContext* opCtx,
                                                                const RecordId& recordId)
    : _context(BatchedWriteContext::get(opCtx)) {
    _context._enterAtomicOperationGroup(recordId);
}

BatchedWriteContext::AtomicOperationGroup::~AtomicOperationGroup() {
    _context._leaveAtomicOperationGroup();
}

void BatchedWriteContext::_enterAtomicOperationGroup(const RecordId& recordId) {
    // Nesting is not supported: a group must be left before another is entered.
    invariant(!_currentGroupRecordId);
    _currentGroupRecordId = recordId;
}

void BatchedWriteContext::_leaveAtomicOperationGroup() {
    // Must be balanced with a preceding _enterAtomicOperationGroup().
    invariant(_currentGroupRecordId);
    _currentGroupRecordId = boost::none;
}

TransactionOperations* BatchedWriteContext::getBatchedOperations(OperationContext* opCtx) {
    invariant(_batchWrites);
    return &_batchedOperations;
}

void BatchedWriteContext::clearBatchedOperations(OperationContext* opCtx) {
    _batchedOperations.clear();
    _ddlOperationOccurred = false;
    _currentGroupRecordId = boost::none;
    _hasAtomicOperationGroups = false;
}

bool BatchedWriteContext::writesAreBatched() const {
    return _batchWrites;
}
void BatchedWriteContext::setWritesAreBatched(bool batched) {
    _batchWrites = batched;
}
}  // namespace mongo
