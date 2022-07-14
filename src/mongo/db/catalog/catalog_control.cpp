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

#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/catalog_control.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/rebuild_indexes.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace catalog {
namespace {
void reopenAllDatabasesAndReloadCollectionCatalog(
    OperationContext* opCtx,
    StorageEngine* storageEngine,
    const MinVisibleTimestampMap& minVisibleTimestampMap,
    Timestamp stableTimestamp) {

    // Open all databases and repopulate the CollectionCatalog.
    LOGV2(20276, "openCatalog: reopening all databases");

    // Applies all Collection writes to the in-memory catalog in a single copy-on-write to the
    // catalog. This avoids quadratic behavior where we iterate over every collection and perform
    // writes where the catalog would be copied every time. boost::optional is used to be able to
    // finish the write batch when encountering the oplog as other systems except immediate
    // visibility for the oplog.
    boost::optional<BatchedCollectionCatalogWriter> catalogWriter(opCtx);

    auto databaseHolder = DatabaseHolder::get(opCtx);
    std::vector<DatabaseName> databasesToOpen = storageEngine->listDatabases();
    for (auto&& dbName : databasesToOpen) {
        LOGV2_FOR_RECOVERY(
            23992, 1, "openCatalog: dbholder reopening database", "db"_attr = dbName);
        auto db = databaseHolder->openDb(opCtx, dbName);
        invariant(db, str::stream() << "failed to reopen database " << dbName.toString());
        for (auto&& collNss : catalogWriter.get()->getAllCollectionNamesFromDb(opCtx, dbName)) {
            // Note that the collection name already includes the database component.
            auto collection = catalogWriter.get()->lookupCollectionByNamespace(opCtx, collNss);
            invariant(collection,
                      str::stream()
                          << "failed to get valid collection pointer for namespace " << collNss);

            if (minVisibleTimestampMap.count(collection->uuid()) > 0) {
                // After rolling back to a stable timestamp T, the minimum visible timestamp for
                // each collection must be reset to (at least) its value at T. Additionally, there
                // cannot exist a minimum visible timestamp greater than lastApplied. This allows us
                // to upper bound what the minimum visible timestamp can be coming out of rollback.
                //
                // Because we only save the latest minimum visible timestamp for each collection, we
                // bound the minimum visible timestamp (where necessary) to the stable timestamp.
                // The benefit of fine grained tracking is assumed to be low-value compared to the
                // cost/effort.
                auto minVisible = std::min(stableTimestamp,
                                           minVisibleTimestampMap.find(collection->uuid())->second);
                auto writableCollection =
                    catalogWriter.get()->lookupCollectionByUUIDForMetadataWrite(opCtx,
                                                                                collection->uuid());
                writableCollection->setMinimumVisibleSnapshot(minVisible);
            }

            // If this is the oplog collection, re-establish the replication system's cached pointer
            // to the oplog.
            if (collNss.isOplog()) {
                LOGV2(20277, "openCatalog: updating cached oplog pointer");

                // The oplog collection must be visible when establishing for repl. Finish our
                // batched catalog write and continue on a new batch afterwards.
                catalogWriter.reset();

                repl::establishOplogCollectionForLogging(
                    opCtx, {collection.get(), CollectionPtr::NoYieldTag{}});
                catalogWriter.emplace(opCtx);
            }
        }
    }

    // Opening CollectionCatalog: The collection catalog is now in sync with the storage engine
    // catalog. Clear the pre-closing state.
    CollectionCatalog::write(opCtx,
                             [&](CollectionCatalog& catalog) { catalog.onOpenCatalog(opCtx); });
    opCtx->getServiceContext()->incrementCatalogGeneration();
    LOGV2(20278, "openCatalog: finished reloading collection catalog");
}
}  // namespace

MinVisibleTimestampMap closeCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());

    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgress();

    MinVisibleTimestampMap minVisibleTimestampMap;
    std::vector<DatabaseName> allDbs =
        opCtx->getServiceContext()->getStorageEngine()->listDatabases();

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& dbName : allDbs) {
        for (auto collIt = catalog->begin(opCtx, dbName); collIt != catalog->end(opCtx); ++collIt) {
            auto coll = *collIt;
            if (!coll) {
                break;
            }

            boost::optional<Timestamp> minVisible = coll->getMinimumVisibleSnapshot();

            // If there's a minimum visible, invariant there's also a UUID.
            if (minVisible) {
                LOGV2_DEBUG(20269,
                            1,
                            "closeCatalog: preserving min visible timestamp.",
                            "coll_ns"_attr = coll->ns(),
                            "uuid"_attr = coll->uuid(),
                            "minVisible"_attr = minVisible);
                minVisibleTimestampMap[coll->uuid()] = *minVisible;
            }
        }
    }

    // Need to mark the CollectionCatalog as open if we our closeAll fails, dismissed if successful.
    ScopeGuard reopenOnFailure([opCtx] {
        CollectionCatalog::write(opCtx,
                                 [&](CollectionCatalog& catalog) { catalog.onOpenCatalog(opCtx); });
    });
    // Closing CollectionCatalog: only lookupNSSByUUID will fall back to using pre-closing state to
    // allow authorization for currently unknown UUIDs. This is needed because authorization needs
    // to work before acquiring locks, and might otherwise spuriously regard a UUID as unknown
    // while reloading the catalog.
    CollectionCatalog::write(opCtx,
                             [&](CollectionCatalog& catalog) { catalog.onCloseCatalog(opCtx); });

    LOGV2_DEBUG(20270, 1, "closeCatalog: closing collection catalog");

    // Close all databases.
    LOGV2(20271, "closeCatalog: closing all databases");
    databaseHolder->closeAll(opCtx);

    // Close the storage engine's catalog.
    LOGV2(20272, "closeCatalog: closing storage engine catalog");
    opCtx->getServiceContext()->getStorageEngine()->closeCatalog(opCtx);

    reopenOnFailure.dismiss();
    return minVisibleTimestampMap;
}

void openCatalog(OperationContext* opCtx,
                 const MinVisibleTimestampMap& minVisibleTimestampMap,
                 Timestamp stableTimestamp) {
    invariant(opCtx->lockState()->isW());

    // Load the catalog in the storage engine.
    LOGV2(20273, "openCatalog: loading storage engine catalog");
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    // Ignore orphaned idents because this function is used during rollback and not at
    // startup recovery, when we may try to recover orphaned idents.
    storageEngine->loadCatalog(opCtx, StorageEngine::LastShutdownState::kClean);

    LOGV2(20274, "openCatalog: reconciling catalog and idents");
    auto reconcileResult = fassert(
        40688,
        storageEngine->reconcileCatalogAndIdents(opCtx, StorageEngine::LastShutdownState::kClean));

    // Determine which indexes need to be rebuilt. rebuildIndexesOnCollection() requires that all
    // indexes on that collection are done at once, so we use a map to group them together.
    StringMap<IndexNameObjs> nsToIndexNameObjMap;
    auto catalog = CollectionCatalog::get(opCtx);
    for (StorageEngine::IndexIdentifier indexIdentifier : reconcileResult.indexesToRebuild) {
        auto indexName = indexIdentifier.indexName;
        auto coll = catalog->lookupCollectionByNamespace(opCtx, indexIdentifier.nss);
        auto indexSpecs = getIndexNameObjs(
            coll, [&indexName](const std::string& name) { return name == indexName; });
        if (!indexSpecs.isOK() || indexSpecs.getValue().first.empty()) {
            fassert(40689,
                    {ErrorCodes::InternalError,
                     str::stream() << "failed to get index spec for index " << indexName
                                   << " in collection " << indexIdentifier.nss});
        }
        auto indexesToRebuild = indexSpecs.getValue();
        invariant(
            indexesToRebuild.first.size() == 1,
            str::stream() << "expected to find a list containing exactly 1 index name, but found "
                          << indexesToRebuild.first.size());
        invariant(
            indexesToRebuild.second.size() == 1,
            str::stream() << "expected to find a list containing exactly 1 index spec, but found "
                          << indexesToRebuild.second.size());

        auto& ino = nsToIndexNameObjMap[indexIdentifier.nss.ns()];
        ino.first.emplace_back(std::move(indexesToRebuild.first.back()));
        ino.second.emplace_back(std::move(indexesToRebuild.second.back()));
    }

    for (const auto& entry : nsToIndexNameObjMap) {
        NamespaceString collNss(entry.first);

        auto collection = catalog->lookupCollectionByNamespace(opCtx, collNss);
        invariant(collection, str::stream() << "couldn't get collection " << collNss.toString());

        for (const auto& indexName : entry.second.first) {
            LOGV2(20275,
                  "openCatalog: rebuilding index: collection: {collNss}, index: {indexName}",
                  "openCatalog: rebuilding index",
                  "namespace"_attr = collNss.toString(),
                  "index"_attr = indexName);
        }

        std::vector<BSONObj> indexSpecs = entry.second.second;
        fassert(40690, rebuildIndexesOnCollection(opCtx, collection, indexSpecs, RepairData::kNo));
    }

    // Once all unfinished index builds have been dropped and the catalog has been reloaded, resume
    // or restart any unfinished index builds. This will not resume/restart any index builds to
    // completion, but rather start the background thread, build the index, and wait for a
    // replicated commit or abort oplog entry.
    IndexBuildsCoordinator::get(opCtx)->restartIndexBuildsForRecovery(
        opCtx, reconcileResult.indexBuildsToRestart, reconcileResult.indexBuildsToResume);

    reopenAllDatabasesAndReloadCollectionCatalog(
        opCtx, storageEngine, minVisibleTimestampMap, stableTimestamp);
}


void openCatalogAfterStorageChange(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    reopenAllDatabasesAndReloadCollectionCatalog(opCtx, storageEngine, {}, {});
}

}  // namespace catalog
}  // namespace mongo
