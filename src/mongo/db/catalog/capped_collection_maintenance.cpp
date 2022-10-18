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

#include "mongo/db/catalog/capped_collection_maintenance.h"

#include "mongo/db/op_observer/op_observer.h"

namespace mongo {
namespace collection_internal {
namespace {

class CappedDeleteSideTxn {
public:
    CappedDeleteSideTxn(OperationContext* opCtx) : _opCtx(opCtx) {
        _originalRecoveryUnit = _opCtx->releaseRecoveryUnit().release();
        invariant(_originalRecoveryUnit);
        _originalRecoveryUnitState = _opCtx->setRecoveryUnit(
            std::unique_ptr<RecoveryUnit>(
                _opCtx->getServiceContext()->getStorageEngine()->newRecoveryUnit()),
            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    }

    ~CappedDeleteSideTxn() {
        _opCtx->setRecoveryUnit(std::unique_ptr<RecoveryUnit>(_originalRecoveryUnit),
                                _originalRecoveryUnitState);
    }

private:
    OperationContext* const _opCtx;
    RecoveryUnit* _originalRecoveryUnit;
    WriteUnitOfWork::RecoveryUnitState _originalRecoveryUnitState;
};

struct CappedCollectionState {
    // For capped deletes performed on collections where 'needsCappedLock' is false, the mutex below
    // protects 'cappedFirstRecord'. Otherwise, when 'needsCappedLock' is true, the exclusive
    // metadata resource protects 'cappedFirstRecord'.
    Mutex cappedFirstRecordMutex =
        MONGO_MAKE_LATCH("CappedCollectionState::cappedFirstRecordMutex");
    RecordId cappedFirstRecord;
};

const auto cappedCollectionState =
    SharedCollectionDecorations::declareDecoration<CappedCollectionState>();

}  // namespace

void cappedDeleteUntilBelowConfiguredMaximum(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const RecordId& justInserted) {
    if (!collection->isCappedAndNeedsDelete(opCtx))
        return;

    // Secondaries only delete from capped collections via oplog application when there are explicit
    // delete oplog entries
    if (!opCtx->isEnforcingConstraints())
        return;

    const auto& nss = collection->ns();
    const auto& uuid = collection->uuid();
    auto& ccs = cappedCollectionState(*collection->getSharedDecorations());

    stdx::unique_lock<Latch> cappedFirstRecordMutex(ccs.cappedFirstRecordMutex, stdx::defer_lock);

    if (collection->needsCappedLock()) {
        // As capped deletes can be part of a larger WriteUnitOfWork, we need a way to protect
        // 'cappedFirstRecord' until the outermost WriteUnitOfWork commits or aborts. Locking the
        // metadata resource exclusively on the collection gives us that guarantee as it uses
        // two-phase locking semantics.
        invariant(opCtx->lockState()->getLockMode(ResourceId(RESOURCE_METADATA, nss.ns())) ==
                  MODE_X);
    } else {
        // Capped deletes not performed under the capped lock need the 'cappedFirstRecordMutex'
        // mutex.
        cappedFirstRecordMutex.lock();
    }

    boost::optional<CappedDeleteSideTxn> cappedDeleteSideTxn;
    if (!collection->needsCappedLock()) {
        // Any capped deletes not performed under the capped lock need to commit the innermost
        // WriteUnitOfWork while 'cappedFirstRecordMutex' is locked.
        cappedDeleteSideTxn.emplace(opCtx);
    }

    const long long currentDataSize = collection->dataSize(opCtx);
    const long long currentNumRecords = collection->numRecords(opCtx);

    const auto cappedMaxSize = collection->getCollectionOptions().cappedSize;
    const long long sizeOverCap =
        (currentDataSize > cappedMaxSize) ? currentDataSize - cappedMaxSize : 0;

    const auto cappedMaxDocs = collection->getCollectionOptions().cappedMaxDocs;
    const long long docsOverCap = (cappedMaxDocs != 0 && currentNumRecords > cappedMaxDocs)
        ? currentNumRecords - cappedMaxDocs
        : 0;

    long long sizeSaved = 0;
    long long docsRemoved = 0;

    WriteUnitOfWork wuow(opCtx);

    boost::optional<Record> record;
    auto cursor = collection->getCursor(opCtx, /*forward=*/true);

    // If the next RecordId to be deleted is known, navigate to it using seekNear(). Using a cursor
    // and advancing it to the first element by calling next() will be slow for capped collections
    // on particular storage engines, such as WiredTiger. In WiredTiger, there may be many
    // tombstones (invisible deleted records) to traverse at the beginning of the table.
    if (!ccs.cappedFirstRecord.isNull()) {
        // Use seekNear instead of seekExact. If this node steps down and a new primary starts
        // deleting capped documents then this node's cached record will become stale. If this node
        // steps up again afterwards, then the cached record will be an already deleted document.
        record = cursor->seekNear(ccs.cappedFirstRecord);
    } else {
        record = cursor->next();
    }

    while (sizeSaved < sizeOverCap || docsRemoved < docsOverCap) {
        if (!record) {
            break;
        }

        if (record->id == justInserted) {
            // We're prohibited from deleting what was just inserted.
            break;
        }

        docsRemoved++;
        sizeSaved += record->data.size();

        BSONObj doc = record->data.toBson();
        if (nss.isReplicated()) {
            OpObserver* opObserver = opCtx->getServiceContext()->getOpObserver();
            opObserver->aboutToDelete(opCtx, nss, uuid, doc);

            OplogDeleteEntryArgs args;
            // Explicitly setting values despite them being the defaults.
            args.deletedDoc = nullptr;
            args.fromMigrate = false;

            // If collection has change stream pre-/post-images enabled, pass the 'deletedDoc' for
            // writing it in the pre-images collection.
            if (collection->isChangeStreamPreAndPostImagesEnabled()) {
                args.deletedDoc = &doc;
                args.changeStreamPreAndPostImagesEnabledForCollection = true;
            }

            // Reserves an optime for the deletion and sets the timestamp for future writes.
            opObserver->onDelete(opCtx, nss, uuid, kUninitializedStmtId, args);
        }

        int64_t unusedKeysDeleted = 0;
        collection->getIndexCatalog()->unindexRecord(opCtx,
                                                     collection,
                                                     doc,
                                                     record->id,
                                                     /*logIfError=*/false,
                                                     &unusedKeysDeleted);

        // We're about to delete the record our cursor is positioned on, so advance the cursor.
        RecordId toDelete = std::move(record->id);
        record = cursor->next();

        collection->getRecordStore()->deleteRecord(opCtx, toDelete);
    }

    if (cappedDeleteSideTxn) {
        // Save the RecordId of the next record to be deleted, if it exists.
        if (!record) {
            ccs.cappedFirstRecord = RecordId();
        } else {
            ccs.cappedFirstRecord = std::move(record->id);
        }
    } else {
        // Update the next record to be deleted. The next record must exist as we're using the same
        // snapshot the insert was performed on and we can't delete newly inserted records.
        invariant(record);
        opCtx->recoveryUnit()->onCommit(
            [&ccs, recordId = std::move(record->id)](boost::optional<Timestamp>) {
                ccs.cappedFirstRecord = std::move(recordId);
            });
    }

    wuow.commit();
}

void cappedTruncateAfter(OperationContext* opCtx,
                         const CollectionPtr& collection,
                         const RecordId& end,
                         bool inclusive) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));
    invariant(collection->isCapped());
    invariant(collection->getIndexCatalog()->numIndexesInProgress() == 0);

    collection->getRecordStore()->cappedTruncateAfter(
        opCtx, end, inclusive, [&](OperationContext* opCtx, const RecordId& loc, RecordData data) {
            BSONObj doc = data.releaseToBson();
            int64_t* const nullKeysDeleted = nullptr;
            collection->getIndexCatalog()->unindexRecord(
                opCtx, collection, doc, loc, false, nullKeysDeleted);

            // We are not capturing and reporting to OpDebug the 'keysDeleted' by unindexRecord().
            // It is questionable whether reporting will add diagnostic value to users and may
            // instead be confusing as it depends on our internal capped collection document removal
            // strategy. We can consider adding either keysDeleted or a new metric reporting
            // document removal if justified by user demand.
        });
}

}  // namespace collection_internal
}  // namespace mongo
