
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <algorithm>

#include "mongo/db/repair_database.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

StatusWith<IndexNameObjs> getIndexNameObjs(OperationContext* opCtx,
                                           DatabaseCatalogEntry* dbce,
                                           CollectionCatalogEntry* cce,
                                           stdx::function<bool(const std::string&)> filter) {
    IndexNameObjs ret;
    std::vector<std::string>& indexNames = ret.first;
    std::vector<BSONObj>& indexSpecs = ret.second;
    {
        // Fetch all indexes
        cce->getAllIndexes(opCtx, &indexNames);
        auto newEnd =
            std::remove_if(indexNames.begin(),
                           indexNames.end(),
                           [&filter](const std::string& indexName) { return !filter(indexName); });
        indexNames.erase(newEnd, indexNames.end());

        indexSpecs.reserve(indexNames.size());

        for (const auto& name : indexNames) {
            BSONObj spec = cce->getIndexSpec(opCtx, name);
            using IndexVersion = IndexDescriptor::IndexVersion;
            IndexVersion indexVersion = IndexVersion::kV1;
            if (auto indexVersionElem = spec[IndexDescriptor::kIndexVersionFieldName]) {
                auto indexVersionNum = indexVersionElem.numberInt();
                invariant(indexVersionNum == static_cast<int>(IndexVersion::kV1) ||
                          indexVersionNum == static_cast<int>(IndexVersion::kV2));
                indexVersion = static_cast<IndexVersion>(indexVersionNum);
            }
            invariant(spec.isOwned());
            indexSpecs.push_back(spec);

            const BSONObj key = spec.getObjectField("key");
            const Status keyStatus = index_key_validate::validateKeyPattern(key, indexVersion);
            if (!keyStatus.isOK()) {
                return Status(
                    ErrorCodes::CannotCreateIndex,
                    str::stream()
                        << "Cannot rebuild index "
                        << spec
                        << ": "
                        << keyStatus.reason()
                        << " For more info see http://dochub.mongodb.org/core/index-validation");
            }
        }
    }

    return ret;
}

Status rebuildIndexesOnCollection(OperationContext* opCtx,
                                  DatabaseCatalogEntry* dbce,
                                  CollectionCatalogEntry* cce,
                                  const IndexNameObjs& indexNameObjs) {
    const std::vector<std::string>& indexNames = indexNameObjs.first;
    const std::vector<BSONObj>& indexSpecs = indexNameObjs.second;

    // Skip the rest if there are no indexes to rebuild.
    if (indexSpecs.empty())
        return Status::OK();

    const auto& ns = cce->ns().ns();
    auto rs = dbce->getRecordStore(ns);

    std::unique_ptr<Collection> collection;
    std::unique_ptr<MultiIndexBlock> indexer;
    {
        // These steps are combined into a single WUOW to ensure there are no commits without
        // the indexes.
        // 1) Drop all indexes.
        // 2) Open the Collection
        // 3) Start the index build process.

        WriteUnitOfWork wuow(opCtx);

        {  // 1
            for (size_t i = 0; i < indexNames.size(); i++) {
                Status s = cce->removeIndex(opCtx, indexNames[i]);
                if (!s.isOK())
                    return s;
            }
        }

        // Indexes must be dropped before we open the Collection otherwise we could attempt to
        // open a bad index and fail.
        // TODO see if MultiIndexBlock can be made to work without a Collection.
        const auto uuid = cce->getCollectionOptions(opCtx).uuid;
        auto databaseHolder = DatabaseHolder::get(opCtx);
        collection = databaseHolder->makeCollection(opCtx, ns, uuid, cce, rs, dbce);

        indexer = std::make_unique<MultiIndexBlock>(opCtx, collection.get());
        Status status = indexer->init(indexSpecs).getStatus();
        if (!status.isOK()) {
            // The WUOW will handle cleanup, so the indexer shouldn't do its own.
            indexer->abortWithoutCleanup();
            return status;
        }

        wuow.commit();
    }

    // Iterate all records in the collection. Delete them if they aren't valid BSON. Index them
    // if they are.

    long long numRecords = 0;
    long long dataSize = 0;

    auto cursor = rs->getCursor(opCtx);
    auto record = cursor->next();
    while (record) {
        opCtx->checkForInterrupt();
        // Cursor is left one past the end of the batch inside writeConflictRetry
        auto beginBatchId = record->id;
        Status status = writeConflictRetry(opCtx, "repairDatabase", cce->ns().ns(), [&] {
            // In the case of WCE in a partial batch, we need to go back to the beginning
            if (!record || (beginBatchId != record->id)) {
                record = cursor->seekExact(beginBatchId);
            }
            WriteUnitOfWork wunit(opCtx);
            for (int i = 0; record && i < internalInsertMaxBatchSize.load(); i++) {
                RecordId id = record->id;
                RecordData& data = record->data;
                // Use the latest BSON validation version. We retain decimal data when repairing
                // database even if decimal is disabled.
                auto validStatus = validateBSON(data.data(), data.size(), BSONVersion::kLatest);
                if (!validStatus.isOK()) {
                    warning() << "Invalid BSON detected at " << id << ": " << redact(validStatus)
                              << ". Deleting.";
                    rs->deleteRecord(opCtx, id);
                } else {
                    numRecords++;
                    dataSize += data.size();
                    auto insertStatus = indexer->insert(data.releaseToBson(), id);
                    if (!insertStatus.isOK()) {
                        return insertStatus;
                    }
                }
                record = cursor->next();
            }
            cursor->save();  // Can't fail per API definition
            // When this exits via success or WCE, we need to restore the cursor
            ON_BLOCK_EXIT([ opCtx, ns = cce->ns().ns(), &cursor ]() {
                // restore CAN throw WCE per API
                writeConflictRetry(
                    opCtx, "retryRestoreCursor", ns, [&cursor] { cursor->restore(); });
            });
            wunit.commit();
            return Status::OK();
        });
        if (!status.isOK()) {
            return status;
        }
    }

    Status status = indexer->dumpInsertsFromBulk();
    if (!status.isOK())
        return status;

    {
        WriteUnitOfWork wunit(opCtx);
        status = indexer->commit();
        if (!status.isOK()) {
            return status;
        }
        rs->updateStatsAfterRepair(opCtx, numRecords, dataSize);
        wunit.commit();
    }

    return Status::OK();
}

namespace {
Status repairCollections(OperationContext* opCtx,
                         StorageEngine* engine,
                         const std::string& dbName) {

    DatabaseCatalogEntry* dbce = engine->getDatabaseCatalogEntry(opCtx, dbName);

    std::list<std::string> colls;
    dbce->getCollectionNamespaces(&colls);

    for (std::list<std::string>::const_iterator it = colls.begin(); it != colls.end(); ++it) {
        // Don't check for interrupt after starting to repair a collection otherwise we can
        // leave data in an inconsistent state. Interrupting between collections is ok, however.
        opCtx->checkForInterrupt();

        log() << "Repairing collection " << *it;

        Status status = engine->repairRecordStore(opCtx, *it);
        if (!status.isOK())
            return status;

        CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry(*it);
        auto swIndexNameObjs = getIndexNameObjs(opCtx, dbce, cce);
        if (!swIndexNameObjs.isOK())
            return swIndexNameObjs.getStatus();

        status = rebuildIndexesOnCollection(opCtx, dbce, cce, swIndexNameObjs.getValue());
        if (!status.isOK())
            return status;
    }
    return Status::OK();
}
}  // namespace

Status repairDatabase(OperationContext* opCtx, StorageEngine* engine, const std::string& dbName) {
    DisableDocumentValidation validationDisabler(opCtx);

    // We must hold some form of lock here
    invariant(opCtx->lockState()->isLocked());
    invariant(dbName.find('.') == std::string::npos);

    log() << "repairDatabase " << dbName;

    BackgroundOperation::assertNoBgOpInProgForDb(dbName);

    opCtx->checkForInterrupt();

    // Close the db and invalidate all current users and caches.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    databaseHolder->close(opCtx, dbName, "database closed for repair");
    ON_BLOCK_EXIT([databaseHolder, &dbName, &opCtx] {
        try {
            // Ensure that we don't trigger an exception when attempting to take locks.
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());

            // Open the db after everything finishes.
            auto db = databaseHolder->openDb(opCtx, dbName);

            // Set the minimum snapshot for all Collections in this db. This ensures that readers
            // using majority readConcern level can only use the collections after their repaired
            // versions are in the committed view.
            auto clusterTime = LogicalClock::getClusterTimeForReplicaSet(opCtx).asTimestamp();

            for (auto&& collection : *db) {
                collection->setMinimumVisibleSnapshot(clusterTime);
            }

            // Restore oplog Collection pointer cache.
            repl::acquireOplogCollectionForLogging(opCtx);
        } catch (...) {
            severe() << "Unexpected exception encountered while reopening database after repair.";
            std::terminate();  // Logs additional info about the specific error.
        }
    });

    auto status = repairCollections(opCtx, engine, dbName);
    if (!status.isOK()) {
        severe() << "Failed to repair database " << dbName << ": " << status.reason();
        return status;
    }

    DatabaseCatalogEntry* dbce = engine->getDatabaseCatalogEntry(opCtx, dbName);

    std::list<std::string> colls;
    dbce->getCollectionNamespaces(&colls);

    for (std::list<std::string>::const_iterator it = colls.begin(); it != colls.end(); ++it) {
        // Don't check for interrupt after starting to repair a collection otherwise we can
        // leave data in an inconsistent state. Interrupting between collections is ok, however.
        opCtx->checkForInterrupt();

        log() << "Repairing collection " << *it;

        Status status = engine->repairRecordStore(opCtx, *it);
        if (!status.isOK())
            return status;

        CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry(*it);
        auto swIndexNameObjs = getIndexNameObjs(opCtx, dbce, cce);
        if (!swIndexNameObjs.isOK())
            return swIndexNameObjs.getStatus();

        status = rebuildIndexesOnCollection(opCtx, dbce, cce, swIndexNameObjs.getValue());
        if (!status.isOK())
            return status;

        engine->flushAllFiles(opCtx, true);
    }

    return Status::OK();
}
}  // namespace mongo
