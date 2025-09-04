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
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"

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
void writeChangeStreamPreImagesForApplyOpsEntries(
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
void writeChangeStreamPreImagesForTransaction(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const std::vector<repl::ReplOperation>& operations,
    const OpObserver::ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    Date_t operationTime) {
    // This function must be called from an outer WriteUnitOfWork in order to be rolled back upon
    // reaching the exception.
    invariant(shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());

    auto applyOpsEntriesIt = applyOpsOperationAssignment.applyOpsEntries.begin();
    for (auto operationIter = operations.begin(); operationIter != operations.end();) {
        tassert(7831000,
                "Unexpected end of applyOps entries vector",
                applyOpsEntriesIt != applyOpsOperationAssignment.applyOpsEntries.end());
        const auto& applyOpsEntry = *applyOpsEntriesIt++;
        const auto operationSequenceEnd = operationIter + applyOpsEntry.operations.size();
        const auto& oplogSlot = reservedSlots[applyOpsEntry.oplogSlotIndex];
        writeChangeStreamPreImagesForApplyOpsEntries(
            opCtx, operationIter, operationSequenceEnd, oplogSlot.getTimestamp(), operationTime);
        operationIter = operationSequenceEnd;
    }
}

}  // namespace

void ChangeStreamPreImagesOpObserver::onUpdate(OperationContext* opCtx,
                                               const OplogUpdateEntryArgs& args,
                                               OpStateAccumulator* opAccumulator) {
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
    // 4. a request to update is not on a temporary resharding collection. This update request
    //    does not result in change streams events. Recording pre-images from temporary
    //    resharing collection could result in incorrect pre-image getting recorded due to the
    //    temporary resharding collection not being consistent until writes are blocked (initial
    //    sync mode application).
    const auto& opTimeBundle = opAccumulator->opTime;
    const auto& nss = args.coll->ns();
    if (args.updateArgs->changeStreamPreAndPostImagesEnabledForCollection &&
        !opTimeBundle.writeOpTime.isNull() &&
        args.updateArgs->source != OperationSource::kFromMigrate &&
        !nss.isTemporaryReshardingCollection()) {
        const auto& preImageDoc = args.updateArgs->preImageDoc;
        const auto uuid = args.coll->uuid();
        invariant(!preImageDoc.isEmpty(),
                  fmt::format("PreImage must be set when writing to change streams pre-images "
                              "collection for update on collection {} (UUID: {}) with optime {}",
                              nss.toStringForErrorMsg(),
                              uuid.toString(),
                              opTimeBundle.writeOpTime.toString()));

        ChangeStreamPreImageId id(uuid, opTimeBundle.writeOpTime.getTimestamp(), 0);
        ChangeStreamPreImage preImage(id, opTimeBundle.wallClockTime, preImageDoc);
        invariant(args.coll->ns().tenantId() == boost::none);
        writeChangeStreamPreImageEntry(opCtx, preImage);
    }
}

void ChangeStreamPreImagesOpObserver::onDelete(OperationContext* opCtx,
                                               const CollectionPtr& coll,
                                               StmtId stmtId,
                                               const BSONObj& doc,
                                               const DocumentKey& documentKey,
                                               const OplogDeleteEntryArgs& args,
                                               OpStateAccumulator* opAccumulator) {
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
    // 4. a request to delete is not on a temporary resharding collection. This delete request
    //    does not result in change streams events. Recording pre-images from temporary
    //    resharing collection could result in incorrect pre-image getting recorded due to the
    //    temporary resharding collection not being consistent until writes are blocked (initial
    //    sync mode application).
    const auto& opTimeBundle = opAccumulator->opTime;
    const auto& nss = coll->ns();
    if (args.changeStreamPreAndPostImagesEnabledForCollection &&
        !opTimeBundle.writeOpTime.isNull() && !args.fromMigrate &&
        !nss.isTemporaryReshardingCollection()) {
        const auto uuid = coll->uuid();
        invariant(
            !doc.isEmpty(),
            fmt::format("Deleted document must be set when writing to change streams pre-images "
                        "collection for update on collection {} (UUID: {}) with optime {}",
                        nss.toStringForErrorMsg(),
                        uuid.toString(),
                        opTimeBundle.writeOpTime.toString()));

        ChangeStreamPreImageId id(uuid, opTimeBundle.writeOpTime.getTimestamp(), 0);
        ChangeStreamPreImage preImage(id, opTimeBundle.wallClockTime, doc);

        invariant(nss.tenantId() == boost::none);
        writeChangeStreamPreImageEntry(opCtx, preImage);
    }
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
    writeChangeStreamPreImagesForTransaction(
        opCtx, reservedSlots, statements, applyOpsOperationAssignment, opTimeBundle.wallClockTime);
}

void ChangeStreamPreImagesOpObserver::preTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    Date_t wallClockTime) {
    const auto& statements = transactionOperations.getOperationsForOpObserver();
    writeChangeStreamPreImagesForTransaction(
        opCtx, reservedSlots, statements, applyOpsOperationAssignment, wallClockTime);
}

}  // namespace mongo
