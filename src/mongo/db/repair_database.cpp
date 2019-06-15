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
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

StatusWith<IndexNameObjs> getIndexNameObjs(OperationContext* opCtx,
                                           CollectionCatalogEntry* cce,
                                           std::function<bool(const std::string&)> filter) {
    IndexNameObjs ret;
    std::vector<std::string>& indexNames = ret.first;
    std::vector<BSONObj>& indexSpecs = ret.second;
    auto durableCatalog = DurableCatalog::get(opCtx);
    {
        // Fetch all indexes
        durableCatalog->getAllIndexes(opCtx, cce->ns(), &indexNames);
        auto newEnd =
            std::remove_if(indexNames.begin(),
                           indexNames.end(),
                           [&filter](const std::string& indexName) { return !filter(indexName); });
        indexNames.erase(newEnd, indexNames.end());

        indexSpecs.reserve(indexNames.size());


        for (const auto& name : indexNames) {
            BSONObj spec = durableCatalog->getIndexSpec(opCtx, cce->ns(), name);
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
                                  CollectionCatalogEntry* cce,
                                  const std::vector<BSONObj>& indexSpecs) {
    // Skip the rest if there are no indexes to rebuild.
    if (indexSpecs.empty())
        return Status::OK();

    // Rebuild the indexes provided by 'indexSpecs'.
    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    UUID buildUUID = UUID::gen();
    auto swRebuild =
        indexBuildsCoord->startIndexRebuildForRecovery(opCtx, cce, indexSpecs, buildUUID);
    if (!swRebuild.isOK()) {
        return swRebuild.getStatus();
    }

    auto[numRecords, dataSize] = swRebuild.getValue();

    auto rs = cce->getRecordStore();

    // Update the record store stats after finishing and committing the index builds.
    WriteUnitOfWork wuow(opCtx);
    rs->updateStatsAfterRepair(opCtx, numRecords, dataSize);
    wuow.commit();

    return Status::OK();
}

namespace {
Status repairCollections(OperationContext* opCtx,
                         StorageEngine* engine,
                         const std::string& dbName) {

    auto colls = CollectionCatalog::get(opCtx).getAllCollectionNamesFromDb(opCtx, dbName);

    for (const auto& nss : colls) {
        opCtx->checkForInterrupt();

        log() << "Repairing collection " << nss;

        Status status = engine->repairRecordStore(opCtx, nss);
        if (!status.isOK())
            return status;
    }

    for (const auto& nss : colls) {
        opCtx->checkForInterrupt();

        CollectionCatalogEntry* cce =
            CollectionCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(nss);
        auto swIndexNameObjs = getIndexNameObjs(opCtx, cce);
        if (!swIndexNameObjs.isOK())
            return swIndexNameObjs.getStatus();

        std::vector<BSONObj> indexSpecs = swIndexNameObjs.getValue().second;
        Status status = rebuildIndexesOnCollection(opCtx, cce, indexSpecs);
        if (!status.isOK())
            return status;

        engine->flushAllFiles(opCtx, true);
    }
    return Status::OK();
}
}  // namespace

Status repairDatabase(OperationContext* opCtx, StorageEngine* engine, const std::string& dbName) {
    DisableDocumentValidation validationDisabler(opCtx);

    // We must hold some form of lock here
    invariant(opCtx->lockState()->isW());
    invariant(dbName.find('.') == std::string::npos);

    log() << "repairDatabase " << dbName;

    BackgroundOperation::assertNoBgOpInProgForDb(dbName);

    opCtx->checkForInterrupt();

    // Close the db and invalidate all current users and caches.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    databaseHolder->close(opCtx, dbName);

    auto status = repairCollections(opCtx, engine, dbName);
    if (!status.isOK()) {
        severe() << "Failed to repair database " << dbName << ": " << status.reason();
    }

    try {
        // Ensure that we don't trigger an exception when attempting to take locks.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        // Open the db after everything finishes.
        auto db = databaseHolder->openDb(opCtx, dbName);

        // Set the minimum snapshot for all Collections in this db. This ensures that readers
        // using majority readConcern level can only use the collections after their repaired
        // versions are in the committed view.
        auto clusterTime = LogicalClock::getClusterTimeForReplicaSet(opCtx).asTimestamp();

        for (auto collIt = db->begin(opCtx); collIt != db->end(opCtx); ++collIt) {
            auto collection = *collIt;
            if (collection) {
                collection->setMinimumVisibleSnapshot(clusterTime);
            }
        }

        // Restore oplog Collection pointer cache.
        repl::acquireOplogCollectionForLogging(opCtx);
    } catch (const ExceptionFor<ErrorCodes::MustDowngrade>&) {
        // openDb can throw an exception with a MustDowngrade status if a collection does not
        // have a UUID.
        throw;
    } catch (...) {
        severe() << "Unexpected exception encountered while reopening database after repair.";
        std::terminate();  // Logs additional info about the specific error.
    }

    return status;
}

}  // namespace mongo
