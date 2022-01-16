/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/db/repl/tenant_migration_shard_merge_util.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"

namespace mongo::repl {
namespace {
using namespace fmt::literals;

void moveFile(const std::string& src, const std::string& dst) {
    LOGV2_DEBUG(6114304, 1, "Moving file", "src"_attr = src, "dst"_attr = dst);
    // Boost filesystem functions clear "ec" on success.
    boost::system::error_code ec;
    boost::filesystem::rename(src, dst, ec);
    if (ec) {
        uasserted(6113900,
                  "Error copying file from '{}' to '{}': {}"_format(src, dst, ec.message()));
    }
}

void buildStorageMetadata(const WTimportArgs& importArgs, BSONObjBuilder& bob) {
    bob << importArgs.ident
        << BSON("tableMetadata" << importArgs.tableMetadata << "fileMetadata"
                                << importArgs.fileMetadata);
}

const std::string kTableExtension = ".wt";

std::string constructSourcePath(const std::string& importPath, const std::string& ident) {
    boost::filesystem::path filePath{importPath};
    filePath /= (ident + kTableExtension);
    return filePath.string();
}

std::string constructDestinationPath(const std::string& ident) {
    boost::filesystem::path filePath{storageGlobalParams.dbpath};
    filePath /= (ident + kTableExtension);
    return filePath.string();
}
}  // namespace

void wiredTigerImportFromBackupCursor(OperationContext* opCtx,
                                      const std::vector<CollectionImportMetadata>& metadatas,
                                      const std::string& importPath) {
    for (auto&& collectionMetadata : metadatas) {
        /*
         * Move one collection file and one or more index files from temp dir to dbpath.
         */
        moveFile(constructSourcePath(importPath, collectionMetadata.importArgs.ident),
                 constructDestinationPath(collectionMetadata.importArgs.ident));

        for (auto&& indexImportArgs : collectionMetadata.indexes) {
            moveFile(constructSourcePath(importPath, indexImportArgs.ident),
                     constructDestinationPath(indexImportArgs.ident));
        }

        /*
         * Import the collection and index(es).
         */
        BSONObjBuilder storageMetadata;
        buildStorageMetadata(collectionMetadata.importArgs, storageMetadata);
        for (const auto& indexImportArgs : collectionMetadata.indexes) {
            buildStorageMetadata(indexImportArgs, storageMetadata);
        }

        const auto nss = collectionMetadata.ns;
        writeConflictRetry(opCtx, "importCollection", nss.ns(), [&] {
            LOGV2_DEBUG(6114303, 1, "Importing donor collection", "ns"_attr = nss);
            AutoGetDb autoDb(opCtx, nss.db(), MODE_IX);
            Lock::CollectionLock collLock(opCtx, nss, MODE_X);
            auto catalog = CollectionCatalog::get(opCtx);
            WriteUnitOfWork wunit(opCtx);
            AutoStatsTracker statsTracker(opCtx,
                                          nss,
                                          Top::LockType::NotLocked,
                                          AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                          catalog->getDatabaseProfileLevel(nss.db()));

            // If the collection creation rolls back, ensure that the Top entry created for the
            // collection is deleted.
            opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
                Top::get(serviceContext).collectionDropped(nss);
            });

            // Create Collection object
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            auto durableCatalog = storageEngine->getCatalog();
            auto importResult = uassertStatusOK(DurableCatalog::get(opCtx)->importCollection(
                opCtx,
                collectionMetadata.ns,
                collectionMetadata.catalogObject,
                storageMetadata.done(),
                DurableCatalog::ImportCollectionUUIDOption::kKeepOld));
            const auto md = durableCatalog->getMetaData(opCtx, importResult.catalogId);
            for (const auto& index : md->indexes) {
                uassert(6114301, "Cannot import non-ready indexes", index.ready);
            }

            std::shared_ptr<Collection> ownedCollection = Collection::Factory::get(opCtx)->make(
                opCtx, nss, importResult.catalogId, md, std::move(importResult.rs));
            ownedCollection->init(opCtx);
            ownedCollection->setCommitted(false);

            // Update the number of records and data size on commit.
            opCtx->recoveryUnit()->registerChange(
                makeCountsChange(ownedCollection->getRecordStore(), collectionMetadata));

            UncommittedCollections::addToTxn(opCtx, std::move(ownedCollection));
            wunit.commit();
            LOGV2(6114300,
                  "Imported donor collection",
                  "ns"_attr = nss,
                  "numRecordsApprox"_attr = collectionMetadata.numRecords,
                  "dataSizeApprox"_attr = collectionMetadata.dataSize);
        });
    }
}
}  // namespace mongo::repl
