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

#include "mongo/db/op_observer/batched_write_context.h"

#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <boost/optional/optional.hpp>

namespace mongo {
const OperationContext::Decoration<BatchedWriteContext> BatchedWriteContext::get =
    OperationContext::declareDecoration<BatchedWriteContext>();

BatchedWriteContext::BatchedWriteContext() {}

void BatchedWriteContext::addBatchedOperation(OperationContext* opCtx,
                                              const BatchedOperation& operation) {
    invariant(_batchWrites);

    // Current support is limited to only insert, update, delete, container insert, and container
    // delete operations. No change stream pre-images, no multi-doc transactions.
    invariant(operation.getOpType() == repl::OpTypeEnum::kDelete ||
              operation.getOpType() == repl::OpTypeEnum::kInsert ||
              operation.getOpType() == repl::OpTypeEnum::kUpdate ||
              operation.getOpType() == repl::OpTypeEnum::kContainerInsert ||
              operation.getOpType() == repl::OpTypeEnum::kContainerDelete);
    invariant(operation.getChangeStreamPreImageRecordingMode() ==
              repl::ReplOperation::ChangeStreamPreImageRecordingMode::kOff);
    invariant(!opCtx->inMultiDocumentTransaction());
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    invariantStatusOK(_batchedOperations.addOperation(operation));
}

TransactionOperations* BatchedWriteContext::getBatchedOperations(OperationContext* opCtx) {
    invariant(_batchWrites);
    return &_batchedOperations;
}

void BatchedWriteContext::clearBatchedOperations(OperationContext* opCtx) {
    _batchedOperations.clear();
    _defaultFromMigrate = false;
}

bool BatchedWriteContext::writesAreBatched() const {
    return _batchWrites;
}
void BatchedWriteContext::setWritesAreBatched(bool batched) {
    _batchWrites = batched;
}

void BatchedWriteContext::setDefaultFromMigrate(bool defaultFromMigrate) {
    invariant(_defaultFromMigrate == defaultFromMigrate || _batchedOperations.isEmpty());
    _defaultFromMigrate = defaultFromMigrate;
}


}  // namespace mongo
