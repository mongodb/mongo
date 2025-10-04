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

#include "mongo/db/op_observer/find_and_modify_images_op_observer.h"

#include "mongo/db/curop.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"  // for acquireCollection() and CollectionAcquisitionRequest
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/session/logical_session_id_gen.h"
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
void writeToImageCollection(OperationContext* opCtx, OpStateAccumulator* opAccumulator) {
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
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);
    auto curOp = CurOp::get(opCtx);
    const auto existingNs = curOp->getNSS();
    UpdateResult res = Helpers::upsert(opCtx, collection, imageEntry.toBSON());
    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
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
    writeToImageCollection(opCtx, opAccumulator);
}

void FindAndModifyImagesOpObserver::onDelete(OperationContext* opCtx,
                                             const CollectionPtr& coll,
                                             StmtId stmtId,
                                             const BSONObj& doc,
                                             const DocumentKey& documentKey,
                                             const OplogDeleteEntryArgs& args,
                                             OpStateAccumulator* opAccumulator) {
    writeToImageCollection(opCtx, opAccumulator);
}

void FindAndModifyImagesOpObserver::onUnpreparedTransactionCommit(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    OpStateAccumulator* opAccumulator) {
    writeToImageCollection(opCtx, opAccumulator);
}

void FindAndModifyImagesOpObserver::onTransactionPrepare(
    OperationContext* opCtx,
    const std::vector<OplogSlot>& reservedSlots,
    const TransactionOperations& transactionOperations,
    const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
    size_t numberOfPrePostImagesToWrite,
    Date_t wallClockTime,
    OpStateAccumulator* opAccumulator) {
    writeToImageCollection(opCtx, opAccumulator);
}

}  // namespace mongo
