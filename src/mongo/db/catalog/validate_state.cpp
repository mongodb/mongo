/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/catalog/validate_state.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_consistency.h"
#include "mongo/db/catalog/validate_adaptor.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/columns_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDuringYieldingLocksForValidation);
MONGO_FAIL_POINT_DEFINE(hangDuringHoldingLocksForValidation);

namespace CollectionValidation {

ValidateState::ValidateState(OperationContext* opCtx,
                             const NamespaceString& nss,
                             ValidateMode mode,
                             RepairMode repairMode,
                             bool logDiagnostics)
    : _nss(nss),
      _mode(mode),
      _repairMode(repairMode),
      _dataThrottle(opCtx),
      _logDiagnostics(logDiagnostics) {

    // Subsequent re-locks will use the UUID when 'background' is true.
    if (isBackground()) {
        // Avoid taking the PBWM lock, which will stall replication if this is a secondary node
        // being validated.
        _noPBWM.emplace(opCtx->lockState());

        _databaseLock.emplace(opCtx, _nss.dbName(), MODE_IS);
        _collectionLock.emplace(opCtx, _nss, MODE_IS);
    } else {
        _databaseLock.emplace(opCtx, _nss.dbName(), MODE_IX);
        _collectionLock.emplace(opCtx, _nss, MODE_X);
    }

    if (MONGO_unlikely(hangDuringHoldingLocksForValidation.shouldFail())) {
        LOGV2(7490901, "Hanging on fail point 'hangDuringHoldingLocksForValidation'");
        hangDuringHoldingLocksForValidation.pauseWhileSet();
    }

    _database = _databaseLock->getDb() ? _databaseLock->getDb() : nullptr;
    if (_database)
        _collection =
            CollectionPtr(CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, _nss));

    if (!_collection) {
        auto view = CollectionCatalog::get(opCtx)->lookupView(opCtx, _nss);
        if (!view) {
            uasserted(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection '" << _nss.toStringForErrorMsg()
                                    << "' does not exist to validate.");
        } else {
            // Uses the bucket collection in place of the time-series collection view.
            if (!view->timeseries() ||
                !feature_flags::gExtendValidateCommand.isEnabled(
                    serverGlobalParams.featureCompatibility)) {
                uasserted(ErrorCodes::CommandNotSupportedOnView, "Cannot validate a view");
            }
            _nss = _nss.makeTimeseriesBucketsNamespace();
            if (isBackground()) {
                _collectionLock.emplace(opCtx, _nss, MODE_IS);
            } else {
                _collectionLock.emplace(opCtx, _nss, MODE_X);
            }
            _collection = CollectionPtr(
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, _nss));
            uassert(
                ErrorCodes::NamespaceNotFound,
                fmt::format(
                    "Cannot validate a time-series collection without its bucket collection {}.",
                    _nss.toStringForErrorMsg()),
                _collection);
        }
    }

    // RepairMode is incompatible with the ValidateModes kBackground and
    // kForegroundFullEnforceFastCount.
    if (fixErrors()) {
        invariant(!isBackground());
        invariant(!shouldEnforceFastCount());
    }

    if (adjustMultikey()) {
        invariant(!isBackground());
    }

    _uuid = _collection->uuid();
    _catalogGeneration = opCtx->getServiceContext()->getCatalogGeneration();
}

bool ValidateState::shouldEnforceFastCount() const {
    if (_mode == ValidateMode::kForegroundFullEnforceFastCount) {
        if (_nss.isOplog() || _nss.isChangeCollection()) {
            // Oplog writers only take a global IX lock, so the oplog can still be written to even
            // during full validation despite its collection X lock. This can cause validate to
            // incorrectly report an incorrect fast count on the oplog when run in enforceFastCount
            // mode.
            // The oplog entries are also written to the change collections and are prone to fast
            // count failures.
            return false;
        } else if (_nss == NamespaceString::kIndexBuildEntryNamespace) {
            // Do not enforce fast count on the 'config.system.indexBuilds' collection. This is an
            // internal collection that should not be queried and is empty most of the time.
            return false;
        } else if (_nss == NamespaceString::kSessionTransactionsTableNamespace) {
            // The 'config.transactions' collection is an implicitly replicated collection used for
            // internal bookkeeping for retryable writes and multi-statement transactions.
            // Replication rollback won't adjust the size storer counts for the
            // 'config.transactions' collection. We therefore do not enforce fast count on it.
            return false;
        } else if (_nss == NamespaceString::kConfigImagesNamespace) {
            // The 'config.image_collection' collection is an implicitly replicated collection used
            // for internal bookkeeping for retryable writes. Replication rollback won't adjust the
            // size storer counts for the 'config.image_collection' collection. We therefore do not
            // enforce fast count on it.
            return false;
        }

        return true;
    }

    return false;
}

void ValidateState::yield(OperationContext* opCtx) {
    if (isBackground()) {
        _yieldLocks(opCtx);
    }
    _yieldCursors(opCtx);
}

void ValidateState::_yieldLocks(OperationContext* opCtx) {
    invariant(isBackground());

    // Drop and reacquire the locks.
    _relockDatabaseAndCollection(opCtx);

    uassert(ErrorCodes::Interrupted,
            str::stream() << "Interrupted due to: catalog restart: " << _nss.toStringForErrorMsg()
                          << " (" << *_uuid << ") while validating the collection",
            _catalogGeneration == opCtx->getServiceContext()->getCatalogGeneration());

    // Check if any of the indexes we were validating were dropped. Indexes created while
    // yielding will be ignored.
    for (const auto& index : _indexes) {
        uassert(ErrorCodes::Interrupted,
                str::stream()
                    << "Interrupted due to: index being validated was dropped from collection: "
                    << _nss.toStringForErrorMsg() << " (" << *_uuid
                    << "), index: " << index->descriptor()->indexName(),
                !index->isDropped());
    }
};

void ValidateState::_yieldCursors(OperationContext* opCtx) {
    // Save all the cursors.
    for (const auto& indexCursor : _indexCursors) {
        indexCursor.second->save();
    }
    for (const auto& indexCursor : _columnStoreIndexCursors) {
        indexCursor.second->save();
    }

    _traverseRecordStoreCursor->save();
    _seekRecordStoreCursor->save();

    // Restore all the cursors.
    for (const auto& indexCursor : _indexCursors) {
        indexCursor.second->restore();
    }
    for (const auto& indexCursor : _columnStoreIndexCursors) {
        indexCursor.second->restore();
    }

    uassert(ErrorCodes::Interrupted,
            "Interrupted due to: failure to restore yielded traverse cursor",
            _traverseRecordStoreCursor->restore());
    uassert(ErrorCodes::Interrupted,
            "Interrupted due to: failure to restore yielded seek cursor",
            _seekRecordStoreCursor->restore());
}

void ValidateState::initializeCursors(OperationContext* opCtx) {
    invariant(!_traverseRecordStoreCursor && !_seekRecordStoreCursor && _indexCursors.size() == 0 &&
              _columnStoreIndexCursors.size() == 0 && _indexes.size() == 0);

    // Background validation reads from the last stable checkpoint instead of the latest data. This
    // allows concurrent writes to go ahead without interfering with validation's view of the data.
    RecoveryUnit::ReadSource rs = RecoveryUnit::ReadSource::kNoTimestamp;
    if (isBackground()) {
        opCtx->recoveryUnit()->abandonSnapshot();
        rs = RecoveryUnit::ReadSource::kCheckpoint;
        opCtx->recoveryUnit()->setTimestampReadSource(rs);
    }

    // We want to share the same data throttle instance across all the cursors used during this
    // validation. Validations started on other collections will not share the same data
    // throttle instance.
    if (!isBackground()) {
        _dataThrottle.turnThrottlingOff();
    }

    // For background validation, keep retrying if any table's cursor is opened on a different
    // checkpoint due to the background system-wide checkpoint happens during opening the cursors.
    // The retry should happen at most once since the checkpoint happens every 60 seconds.
    uint64_t checkpointId = 0;
    uint64_t currCheckpointId = 0;
    do {
        _indexCursors.clear();
        _indexes.clear();
        StringSet readyDurableIndexes;
        try {
            _traverseRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
                opCtx, _collection->getRecordStore(), &_dataThrottle);
            _seekRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
                opCtx, _collection->getRecordStore(), &_dataThrottle);
            DurableCatalog::get(opCtx)->getReadyIndexes(
                opCtx, _collection->getCatalogId(), &readyDurableIndexes);
            checkpointId = _traverseRecordStoreCursor->getCheckpointId();
            currCheckpointId = _seekRecordStoreCursor->getCheckpointId();
        } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
            invariant(isBackground());
            // End the validation if we can't open a checkpoint cursor on the collection.
            LOGV2(
                6868900,
                "Skipping background validation because the collection is not yet in a checkpoint",
                "nss"_attr = _nss,
                "ex"_attr = ex);
            throw;
        }

        const IndexCatalog* indexCatalog = _collection->getIndexCatalog();
        // The index iterator for ready indexes is timestamp-aware and will only return indexes that
        // are visible at our read time.
        const auto it =
            indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
        while (it->more() && checkpointId == currCheckpointId) {
            const IndexCatalogEntry* entry = it->next();
            const IndexDescriptor* desc = entry->descriptor();

            // Filter out any in-memory index in the collection that is not in our PIT view of the
            // MDB catalog. This is only important when background:true because we are then reading
            // from the checkpoint's view of the MDB catalog and data.
            if (isBackground() &&
                readyDurableIndexes.find(desc->indexName()) == readyDurableIndexes.end()) {
                LOGV2(
                    6868901,
                    "Skipping background validation on the index because the index is not yet in a "
                    "checkpoint.",
                    "desc_indexName"_attr = desc->indexName(),
                    "nss"_attr = _nss);
                _skippedIndexes.emplace(desc->indexName());
                continue;
            }

            // Read the index's ident from disk (the checkpoint if background:true). If it does not
            // match the in-memory ident saved in the IndexCatalogEntry, then our PIT view of the
            // index is old and the index has been dropped and recreated. In this case we will skip
            // it since there is no utility in checking a dropped index (we also cannot currently
            // access it because its in-memory representation is gone).
            auto diskIndexIdent =
                opCtx->getServiceContext()->getStorageEngine()->getCatalog()->getIndexIdent(
                    opCtx, _collection->getCatalogId(), desc->indexName());
            if (entry->getIdent() != diskIndexIdent) {
                LOGV2(6868902,
                      "Skipping validation on the index because the index was recreated and is not "
                      "yet in a checkpoint.",
                      "desc_indexName"_attr = desc->indexName(),
                      "nss"_attr = _nss);
                _skippedIndexes.emplace(desc->indexName());
                continue;
            }

            try {
                if (entry->descriptor()->getAccessMethodName() == IndexNames::COLUMN) {
                    const auto iam = entry->accessMethod();
                    auto indexCursor =
                        static_cast<ColumnStoreAccessMethod*>(iam)->storage()->newCursor(opCtx);
                    currCheckpointId = indexCursor->getCheckpointId();
                    _columnStoreIndexCursors.emplace(desc->indexName(), std::move(indexCursor));
                } else {
                    const auto iam = entry->accessMethod()->asSortedData();
                    if (!iam) {
                        LOGV2(6325100,
                              "[Debugging] skipping index {index_name} because it isn't SortedData",
                              "index_name"_attr = desc->indexName());
                        _skippedIndexes.emplace(desc->indexName());
                        continue;
                    }

                    auto indexCursor = std::make_unique<SortedDataInterfaceThrottleCursor>(
                        opCtx, iam, &_dataThrottle);
                    currCheckpointId = indexCursor->getCheckpointId();
                    _indexCursors.emplace(desc->indexName(), std::move(indexCursor));
                }
            } catch (const ExceptionFor<ErrorCodes::CursorNotFound>& ex) {
                invariant(isBackground());
                LOGV2(7359900,
                      "Skipping validation on the index because the index's checkpoint is not "
                      "consistent with the system-wide checkpoint.",
                      "indexName"_attr = desc->indexName(),
                      "nss"_attr = _nss,
                      "ex"_attr = ex);
                _skippedIndexes.emplace(desc->indexName());
                continue;
            }

            _indexes.push_back(indexCatalog->getEntryShared(desc));
        }
        // For foreground validation which doesn't use checkpoint cursors, the checkpoint id will
        // always be zero.
        if (!isBackground()) {
            invariant(!checkpointId && !currCheckpointId);
        }
    } while (checkpointId != currCheckpointId);

    if (rs != RecoveryUnit::ReadSource::kNoTimestamp) {
        invariant(rs == RecoveryUnit::ReadSource::kCheckpoint);
        invariant(isBackground());
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        _validateTs = storageEngine->getLastStableRecoveryTimestamp();
    }

    // Because SeekableRecordCursors don't have a method to reset to the start, we save and then
    // use a seek to the first RecordId to reset the cursor (and reuse it) as needed. When
    // iterating through a Record Store cursor, we initialize the loop (and obtain the first
    // Record) with a seek to the first Record (using firstRecordId). Subsequent loop iterations
    // use cursor->next() to get subsequent Records. However, if the Record Store is empty,
    // there is no first record. In this case, we set the first Record Id to an invalid RecordId
    // (RecordId()), which will halt iteration at the initialization step.
    auto record = _traverseRecordStoreCursor->next(opCtx);
    _firstRecordId = record ? std::move(record->id) : RecordId();
}

void ValidateState::_relockDatabaseAndCollection(OperationContext* opCtx) {
    invariant(isBackground());

    _collectionLock.reset();
    _databaseLock.reset();

    if (MONGO_unlikely(hangDuringYieldingLocksForValidation.shouldFail())) {
        LOGV2(20411, "Hanging on fail point 'hangDuringYieldingLocksForValidation'");
        hangDuringYieldingLocksForValidation.pauseWhileSet();
    }

    std::string dbErrMsg = str::stream()
        << "Interrupted due to: database drop: " << _nss.db()
        << " while validating collection: " << _nss.toStringForErrorMsg() << " (" << *_uuid << ")";

    _databaseLock.emplace(opCtx, _nss.dbName(), MODE_IS);
    _database = DatabaseHolder::get(opCtx)->getDb(opCtx, _nss.dbName());
    uassert(ErrorCodes::Interrupted, dbErrMsg, _database);
    uassert(ErrorCodes::Interrupted, dbErrMsg, !_database->isDropPending(opCtx));

    std::string collErrMsg = str::stream()
        << "Interrupted due to: collection drop: " << _nss.toStringForErrorMsg() << " (" << *_uuid
        << ") while validating the collection";

    try {
        NamespaceStringOrUUID nssOrUUID(_nss.dbName(), *_uuid);
        _collectionLock.emplace(opCtx, nssOrUUID, MODE_IS);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        uasserted(ErrorCodes::Interrupted, collErrMsg);
    }

    _collection =
        CollectionPtr(CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, *_uuid));
    uassert(ErrorCodes::Interrupted, collErrMsg, _collection);

    // The namespace of the collection can be changed during a same database collection rename.
    _nss = _collection->ns();
}

}  // namespace CollectionValidation
}  // namespace mongo
