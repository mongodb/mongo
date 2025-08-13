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


#include "mongo/db/index_builds/skipped_record_tracker.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/preallocated_container_pool.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <mutex>

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex


namespace mongo {
namespace {
static constexpr StringData kRecordIdField = "recordId"_sd;
}

SkippedRecordTracker::SkippedRecordTracker(OperationContext* opCtx,
                                           StringData ident,
                                           bool tableExists)
    : _ident(std::string{ident}) {
    if (!tableExists) {
        return;
    }

    // Only initialize the table if it already exists. Otherwise, lazily initialize table when we
    // record the first document.
    _skippedRecordsTable =
        opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStoreFromExistingIdent(
            opCtx, _ident, KeyFormat::Long);
}

void SkippedRecordTracker::keepTemporaryTable() {
    if (_skippedRecordsTable) {
        _skippedRecordsTable->keep();
    }
}

void SkippedRecordTracker::record(OperationContext* opCtx, const RecordId& recordId) {
    BSONObjBuilder builder;
    recordId.serializeToken(kRecordIdField, &builder);
    BSONObj toInsert = builder.obj();

    // Lazily initialize table when we record the first document.
    if (!_skippedRecordsTable) {
        _skippedRecordsTable =
            opCtx->getServiceContext()->getStorageEngine()->makeTemporaryRecordStore(
                opCtx, _ident, KeyFormat::Long);
    }

    writeConflictRetry(
        opCtx, "recordSkippedRecordTracker", NamespaceString::kIndexBuildEntryNamespace, [&]() {
            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(_skippedRecordsTable->rs()
                                ->insertRecord(opCtx,
                                               *shard_role_details::getRecoveryUnit(opCtx),
                                               toInsert.objdata(),
                                               toInsert.objsize(),
                                               Timestamp::min())
                                .getStatus());
            wuow.commit();
        });
}

bool SkippedRecordTracker::areAllRecordsApplied(OperationContext* opCtx) const {
    if (!_skippedRecordsTable) {
        return true;
    }
    auto cursor =
        _skippedRecordsTable->rs()->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    auto record = cursor->next();

    // The table is empty only when all writes are applied.
    if (!record)
        return true;

    return false;
}

Status SkippedRecordTracker::retrySkippedRecords(OperationContext* opCtx,
                                                 const CollectionPtr& collection,
                                                 const IndexCatalogEntry* indexCatalogEntry,
                                                 RetrySkippedRecordMode mode) {

    const bool keyGenerationOnly = mode == RetrySkippedRecordMode::kKeyGeneration;

    dassert(shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
        collection->ns(), keyGenerationOnly ? MODE_IX : MODE_X));
    if (!_skippedRecordsTable) {
        return Status::OK();
    }

    InsertDeleteOptions options;
    collection->getIndexCatalog()->prepareInsertDeleteOptions(
        opCtx,
        indexCatalogEntry->getNSSFromCatalog(opCtx),
        indexCatalogEntry->descriptor(),
        &options);

    // This should only be called when constraints are being enforced, on a primary. It does not
    // make sense, nor is it necessary for this to be called on a secondary.
    invariant(options.getKeysMode ==
              InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints);

    static const char* curopMessage = "Index Build: retrying skipped records";
    ProgressMeterHolder progress;
    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.set(
            lk,
            CurOp::get(opCtx)->setProgress(lk, curopMessage, _skippedRecordCounter.load(), 1),
            opCtx);
    }

    int resolved = 0;
    const auto onResolved = [&]() {
        resolved++;

        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.get(lk)->hit();
    };

    SharedBufferFragmentBuilder pooledBuilder(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
    auto& containerPool = PreallocatedContainerPool::get(opCtx);

    auto recordStore = _skippedRecordsTable->rs();
    auto cursor = recordStore->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    while (auto record = cursor->next()) {
        const BSONObj doc = record->data.toBson();

        // This is the RecordId of the skipped record from the collection.
        RecordId skippedRecordId = RecordId::deserializeToken(doc[kRecordIdField]);
        boost::optional<WriteUnitOfWork> wuow;
        if (!keyGenerationOnly) {
            wuow.emplace(opCtx);
        }

        // If the record still exists, get a potentially new version of the document to index.
        auto collCursor = collection->getCursor(opCtx);
        auto skippedRecord = collCursor->seekExact(skippedRecordId);
        if (keyGenerationOnly && !skippedRecord) {
            continue;
        } else if (skippedRecord) {
            const auto skippedDoc = skippedRecord->data.toBson();
            LOGV2_DEBUG(23882,
                        2,
                        "reapplying skipped RecordID {skippedRecordId}: {skippedDoc}",
                        "skippedRecordId"_attr = skippedRecordId,
                        "skippedDoc"_attr = skippedDoc);

            auto keys = containerPool.keys();
            auto multikeyMetadataKeys = containerPool.multikeyMetadataKeys();
            auto multikeyPaths = containerPool.multikeyPaths();
            auto iam = indexCatalogEntry->accessMethod()->asSortedData();

            try {
                // Because constraint enforcement is set, this will throw if there are any indexing
                // errors, instead of writing back to the skipped records table, which would
                // normally happen if constraints were relaxed.
                iam->getKeys(opCtx,
                             collection,
                             indexCatalogEntry,
                             pooledBuilder,
                             skippedDoc,
                             options.getKeysMode,
                             SortedDataIndexAccessMethod::GetKeysContext::kAddingKeys,
                             keys.get(),
                             multikeyMetadataKeys.get(),
                             multikeyPaths.get(),
                             skippedRecordId);

                if (keyGenerationOnly) {
                    // On dry runs we can skip everything else that comes after key generation.
                    onResolved();
                    continue;
                }
                auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
                auto status = iam->insertKeys(
                    opCtx, ru, collection, indexCatalogEntry, *keys, options, nullptr, nullptr);
                if (!status.isOK()) {
                    return status;
                }

                status = iam->insertKeys(opCtx,
                                         ru,
                                         collection,
                                         indexCatalogEntry,
                                         *multikeyMetadataKeys,
                                         options,
                                         nullptr,
                                         nullptr);
                if (!status.isOK()) {
                    return status;
                }
            } catch (const DBException& ex) {
                return ex.toStatus();
            }

            if (iam->shouldMarkIndexAsMultikey(
                    keys->size(), *multikeyMetadataKeys, *multikeyPaths)) {
                if (!_multikeyPaths) {
                    _multikeyPaths = *multikeyPaths;
                }

                MultikeyPathTracker::mergeMultikeyPaths(&_multikeyPaths.value(), *multikeyPaths);
            }
        }

        // Delete the record so that it is not applied more than once.
        recordStore->deleteRecord(opCtx, *shard_role_details::getRecoveryUnit(opCtx), record->id);

        cursor->save();
        wuow->commit();
        cursor->restore(*shard_role_details::getRecoveryUnit(opCtx));

        onResolved();
    }

    {
        stdx::unique_lock<Client> lk(*opCtx->getClient());
        progress.get(lk)->finished();
    }

    int logLevel = (resolved > 0) ? 0 : 1;
    if (keyGenerationOnly) {
        LOGV2_DEBUG(7333101,
                    logLevel,
                    "Index build: verified key generation for skipped records",
                    "index"_attr = indexCatalogEntry->descriptor()->indexName(),
                    "numResolved"_attr = resolved);
    } else {
        LOGV2_DEBUG(23883,
                    logLevel,
                    "Index build: reapplied skipped records",
                    "index"_attr = indexCatalogEntry->descriptor()->indexName(),
                    "numResolved"_attr = resolved);
    }
    return Status::OK();
}

}  // namespace mongo
