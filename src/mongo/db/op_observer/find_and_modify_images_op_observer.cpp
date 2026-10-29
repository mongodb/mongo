// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/op_observer/find_and_modify_images_op_observer.h"

#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/shard_catalog/document_validation.h"
#include "mongo/db/shard_role/shard_role.h"  // for acquireCollection() and CollectionAcquisitionRequest
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/util/assert_util.h"

#include <string>

namespace mongo {
namespace {

/**
 * Writes temporary documents for retryable findAndModify commands to the
 * side collection (config.image_collection).
 *
 * In server version 7.0 and earlier, this behavior used to be configurable
 * through the server parameter storeFindAndModifyImagesInSideCollection.
 * See SERVER-59443.
 */
void writeToImageCollectionIfNeeded(OperationContext* opCtx, OpStateAccumulator* opAccumulator) {
    auto& rss = rss::ReplicatedStorageService::get(opCtx);
    if (!rss.getPersistenceProvider().supportsFindAndModifyImageCollection()) {
        return;
    }

    if (!opAccumulator) {
        return;
    }

    if (!opAccumulator->retryableFindAndModifyImageToWrite) {
        return;
    }

    const auto& sessionId = *opCtx->getLogicalSessionId();
    const auto& imageToWrite = *opAccumulator->retryableFindAndModifyImageToWrite;

    repl::ImageEntry imageEntry;
    imageEntry.set_id(sessionId);
    imageEntry.setTxnNumber(opCtx->getTxnNumber().value());
    imageEntry.setTs(imageToWrite.timestamp);
    imageEntry.setImageKind(imageToWrite.imageKind);
    imageEntry.setImage(imageToWrite.imageDoc);

    DisableDocumentValidation documentValidationDisabler(
        opCtx, DocumentValidationSettings::kDisableInternalValidation);

    // In practice, this lock acquisition on kConfigImagesNamespace cannot block. The only time a
    // stronger lock acquisition is taken on this namespace is during step up to create the
    // collection.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));
    auto collection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::kConfigImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);
    auto curOp = CurOp::get(opCtx);
    const auto existingNs = curOp->getNSS();
    UpdateResult res = Helpers::upsert(opCtx, collection, imageEntry.toBSON());
    {
        std::lock_guard<Client> clientLock(*opCtx->getClient());
        curOp->setNS(clientLock, existingNs);
    }

    invariant(res.numDocsModified == 1 || !res.upsertedId.isEmpty(),
              str::stream() << "NumDocsModified: " << res.numDocsModified
                            << ". Upserted Id: " << res.upsertedId.toString());
}

}  // namespace

void FindAndModifyImagesOpObserver::onUpdate(OperationContext* opCtx,
                                             const OplogUpdateEntryArgs& args,
                                             OpStateAccumulator* opAccumulator) {
    writeToImageCollectionIfNeeded(opCtx, opAccumulator);
}

void FindAndModifyImagesOpObserver::onDelete(OperationContext* opCtx,
                                             const CollectionPtr& coll,
                                             StmtId stmtId,
                                             const BSONObj& doc,
                                             const DocumentKey& documentKey,
                                             const OplogDeleteEntryArgs& args,
                                             OpStateAccumulator* opAccumulator) {
    writeToImageCollectionIfNeeded(opCtx, opAccumulator);
}

void FindAndModifyImagesOpObserver::onUnpreparedTransactionCommit(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    OpStateAccumulator* opAccumulator) {
    writeToImageCollectionIfNeeded(opCtx, opAccumulator);
}

void FindAndModifyImagesOpObserver::onTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    size_t numberOfPrePostImagesToWrite,
    Date_t wallClockTime,
    OpStateAccumulator* opAccumulator) {
    writeToImageCollectionIfNeeded(opCtx, opAccumulator);
}

}  // namespace mongo
