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


#include <absl/container/flat_hash_map.h>
#include <fmt/format.h>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/validate/validate_gen.h"
#include "mongo/db/catalog/validate/validate_state.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/views/view.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangDuringValidationInitialization);

namespace CollectionValidation {

ValidateState::ValidateState(OperationContext* opCtx,
                             const NamespaceString& nss,
                             ValidationOptions options)
    : ValidationOptions(std::move(options)),
      _nss(nss),
      _dataThrottle(opCtx, [&]() { return gMaxValidateMBperSec.load(); }) {

    // RepairMode is incompatible with the ValidateModes kBackground and
    // kForegroundFullEnforceFastCount.
    if (fixErrors()) {
        invariant(!isBackground());
        invariant(!shouldEnforceFastCount());
    }

    if (adjustMultikey()) {
        invariant(!isBackground());
    }
}

bool ValidateState::shouldEnforceFastCount() const {
    if (enforceFastCountRequested()) {
        if (_nss.isOplog() || _nss.isChangeCollection() ||
            _nss.isChangeStreamPreImagesCollection()) {
            // Oplog writers only take a global IX lock, so the oplog can still be written to even
            // during full validation despite its collection X lock. This can cause validate to
            // incorrectly report an incorrect fast count on the oplog when run in enforceFastCount
            // mode.
            // The oplog entries are also written to the change collections and pre-images
            // collections, these collections are also prone to fast count failures.
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

void ValidateState::yieldCursors(OperationContext* opCtx) {
    // Save all the cursors.
    for (const auto& indexCursor : _indexCursors) {
        indexCursor.second->save();
    }

    _traverseRecordStoreCursor->save();
    _seekRecordStoreCursor->save();

    // Restore all the cursors.
    for (const auto& indexCursor : _indexCursors) {
        indexCursor.second->restore();
    }

    uassert(ErrorCodes::Interrupted,
            "Interrupted due to: failure to restore yielded traverse cursor",
            _traverseRecordStoreCursor->restore());
    uassert(ErrorCodes::Interrupted,
            "Interrupted due to: failure to restore yielded seek cursor",
            _seekRecordStoreCursor->restore());
}

Status ValidateState::initializeCollection(OperationContext* opCtx) {
    _validateTs = opCtx->getServiceContext()->getStorageEngine()->getLastStableRecoveryTimestamp();

    // Background validation reads data from the last stable checkpoint.
    if (isBackground()) {
        if (!_validateTs) {
            return Status(
                ErrorCodes::NamespaceNotFound,
                fmt::format("Cannot run background validation on collection {} because there "
                            "is no checkpoint yet",
                            _nss.toStringForErrorMsg()));
        }
        shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
            RecoveryUnit::ReadSource::kProvided, *_validateTs);

        _globalLock.emplace(opCtx, MODE_IS);

        try {
            shard_role_details::getRecoveryUnit(opCtx)->preallocateSnapshot();
        } catch (const ExceptionFor<ErrorCodes::SnapshotTooOld>&) {
            // This will throw SnapshotTooOld to indicate we cannot find an available snapshot at
            // the provided timestamp. This is likely because minSnapshotHistoryWindowInSeconds has
            // been changed to a lower value from the default of 5 minutes.
            return Status(
                ErrorCodes::NamespaceNotFound,
                fmt::format("Cannot run background validation on collection {} because the "
                            "snapshot history is no longer available",
                            _nss.toStringForErrorMsg()));
        }

        _catalog = CollectionCatalog::get(opCtx);
        _collection =
            CollectionPtr(_catalog->establishConsistentCollection(opCtx, _nss, _validateTs));
    } else {
        _globalLock.emplace(opCtx, MODE_IX);
        _databaseLock.emplace(opCtx, _nss.dbName(), MODE_IX);
        _collectionLock.emplace(opCtx, _nss, MODE_X);
        _catalog = CollectionCatalog::get(opCtx);
        _collection = CollectionPtr(_catalog->lookupCollectionByNamespace(opCtx, _nss));
    }

    if (!_collection) {
        auto view = _catalog->lookupView(opCtx, _nss);
        if (!view) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream() << "Collection '" << _nss.toStringForErrorMsg()
                                        << "' does not exist to validate.");
        }

        // For validation on time-series collections, we need to use the bucket namespace.
        if (!view->timeseries()) {
            return Status(ErrorCodes::CommandNotSupportedOnView, "Cannot validate a view");
        }

        _nss = _nss.makeTimeseriesBucketsNamespace();
        if (isBackground()) {
            _collection =
                CollectionPtr(_catalog->establishConsistentCollection(opCtx, _nss, _validateTs));
        } else {
            _collectionLock.emplace(opCtx, _nss, MODE_X);
            _collection = CollectionPtr(_catalog->lookupCollectionByNamespace(opCtx, _nss));
        }

        if (!_collection) {
            return Status(ErrorCodes::NamespaceNotFound,
                          fmt::format("Cannot validate a time-series collection without its "
                                      "bucket collection {}.",
                                      _nss.toStringForErrorMsg()));
        }
    }

    if (MONGO_unlikely(hangDuringValidationInitialization.shouldFail())) {
        LOGV2(7490901, "Hanging on fail point 'hangDuringValidationInitialization'");
        hangDuringValidationInitialization.pauseWhileSet();
    }

    _uuid = _collection->uuid();

    // We want to share the same data throttle instance across all the cursors used during this
    // validation. Validations started on other collections will not share the same data
    // throttle instance.
    if (!isBackground()) {
        _dataThrottle.turnThrottlingOff();
    }

    return Status::OK();
}

void ValidateState::initializeCursors(OperationContext* opCtx) {
    _traverseRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
        opCtx, _collection->getRecordStore(), &_dataThrottle);
    _seekRecordStoreCursor = std::make_unique<SeekableRecordThrottleCursor>(
        opCtx, _collection->getRecordStore(), &_dataThrottle);

    const IndexCatalog* indexCatalog = _collection->getIndexCatalog();
    // The index iterator for ready indexes is timestamp-aware and will only return indexes that
    // are visible at our read time.
    const auto it = indexCatalog->getIndexIterator(opCtx, IndexCatalog::InclusionPolicy::kReady);
    while (it->more()) {
        const IndexCatalogEntry* entry = it->next();
        const IndexDescriptor* desc = entry->descriptor();
        const auto iam = entry->accessMethod()->asSortedData();
        auto indexCursor =
            std::make_unique<SortedDataInterfaceThrottleCursor>(opCtx, iam, &_dataThrottle);
        _indexCursors.emplace(desc->indexName(), std::move(indexCursor));
        _indexIdents.push_back(desc->getEntry()->getIdent());
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

}  // namespace CollectionValidation
}  // namespace mongo
