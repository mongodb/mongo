/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/db/index/skipped_record_tracker.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
static constexpr StringData kRecordIdField = "recordId"_sd;
}

SkippedRecordTracker::SkippedRecordTracker(IndexCatalogEntry* indexCatalogEntry)
    : SkippedRecordTracker(nullptr, indexCatalogEntry, boost::none) {}

SkippedRecordTracker::SkippedRecordTracker(OperationContext* opCtx,
                                           IndexCatalogEntry* indexCatalogEntry,
                                           boost::optional<StringData> ident)
    : _indexCatalogEntry(indexCatalogEntry) {
    if (!ident) {
        return;
    }

    // Only initialize the table when resuming an index build if an ident already exists. Otherwise,
    // lazily initialize table when we record the first document.
    _skippedRecordsTable =
        opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStoreFromExistingIdent(
            opCtx, ident.get());
}

void SkippedRecordTracker::finalizeTemporaryTable(OperationContext* opCtx,
                                                  TemporaryRecordStore::FinalizationAction action) {
    if (_skippedRecordsTable) {
        _skippedRecordsTable->finalizeTemporaryTable(opCtx, action);
    }
}

void SkippedRecordTracker::record(OperationContext* opCtx, const RecordId& recordId) {
    BSONObjBuilder builder;
    recordId.serializeToken(kRecordIdField, &builder);
    BSONObj toInsert = builder.obj();

    // Lazily initialize table when we record the first document.
    if (!_skippedRecordsTable) {
        _skippedRecordsTable =
            opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(opCtx);
    }

    writeConflictRetry(
        opCtx,
        "recordSkippedRecordTracker",
        NamespaceString::kIndexBuildEntryNamespace.ns(),
        [&]() {
            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(
                _skippedRecordsTable->rs()
                    ->insertRecord(opCtx, toInsert.objdata(), toInsert.objsize(), Timestamp::min())
                    .getStatus());
            wuow.commit();
        });
}

bool SkippedRecordTracker::areAllRecordsApplied(OperationContext* opCtx) const {
    if (!_skippedRecordsTable) {
        return true;
    }
    auto cursor = _skippedRecordsTable->rs()->getCursor(opCtx);
    auto record = cursor->next();

    // The table is empty only when all writes are applied.
    if (!record)
        return true;

    return false;
}

Status SkippedRecordTracker::retrySkippedRecords(OperationContext* opCtx,
                                                 const CollectionPtr& collection) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));
    if (!_skippedRecordsTable) {
        return Status::OK();
    }

    InsertDeleteOptions options;
    collection->getIndexCatalog()->prepareInsertDeleteOptions(
        opCtx,
        _indexCatalogEntry->getNSSFromCatalog(opCtx),
        _indexCatalogEntry->descriptor(),
        &options);
    options.fromIndexBuilder = true;

    // This should only be called when constraints are being enforced, on a primary. It does not
    // make sense, nor is it necessary for this to be called on a secondary.
    invariant(options.getKeysMode == IndexAccessMethod::GetKeysMode::kEnforceConstraints);

    static const char* curopMessage = "Index Build: retrying skipped records";
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(
            CurOp::get(opCtx)->setProgress_inlock(curopMessage, _skippedRecordCounter.load(), 1));
    }

    SharedBufferFragmentBuilder pooledBuilder(KeyString::HeapBuilder::kHeapAllocatorDefaultBytes);
    auto recordStore = _skippedRecordsTable->rs();
    auto cursor = recordStore->getCursor(opCtx);
    int resolved = 0;
    while (auto record = cursor->next()) {
        const BSONObj doc = record->data.toBson();

        // This is the RecordId of the skipped record from the collection.
        RecordId skippedRecordId = RecordId::deserializeToken(doc[kRecordIdField]);
        WriteUnitOfWork wuow(opCtx);

        // If the record still exists, get a potentially new version of the document to index.
        auto collCursor = collection->getCursor(opCtx);
        auto skippedRecord = collCursor->seekExact(skippedRecordId);
        if (skippedRecord) {
            const auto skippedDoc = skippedRecord->data.toBson();
            LOGV2_DEBUG(23882,
                        2,
                        "reapplying skipped RecordID {skippedRecordId}: {skippedDoc}",
                        "skippedRecordId"_attr = skippedRecordId,
                        "skippedDoc"_attr = skippedDoc);

            try {
                // Because constraint enforcement is set, this will throw if there are any indexing
                // errors, instead of writing back to the skipped records table, which would
                // normally happen if constraints were relaxed.
                auto status = _indexCatalogEntry->accessMethod()->insert(opCtx,
                                                                         pooledBuilder,
                                                                         collection,
                                                                         skippedDoc,
                                                                         skippedRecordId,
                                                                         options,
                                                                         nullptr,
                                                                         nullptr);
                if (!status.isOK()) {
                    return status;
                }
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        }

        // Delete the record so that it is not applied more than once.
        recordStore->deleteRecord(opCtx, record->id);

        cursor->save();
        wuow.commit();
        cursor->restore();

        progress->hit();
        resolved++;
    }
    progress->finished();

    int logLevel = (resolved > 0) ? 0 : 1;
    LOGV2_DEBUG(23883,
                logLevel,
                "index build: reapplied {resolved} skipped records for index: "
                "{indexCatalogEntry_descriptor_indexName}",
                "resolved"_attr = resolved,
                "indexCatalogEntry_descriptor_indexName"_attr =
                    _indexCatalogEntry->descriptor()->indexName());
    return Status::OK();
}

}  // namespace mongo
