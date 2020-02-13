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
#include "mongo/db/catalog/collection_validation.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/multi_index_block.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/rebuild_indexes.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

Status rebuildIndexesForNamespace(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  StorageEngine* engine) {
    opCtx->checkForInterrupt();
    auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
    auto swIndexNameObjs = getIndexNameObjs(opCtx, collection->getCatalogId());
    if (!swIndexNameObjs.isOK())
        return swIndexNameObjs.getStatus();

    std::vector<BSONObj> indexSpecs = swIndexNameObjs.getValue().second;
    Status status = rebuildIndexesOnCollection(opCtx, collection, indexSpecs, RepairData::kYes);
    if (!status.isOK())
        return status;

    engine->flushAllFiles(opCtx, /*callerHoldsReadLock*/ false);
    return Status::OK();
}

namespace {
Status repairCollections(OperationContext* opCtx,
                         StorageEngine* engine,
                         const std::string& dbName) {
    auto colls = CollectionCatalog::get(opCtx).getAllCollectionNamesFromDb(opCtx, dbName);

    for (const auto& nss : colls) {
        opCtx->checkForInterrupt();

        LOGV2(21027, "Repairing collection {nss}", "nss"_attr = nss);

        auto collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
        Status status = engine->repairRecordStore(opCtx, collection->getCatalogId(), nss);

        // If data was modified during repairRecordStore, we know to rebuild indexes without needing
        // to run an expensive collection validation.
        if (status.code() == ErrorCodes::DataModifiedByRepair) {
            Status status = rebuildIndexesForNamespace(opCtx, nss, engine);
            if (!status.isOK()) {
                return status;
            }
            continue;
        } else if (!status.isOK()) {
            return status;
        }

        // Run collection validation to avoid unecessarily rebuilding indexes on valid collections
        // with consistent indexes. Initialize the collection prior to validation. Need to lookup
        // from catalog again because the old collection object was invalidated by
        // repairRecordStore.
        collection = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss);
        collection->init(opCtx);

        ValidateResults validateResults;
        BSONObjBuilder output;

        // Set options to exclude FullRecordStoreValidation because we have already validated the
        // underlying record store in the call to repairRecordStore above.
        auto options = CollectionValidation::ValidateOptions::kFullIndexValidation;

        const bool background = false;
        status = CollectionValidation::validate(
            opCtx, nss, options, background, &validateResults, &output);
        if (!status.isOK()) {
            return status;
        }

        LOGV2(21028,
              "Collection validation results: {output_done}",
              "output_done"_attr = output.done());

        if (!validateResults.valid) {
            status = rebuildIndexesForNamespace(opCtx, nss, engine);
            if (!status.isOK()) {
                return status;
            }
        }
    }
    return Status::OK();
}
}  // namespace

Status repairDatabase(OperationContext* opCtx, StorageEngine* engine, const std::string& dbName) {
    DisableDocumentValidation validationDisabler(opCtx);

    // We must hold some form of lock here
    invariant(opCtx->lockState()->isW());
    invariant(dbName.find('.') == std::string::npos);

    LOGV2(21029, "repairDatabase {dbName}", "dbName"_attr = dbName);

    BackgroundOperation::assertNoBgOpInProgForDb(dbName);

    opCtx->checkForInterrupt();

    // Close the db and invalidate all current users and caches.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    databaseHolder->close(opCtx, dbName);

    // Reopening db is necessary for repairCollections.
    auto db = databaseHolder->openDb(opCtx, dbName);

    auto status = repairCollections(opCtx, engine, dbName);
    if (!status.isOK()) {
        LOGV2_FATAL(21030,
                    "Failed to repair database {dbName}: {status_reason}",
                    "dbName"_attr = dbName,
                    "status_reason"_attr = status.reason());
    }

    try {
        // Ensure that we don't trigger an exception when attempting to take locks.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

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
        LOGV2_FATAL(21031,
                    "Unexpected exception encountered while reopening database after repair.");
        std::terminate();  // Logs additional info about the specific error.
    }

    return status;
}

}  // namespace mongo
