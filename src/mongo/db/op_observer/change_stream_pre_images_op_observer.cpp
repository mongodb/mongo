/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/op_observer/change_stream_pre_images_op_observer.h"

#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/shard_role/transaction_resources.h"

#include <fmt/format.h>

namespace mongo {

namespace {

/**
 * Inserts the document into the pre-images collection.
 */
void writeChangeStreamPreImageEntry(OperationContext* opCtx, const ChangeStreamPreImage& preImage) {
    ChangeStreamPreImagesCollectionManager::get(opCtx).insertPreImage(opCtx, preImage);
}

/**
 * Writes pre-images for update/replace/delete operations packed into a single "applyOps" entry to
 * the change stream pre-images collection if required. The operations are defined by sequence
 * ['stmtBegin', 'stmtEnd'). 'applyOpsTimestamp' and 'operationTime' are the timestamp and the wall
 * clock time, respectively, of the "applyOps" entry. A pre-image is recorded for an operation only
 * if pre-images are enabled for the collection the operation is issued on.
 */
void writeChangeStreamPreImagesForApplyOpsEntry(
    OperationContext* opCtx,
    std::vector<repl::ReplOperation>::const_iterator stmtBegin,
    std::vector<repl::ReplOperation>::const_iterator stmtEnd,
    Timestamp applyOpsTimestamp,
    Date_t operationTime) {
    int64_t applyOpsIndex{0};
    for (auto stmtIterator = stmtBegin; stmtIterator != stmtEnd; ++stmtIterator) {
        auto& operation = *stmtIterator;
        if (operation.isChangeStreamPreImageRecordedInPreImagesCollection() &&
            !operation.getNss().isTemporaryReshardingCollection()) {
            invariant(operation.getUuid());
            invariant(!operation.getPreImage().isEmpty());
            invariant(operation.getTid() == boost::none);
            writeChangeStreamPreImageEntry(
                opCtx,
                ChangeStreamPreImage{
                    ChangeStreamPreImageId{*operation.getUuid(), applyOpsTimestamp, applyOpsIndex},
                    operationTime,
                    operation.getPreImage()});
        }
        ++applyOpsIndex;
    }
}

/**
 * Writes change stream pre-images for transaction 'operations'. The 'applyOpsOperationAssignment'
 * contains a representation of "applyOps" entries to be written for the transaction. The
 * 'operationTime' is wall clock time of the operations used for the pre-image documents.
 */
template <typename OplogSlotAccessorFn>
void writeChangeStreamPreImagesForApplyOpsChain(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& oplogSlots,
    OplogSlotAccessorFn&& getOplogSlot,
    const std::vector<repl::ReplOperation>& operations,
    const std::vector<TransactionOperations::ApplyOpsInfo::ApplyOpsEntry>& applyOpsEntries,
    Date_t operationTime) {
    // This function must be called from an outer WriteUnitOfWork in order to be rolled back upon
    // reaching the exception.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    auto i = 0;
    for (auto operationIter = operations.begin(); operationIter != operations.end(); i++) {
        auto applyOpsEntryIt = applyOpsEntries.begin() + i;
        tassert(7831000,
                "Unexpected end of applyOps entries vector",
                applyOpsEntryIt != applyOpsEntries.end());
        const auto& applyOpsEntry = *applyOpsEntryIt;
        const auto operationSequenceEnd = operationIter + applyOpsEntry.operations.size();
        const auto& oplogSlot = getOplogSlot(oplogSlots, applyOpsEntry, i);
        writeChangeStreamPreImagesForApplyOpsEntry(
            opCtx, operationIter, operationSequenceEnd, oplogSlot.getTimestamp(), operationTime);
        operationIter = operationSequenceEnd;
    }
}

void writeChangeStreamPreImagesForTransaction(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const std::vector<repl::ReplOperation>& operations,
    const std::vector<TransactionOperations::ApplyOpsInfo::ApplyOpsEntry>& applyOpsEntries,
    Date_t operationTime) {
    writeChangeStreamPreImagesForApplyOpsChain(
        opCtx,
        reservedSlots,
        [&](const std::vector<OplogSlot>& reservedSlots,
            const auto& applyOpsEntry,
            int i) -> const OplogSlot& { return reservedSlots[applyOpsEntry.oplogSlotIndex]; },
        operations,
        applyOpsEntries,
        operationTime);
}

void writeChangeStreamPreImageEntryIfNecessary(
    OperationContext* opCtx,
    OpStateAccumulator* opAccumulator,
    const NamespaceString& nss,
    const BSONObj& preImageDoc,
    UUID uuid,
    bool changeStreamPreAndPostImagesEnabledForCollection,
    bool fromMigrate) {
    if (!opAccumulator) {
        return;
    }

    // Write a pre-image to the change streams pre-images collection when following conditions
    // are met:
    // 1. The collection has 'changeStreamPreAndPostImages' enabled.
    // 2. The node wrote the oplog entry for the corresponding operation.
    // 3. The request to write the pre-image does not come from chunk-migrate event, i.e. source
    //    of the request is not 'fromMigrate'. The 'fromMigrate' events are filtered out by
    //    change streams and storing them in pre-image collection is redundant.
    // 4. a request to update/delete is not on a temporary resharding collection. This update/delete
    //    request does not result in change streams events. Recording pre-images from temporary
    //    resharing collection could result in incorrect pre-image getting recorded due to the
    //    temporary resharding collection not being consistent until writes are blocked (initial
    //    sync mode application).
    const auto& opTimeBundle = opAccumulator->opTime;
    if (changeStreamPreAndPostImagesEnabledForCollection && !opTimeBundle.writeOpTime.isNull() &&
        !fromMigrate && !nss.isTemporaryReshardingCollection()) {
        invariant(!preImageDoc.isEmpty(),
                  fmt::format("PreImage must be set when writing to change streams pre-images "
                              "collection for update on collection {} (UUID: {}) with optime {}",
                              nss.toStringForErrorMsg(),
                              uuid.toString(),
                              opTimeBundle.writeOpTime.toString()));

        ChangeStreamPreImageId id(uuid, opTimeBundle.writeOpTime.getTimestamp(), 0);
        ChangeStreamPreImage preImage(std::move(id), opTimeBundle.wallClockTime, preImageDoc);
        invariant(nss.tenantId() == boost::none);
        writeChangeStreamPreImageEntry(opCtx, preImage);
    }
}

}  // namespace

void ChangeStreamPreImagesOpObserver::onUpdate(OperationContext* opCtx,
                                               const OplogUpdateEntryArgs& args,
                                               OpStateAccumulator* opAccumulator) {
    writeChangeStreamPreImageEntryIfNecessary(
        opCtx,
        opAccumulator,
        args.coll->ns(),
        args.updateArgs->preImageDoc,
        args.coll->uuid(),
        args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection,
        args.updateArgs->source == OperationSource::kFromMigrate);
}

void ChangeStreamPreImagesOpObserver::onDelete(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               StmtId stmtId,
                                               const BSONObj& doc,
                                               const DocumentKey& documentKey,
                                               const OplogDeleteEntryArgs& args,
                                               OpStateAccumulator* opAccumulator) {
    writeChangeStreamPreImageEntryIfNecessary(opCtx,
                                              opAccumulator,
                                              coll->ns(),
                                              doc,
                                              coll->uuid(),
                                              args.changeStreamPreAndPostImagesEnabledForCollection,
                                              args.fromMigrate);
}

void ChangeStreamPreImagesOpObserver::onUnpreparedTransactionCommit(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    OpStateAccumulator* opAccumulator) {
    if (!opAccumulator) {
        return;
    }

    // Return early if the node did not write the oplog entry for the corresponding operation.
    // This check is slightly redundant given that we don't need to do this for prepared
    // transactions.
    const auto& opTimeBundle = opAccumulator->opTime;
    if (opTimeBundle.writeOpTime.isNull()) {
        return;
    }

    // Write change stream pre-images. At this point the pre-images will be written at the
    // transaction commit timestamp as driven (implicitly) by the last written "applyOps" oplog
    // entry.
    const auto& statements = transactionOperations.getOperationsForOpObserver();
    writeChangeStreamPreImagesForTransaction(opCtx,
                                             reservedSlots,
                                             statements,
                                             applyOpsOperationAssignment.applyOpsEntries,
                                             opTimeBundle.wallClockTime);
}

void ChangeStreamPreImagesOpObserver::preTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    Date_t wallClockTime) {
    const auto& statements = transactionOperations.getOperationsForOpObserver();
    writeChangeStreamPreImagesForTransaction(opCtx,
                                             reservedSlots,
                                             statements,
                                             applyOpsOperationAssignment.applyOpsEntries,
                                             wallClockTime);
}

void ChangeStreamPreImagesOpObserver::onBatchedWriteCommit(
    OperationContext* opCtx,
    WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
    OpStateAccumulator* opStateAccumulator) {
    if (!opStateAccumulator) {
        return;
    }

    auto& batchedWriteContext = BatchedWriteContext::get(opCtx);
    TransactionOperations* batchedOps = batchedWriteContext.getBatchedOperations(opCtx);
    const std::vector<repl::ReplOperation>& oplogEntries = batchedOps->getOperationsForOpObserver();

    if (batchedOps->isEmpty()) {
        return;
    } else if (batchedOps->numOperations() == 1) {
        auto op = oplogEntries.front();
        if (op.getOpType() == repl::OpTypeEnum::kUpdate ||
            op.getOpType() == repl::OpTypeEnum::kDelete) {
            writeChangeStreamPreImageEntryIfNecessary(
                opCtx,
                opStateAccumulator,
                op.getNss(),
                op.getPreImage(),
                op.getUuid().value(),
                op.isChangeStreamPreImageRecordedInPreImagesCollection(),
                op.getFromMigrate().value_or(false));
        }
        return;
    } else {
        writeChangeStreamPreImagesForApplyOpsChain(
            opCtx,
            opStateAccumulator->batchOpTimes,
            [&](const std::vector<OplogSlot>& batchOpTimes,
                const auto& applyOpsEntry,
                int i) -> const OplogSlot& {
                // OpTimes are inserted into opStateAccumulator->batchOpTimes in the same order as
                // opStateAccumulator->applyOpsEntries in OpObserverImpl::onBatchedWriteCommit
                return batchOpTimes[i];
            },
            oplogEntries,
            opStateAccumulator->applyOpsEntries,
            opStateAccumulator->opTime.wallClockTime);
    }
}

}  // namespace mongo
