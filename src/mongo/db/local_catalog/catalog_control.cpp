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

#include "mongo/db/local_catalog/catalog_control.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/index_builds/rebuild_indexes.h"
#include "mongo/db/local_catalog/catalog_repair.h"
#include "mongo/db/local_catalog/catalog_stats.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/historical_catalogid_tracker.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace catalog {
// Class since it accesses the internal
// CollectionCatalog::lookupCollectionByNamespaceForMetadataWrite method. This is possible because
// this class is actually a friend class with a forward declaration in collection_catalog.h. Note
// that this class is not exposed to the public and can only be used in this file.
class CatalogControlUtils {
public:
    static void reopenAllDatabasesAndReloadCollectionCatalog(
        OperationContext* opCtx,
        StorageEngine* storageEngine,
        const PreviousCatalogState& previousCatalogState,
        Timestamp stableTimestamp) {

        // Open all databases and repopulate the CollectionCatalog.
        LOGV2(20276, "openCatalog: reopening all databases");
        auto databaseHolder = DatabaseHolder::get(opCtx);
        std::vector<DatabaseName> databasesToOpen = catalog::listDatabases();
        for (auto&& dbName : databasesToOpen) {
            LOGV2_FOR_RECOVERY(
                23992, 1, "openCatalog: dbholder reopening database", logAttrs(dbName));
            auto db = databaseHolder->openDb(opCtx, dbName);
            invariant(
                db, str::stream() << "failed to reopen database " << dbName.toStringForErrorMsg());
            for (auto&& collNss :
                 CollectionCatalog::get(opCtx)->getAllCollectionNamesFromDb(opCtx, dbName)) {
                // Note that the collection name already includes the database component.
                auto collection =
                    CollectionCatalog::get(opCtx)->lookupCollectionByNamespaceForMetadataWrite(
                        opCtx, collNss);
                invariant(collection,
                          str::stream() << "failed to get valid collection pointer for namespace "
                                        << collNss.toStringForErrorMsg());

                if (auto it = previousCatalogState.minValidTimestampMap.find(collection->uuid());
                    it != previousCatalogState.minValidTimestampMap.end()) {
                    // After rolling back to a stable timestamp T, the minimum valid timestamp for
                    // each collection must be reset to (at least) its value at T. When the min
                    // valid timestamp is clamped to the stable timestamp we may end up with a
                    // pessimistic minimum valid timestamp set where the last DDL operation occurred
                    // earlier. This is fine as this is just an optimization when to avoid reading
                    // the catalog from WT.
                    auto minValid = std::min(stableTimestamp, it->second);

                    collection->setMinimumValidSnapshot(minValid);
                }

                if (collection->getTimeseriesOptions()) {
                    bool extendedRangeSetting;
                    if (auto it =
                            previousCatalogState.requiresTimestampExtendedRangeSupportMap.find(
                                collection->uuid());
                        it != previousCatalogState.requiresTimestampExtendedRangeSupportMap.end()) {
                        extendedRangeSetting = it->second;
                    } else {
                        extendedRangeSetting = timeseries::collectionMayRequireExtendedRangeSupport(
                            opCtx, *collection);
                    }

                    if (extendedRangeSetting) {
                        collection->setRequiresTimeseriesExtendedRangeSupport(opCtx);
                    }
                }

                // If this is the oplog collection, re-establish the replication system's cached
                // pointer to the oplog.
                if (collNss.isOplog()) {
                    LOGV2(20277, "openCatalog: updating cached oplog pointer");
                    repl::establishOplogRecordStoreForLogging(opCtx, collection->getRecordStore());
                }
            }
        }

        // Opening CollectionCatalog: The collection catalog is now in sync with the storage engine
        // catalog. Clear the pre-closing state.
        CollectionCatalog::write(opCtx,
                                 [&](CollectionCatalog& catalog) { catalog.onOpenCatalog(); });
        LOGV2(20278, "openCatalog: finished reloading collection catalog");
    }
};

PreviousCatalogState closeCatalog(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgress();

    PreviousCatalogState previousCatalogState;
    std::vector<DatabaseName> allDbs = catalog::listDatabases();

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto catalog = CollectionCatalog::get(opCtx);
    for (auto&& dbName : allDbs) {
        for (auto&& coll : catalog->range(dbName)) {
            if (!coll) {
                break;
            }

            // If there's a minimum valid, invariant there's also a UUID.
            boost::optional<Timestamp> minValid = coll->getMinimumValidSnapshot();
            if (minValid) {
                LOGV2_DEBUG(6825500,
                            1,
                            "closeCatalog: preserving min valid timestamp.",
                            "ns"_attr = coll->ns(),
                            "uuid"_attr = coll->uuid(),
                            "minValid"_attr = minValid);
                previousCatalogState.minValidTimestampMap[coll->uuid()] = *minValid;
            }

            if (coll->getTimeseriesOptions()) {
                previousCatalogState.requiresTimestampExtendedRangeSupportMap[coll->uuid()] =
                    coll->getRequiresTimeseriesExtendedRangeSupport();
            }
        }
    }

    // Need to mark the CollectionCatalog as open if we our closeAll fails, dismissed if successful.
    ScopeGuard reopenOnFailure([opCtx] {
        CollectionCatalog::write(opCtx,
                                 [](CollectionCatalog& catalog) { catalog.onOpenCatalog(); });
    });
    // Closing CollectionCatalog: only lookupNSSByUUID will fall back to using pre-closing state to
    // allow authorization for currently unknown UUIDs. This is needed because authorization needs
    // to work before acquiring locks, and might otherwise spuriously regard a UUID as unknown
    // while reloading the catalog.
    CollectionCatalog::write(opCtx, [](CollectionCatalog& catalog) { catalog.onCloseCatalog(); });

    LOGV2_DEBUG(20270, 1, "closeCatalog: closing collection catalog");

    // Close all databases.
    LOGV2(20271, "closeCatalog: closing all databases");
    databaseHolder->closeAll(opCtx);

    if (auto truncateMarkers = LocalOplogInfo::get(opCtx)->getTruncateMarkers()) {
        truncateMarkers->kill();
    }

    CollectionCatalog::write(opCtx, [opCtx](CollectionCatalog& catalog) {
        catalog.deregisterAllCollectionsAndViews(opCtx->getServiceContext());
    });

    // Close the storage engine's catalog.
    LOGV2(20272, "closeCatalog: closing storage engine catalog");
    opCtx->getServiceContext()->getStorageEngine()->closeMDBCatalog(opCtx);

    // Reset the stats counter for extended range time-series collections. This is maintained
    // outside the catalog itself.
    catalog_stats::requiresTimeseriesExtendedRangeSupport.storeRelaxed(0);

    reopenOnFailure.dismiss();
    return previousCatalogState;
}

void openCatalog(OperationContext* opCtx,
                 const PreviousCatalogState& previousCatalogState,
                 Timestamp stableTimestamp) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    // Load the catalog in the storage engine.
    LOGV2(20273, "openCatalog: loading storage engine catalog");
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    // Remove catalogId mappings for larger timestamp than 'stableTimestamp'.
    CollectionCatalog::write(opCtx, [stableTimestamp](CollectionCatalog& catalog) {
        catalog.catalogIdTracker().rollback(stableTimestamp);
    });

    // Ignore orphaned idents because this function is used during rollback and not at
    // startup recovery, when we may try to recover orphaned idents.
    storageEngine->loadMDBCatalog(opCtx, StorageEngine::LastShutdownState::kClean);
    catalog::initializeCollectionCatalog(opCtx, storageEngine, stableTimestamp);

    LOGV2(20274, "openCatalog: reconciling catalog and idents");
    auto reconcileResult =
        fassert(40688,
                catalog_repair::reconcileCatalogAndIdents(opCtx,
                                                          storageEngine,
                                                          stableTimestamp,
                                                          StorageEngine::LastShutdownState::kClean,
                                                          false /* forRepair */));

    // Once all unfinished index builds have been dropped and the catalog has been reloaded, resume
    // or restart any unfinished index builds. This will not resume/restart any index builds to
    // completion, but rather start the background thread, build the index, and wait for a
    // replicated commit or abort oplog entry.
    IndexBuildsCoordinator::get(opCtx)->restartIndexBuildsForRecovery(
        opCtx, reconcileResult.indexBuildsToRestart, reconcileResult.indexBuildsToResume);

    CatalogControlUtils::reopenAllDatabasesAndReloadCollectionCatalog(
        opCtx, storageEngine, previousCatalogState, stableTimestamp);
}

void openCatalogAfterStorageChange(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isW());
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    CatalogControlUtils::reopenAllDatabasesAndReloadCollectionCatalog(opCtx, storageEngine, {}, {});
}

}  // namespace catalog
}  // namespace mongo
