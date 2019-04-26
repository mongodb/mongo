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
#define LOG_FOR_RECOVERY(level) \
    MONGO_LOG_COMPONENT(level, ::mongo::logger::LogComponent::kStorageRecovery)

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/catalog_control.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/ftdc/ftdc_mongod.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repair_database.h"
#include "mongo/util/log.h"

namespace mongo {
namespace catalog {
MinVisibleTimestampMap closeCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());

    BackgroundOperation::assertNoBgOpInProg();
    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgress();

    MinVisibleTimestampMap minVisibleTimestampMap;
    std::vector<std::string> allDbs =
        opCtx->getServiceContext()->getStorageEngine()->listDatabases();

    auto databaseHolder = DatabaseHolder::get(opCtx);
    for (auto&& dbName : allDbs) {
        const auto db = databaseHolder->getDb(opCtx, dbName);
        for (auto collIt = db->begin(opCtx); collIt != db->end(opCtx); ++collIt) {
            auto coll = *collIt;
            if (!coll) {
                break;
            }

            OptionalCollectionUUID uuid = coll->uuid();
            boost::optional<Timestamp> minVisible = coll->getMinimumVisibleSnapshot();

            // If there's a minimum visible, invariant there's also a UUID.
            invariant(!minVisible || uuid);
            if (uuid && minVisible) {
                LOG(1) << "closeCatalog: preserving min visible timestamp. Collection: "
                       << coll->ns() << " UUID: " << uuid << " TS: " << minVisible;
                minVisibleTimestampMap[*uuid] = *minVisible;
            }
        }
    }

    // Need to mark the UUIDCatalog as open if we our closeAll fails, dismissed if successful.
    auto reopenOnFailure = makeGuard([opCtx] { UUIDCatalog::get(opCtx).onOpenCatalog(opCtx); });
    // Closing UUID Catalog: only lookupNSSByUUID will fall back to using pre-closing state to
    // allow authorization for currently unknown UUIDs. This is needed because authorization needs
    // to work before acquiring locks, and might otherwise spuriously regard a UUID as unknown
    // while reloading the catalog.
    UUIDCatalog::get(opCtx).onCloseCatalog(opCtx);
    LOG(1) << "closeCatalog: closing UUID catalog";

    // Close all databases.
    log() << "closeCatalog: closing all databases";
    databaseHolder->closeAll(opCtx);

    // Close the storage engine's catalog.
    log() << "closeCatalog: closing storage engine catalog";
    opCtx->getServiceContext()->getStorageEngine()->closeCatalog(opCtx);

    reopenOnFailure.dismiss();
    return minVisibleTimestampMap;
}

void openCatalog(OperationContext* opCtx, const MinVisibleTimestampMap& minVisibleTimestampMap) {
    invariant(opCtx->lockState()->isW());

    // Load the catalog in the storage engine.
    log() << "openCatalog: loading storage engine catalog";
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->loadCatalog(opCtx);

    log() << "openCatalog: reconciling catalog and idents";
    auto indexesToRebuild = storageEngine->reconcileCatalogAndIdents(opCtx);
    fassert(40688, indexesToRebuild.getStatus());

    // Determine which indexes need to be rebuilt. rebuildIndexesOnCollection() requires that all
    // indexes on that collection are done at once, so we use a map to group them together.
    StringMap<IndexNameObjs> nsToIndexNameObjMap;
    for (auto indexNamespace : indexesToRebuild.getValue()) {
        NamespaceString collNss(indexNamespace.first);
        auto indexName = indexNamespace.second;

        auto collCatalogEntry =
            UUIDCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(collNss);
        invariant(collCatalogEntry,
                  str::stream() << "couldn't get collection catalog entry for collection "
                                << collNss.toString());

        auto indexSpecs =
            getIndexNameObjs(opCtx, collCatalogEntry, [&indexName](const std::string& name) {
                return name == indexName;
            });
        if (!indexSpecs.isOK() || indexSpecs.getValue().first.empty()) {
            fassert(40689,
                    {ErrorCodes::InternalError,
                     str::stream() << "failed to get index spec for index " << indexName
                                   << " in collection "
                                   << collNss.toString()});
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

        auto& ino = nsToIndexNameObjMap[collNss.ns()];
        ino.first.emplace_back(std::move(indexesToRebuild.first.back()));
        ino.second.emplace_back(std::move(indexesToRebuild.second.back()));
    }

    for (const auto& entry : nsToIndexNameObjMap) {
        NamespaceString collNss(entry.first);

        auto collCatalogEntry =
            UUIDCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(collNss);
        invariant(collCatalogEntry,
                  str::stream() << "couldn't get collection catalog entry for collection "
                                << collNss.toString());

        for (const auto& indexName : entry.second.first) {
            log() << "openCatalog: rebuilding index: collection: " << collNss.toString()
                  << ", index: " << indexName;
        }

        std::vector<BSONObj> indexSpecs = entry.second.second;
        fassert(40690, rebuildIndexesOnCollection(opCtx, collCatalogEntry, indexSpecs));
    }

    // Open all databases and repopulate the UUID catalog.
    log() << "openCatalog: reopening all databases";
    auto databaseHolder = DatabaseHolder::get(opCtx);
    std::vector<std::string> databasesToOpen = storageEngine->listDatabases();
    for (auto&& dbName : databasesToOpen) {
        LOG_FOR_RECOVERY(1) << "openCatalog: dbholder reopening database " << dbName;
        auto db = databaseHolder->openDb(opCtx, dbName);
        invariant(db, str::stream() << "failed to reopen database " << dbName);
        for (auto&& collNss : UUIDCatalog::get(opCtx).getAllCollectionNamesFromDb(opCtx, dbName)) {
            // Note that the collection name already includes the database component.
            auto collection = db->getCollection(opCtx, collNss.ns());
            invariant(collection,
                      str::stream() << "failed to get valid collection pointer for namespace "
                                    << collNss);

            auto uuid = collection->uuid();
            invariant(uuid);

            if (minVisibleTimestampMap.count(*uuid) > 0) {
                collection->setMinimumVisibleSnapshot(minVisibleTimestampMap.find(*uuid)->second);
            }

            // If this is the oplog collection, re-establish the replication system's cached pointer
            // to the oplog.
            if (collNss.isOplog()) {
                log() << "openCatalog: updating cached oplog pointer";
                collection->establishOplogCollectionForLogging(opCtx);
            }
        }
    }
    // Opening UUID Catalog: The UUID catalog is now in sync with the storage engine catalog. Clear
    // the pre-closing state.
    UUIDCatalog::get(opCtx).onOpenCatalog(opCtx);
    log() << "openCatalog: finished reloading UUID catalog";
}
}  // namespace catalog
}  // namespace mongo
