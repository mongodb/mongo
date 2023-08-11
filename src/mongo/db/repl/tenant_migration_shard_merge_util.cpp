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


#include "mongo/db/repl/tenant_migration_shard_merge_util.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <fmt/format.h>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/preprocessor/control/iif.hpp>
// IWYU pragma: no_include "boost/system/detail/error_code.hpp"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/import_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/durable_catalog_entry.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/cluster_parameter_synchronization_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

namespace mongo::repl::shard_merge_utils {
namespace {

MONGO_FAIL_POINT_DEFINE(skipImportFiles);

using namespace fmt::literals;

void moveFile(const std::string& src, const std::string& dst) {
    LOGV2_DEBUG(6114304, 1, "Moving file", "from"_attr = src, "to"_attr = dst);

    uassert(6114401,
            "Destination file '{}' already exists"_format(dst),
            !boost::filesystem::exists(dst));

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

/**
 * Generate a new ident and move the file.
 */
std::string moveWithNewIdent(OperationContext* opCtx,
                             const std::string& tempWTDirectory,
                             const mongo::NamespaceString& metadataNS,
                             const std::string& oldIdent,
                             const char* kind,
                             std::vector<std::tuple<std::string, std::string>>& revertMoves) {
    auto srcFilePath = constructSourcePath(tempWTDirectory, oldIdent);

    while (true) {
        try {
            auto newIdent = DurableCatalog::get(opCtx)->generateUniqueIdent(metadataNS, kind);
            auto destFilePath = constructDestinationPath(newIdent);

            moveFile(srcFilePath, destFilePath);
            // Register revert file move in case of failure to import collection and it's
            // indexes.
            revertMoves.emplace_back(std::move(srcFilePath), std::move(destFilePath));

            return newIdent;
        } catch (const DBException& ex) {
            // Retry move on "destination file already exists" error. This can happen due to
            // ident collision between this import and another parallel import  via
            // importCollection command.
            if (ex.code() == 6114401) {
                LOGV2(7199801,
                      "Failed to move file from temp to active WT directory. Retrying "
                      "the move operation using another new unique ident.",
                      "error"_attr = redact(ex.toStatus()));
                continue;
            }
            throw;
        }
    }
    MONGO_UNREACHABLE;
}

/**
 * Import the collection and its indexes into the main wiredtiger instance.
 */
void importCollectionAndItsIndexesInMainWTInstance(OperationContext* opCtx,
                                                   const CollectionImportMetadata& metadata,
                                                   const UUID& migrationId,
                                                   const BSONObj& storageMetaObj) {
    const auto nss = metadata.ns;
    writeConflictRetry(opCtx, "importCollection", nss, [&] {
        LOGV2_DEBUG(6114303, 1, "Importing donor collection", "ns"_attr = nss);
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        auto db = autoDb.ensureDbExists(opCtx);
        invariant(db);
        Lock::CollectionLock collLock(opCtx, nss, MODE_X);
        auto catalog = CollectionCatalog::get(opCtx);
        WriteUnitOfWork wunit(opCtx);
        AutoStatsTracker statsTracker(opCtx,
                                      nss,
                                      Top::LockType::NotLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      catalog->getDatabaseProfileLevel(nss.dbName()));

        // If the collection creation rolls back, ensure that the Top entry created for the
        // collection is deleted.
        opCtx->recoveryUnit()->onRollback(
            [nss, serviceContext = opCtx->getServiceContext()](OperationContext*) {
                Top::get(serviceContext).collectionDropped(nss);
            });

        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "Collection already exists. NS: " << nss.toStringForErrorMsg(),
                !CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss));

        // Create Collection object.
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        auto durableCatalog = storageEngine->getCatalog();
        ImportOptions importOptions(ImportOptions::ImportCollectionUUIDOption::kKeepOld);
        importOptions.importTimestampRule = ImportOptions::ImportTimestampRule::kStable;
        // Since we are using the ident id generated by this recipient node, ident collisions in
        // the future after import is not possible. So, it's ok to skip the ident collision
        // check. Otherwise, we would unnecessarily generate new rand after each collection
        // import.
        importOptions.skipIdentCollisionCheck = true;

        auto importResult = uassertStatusOK(DurableCatalog::get(opCtx)->importCollection(
            opCtx, nss, metadata.catalogObject, storageMetaObj, importOptions));

        const auto catalogEntry =
            durableCatalog->getParsedCatalogEntry(opCtx, importResult.catalogId);
        const auto md = catalogEntry->metadata;
        for (const auto& index : md->indexes) {
            uassert(6114301, "Cannot import non-ready indexes", index.ready);
        }

        std::shared_ptr<Collection> ownedCollection = Collection::Factory::get(opCtx)->make(
            opCtx, nss, importResult.catalogId, md, std::move(importResult.rs));
        ownedCollection->init(opCtx);
        historicalIDTrackerAllowsMixedModeWrites(ownedCollection->getSharedDecorations())
            .store(true);

        // Update the number of records and data size on commit.
        opCtx->recoveryUnit()->registerChange(
            makeCountsChange(ownedCollection->getRecordStore(), metadata));

        CollectionCatalog::get(opCtx)->onCreateCollection(opCtx, ownedCollection);

        auto importedCatalogEntry =
            storageEngine->getCatalog()->getCatalogEntry(opCtx, importResult.catalogId);
        opCtx->getServiceContext()->getOpObserver()->onImportCollection(opCtx,
                                                                        migrationId,
                                                                        nss,
                                                                        metadata.numRecords,
                                                                        metadata.dataSize,
                                                                        importedCatalogEntry,
                                                                        storageMetaObj,
                                                                        /*dryRun=*/false);

        wunit.commit();

        if (metadata.numRecords > 0 &&
            nss == NamespaceString::makeClusterParametersNSS(nss.tenantId())) {
            cluster_parameters::initializeAllTenantParametersFromCollection(opCtx,
                                                                            &*ownedCollection);
        }

        LOGV2(6114300,
              "Imported donor collection",
              "ns"_attr = nss,
              "numRecordsApprox"_attr = metadata.numRecords,
              "dataSizeApprox"_attr = metadata.dataSize);
    });
}

}  // namespace

std::string getPathRelativeTo(const std::string& path, const std::string& basePath) {
    if (basePath.empty() || path.find(basePath) != 0) {
        uasserted(6113319,
                  str::stream() << "The file " << path << " is not a subdirectory of " << basePath);
    }

    auto result = path.substr(basePath.size());
    // Skip separators at the beginning of the relative part.
    if (!result.empty() && (result[0] == '/' || result[0] == '\\')) {
        result.erase(result.begin());
    }

    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

void createImportDoneMarkerLocalCollection(OperationContext* opCtx, const UUID& migrationId) {
    UnreplicatedWritesBlock writeBlock(opCtx);
    // Collections in 'local' db should not expect any lock or prepare conflicts.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowAcquisitionOfLocks(opCtx->lockState());

    auto status = StorageInterface::get(opCtx)->createCollection(
        opCtx, getImportDoneMarkerNs(migrationId), CollectionOptions());

    if (!status.isOK()) {
        uassertStatusOK(status.withContext(
            str::stream() << "Failed to create import done marker local collection for migration: "
                          << migrationId));
    }
}

void dropImportDoneMarkerLocalCollection(OperationContext* opCtx, const UUID& migrationId) {
    UnreplicatedWritesBlock writeBlock(opCtx);
    // Collections in 'local' db should not expect any lock or prepare conflicts.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowAcquisitionOfLocks(opCtx->lockState());

    auto status =
        StorageInterface::get(opCtx)->dropCollection(opCtx, getImportDoneMarkerNs(migrationId));

    if (!status.isOK()) {
        uassertStatusOK(status.withContext(
            str::stream() << "Failed to drop import done marker local collection for migration: "
                          << migrationId));
    }
}

void runRollbackAndThenImportFiles(OperationContext* opCtx, const UUID& migrationId) {
    if (MONGO_unlikely(skipImportFiles.shouldFail())) {
        LOGV2(7800200,
              "Skipping file import due to 'skipImportFiles' failpoint enabled",
              "migrationId"_attr = migrationId);
        return;
    }

    auto tempWTDirectory = fileClonerTempDir(migrationId);
    uassert(6113315,
            str::stream() << "Missing file cloner's temporary dbpath directory: "
                          << tempWTDirectory.string(),
            boost::filesystem::exists(tempWTDirectory));

    // TODO SERVER-63204: Evaluate correct place to remove the temporary WT dbpath.
    ON_BLOCK_EXIT([&tempWTDirectory, &migrationId] {
        LOGV2_INFO(6113324,
                   "Done importing files, removing the temporary WT dbpath",
                   "migrationId"_attr = migrationId,
                   "tempDbPath"_attr = tempWTDirectory.string());
        boost::system::error_code ec;
        boost::filesystem::remove_all(tempWTDirectory, ec);
    });

    auto metadatas =
        wiredTigerRollbackToStableAndGetMetadata(opCtx, tempWTDirectory.string(), migrationId);

    // Disable replication because this logic is executed on all nodes during a Shard Merge.
    repl::UnreplicatedWritesBlock uwb(opCtx);

    for (auto&& metadata : metadatas) {
        std::vector<std::tuple<std::string, std::string>> revertMoves;

        ScopeGuard revertFileMoves([&] {
            for (const auto& [srcFilePath, destFilePath] : revertMoves) {
                try {
                    moveFile(destFilePath, srcFilePath);
                } catch (DBException& e) {
                    LOGV2_WARNING(7199800,
                                  "Failed to move file",
                                  "from"_attr = destFilePath,
                                  "to"_attr = srcFilePath,
                                  "error"_attr = redact(e));
                }
            }
        });

        BSONObjBuilder catalogMetaBuilder;
        BSONObjBuilder storageMetaBuilder;

        // Moves the collection file and it's associated index files from temp dir to dbpath.
        // And, regenerate metadata info with new unique ident id.
        auto newCollIdent = moveWithNewIdent(opCtx,
                                             tempWTDirectory.string(),
                                             metadata.ns,
                                             metadata.collection.ident,
                                             "collection",
                                             revertMoves);

        catalogMetaBuilder.append("ident", newCollIdent);
        // Update the collection ident id.
        metadata.collection.ident = std::move(newCollIdent);
        buildStorageMetadata(metadata.collection, storageMetaBuilder);

        BSONObjBuilder newIndexIdentMap;
        for (auto&& index : metadata.indexes) {
            auto newIndexIdent = moveWithNewIdent(
                opCtx, tempWTDirectory.string(), metadata.ns, index.ident, "index", revertMoves);
            newIndexIdentMap.append(index.indexName, newIndexIdent);
            // Update the index ident id.
            index.ident = std::move(newIndexIdent);
            buildStorageMetadata(index, storageMetaBuilder);
        }

        catalogMetaBuilder.append("idxIdent", newIndexIdentMap.obj());
        metadata.catalogObject = metadata.catalogObject.addFields(catalogMetaBuilder.obj());
        const auto storageMetaObj = storageMetaBuilder.done();

        importCollectionAndItsIndexesInMainWTInstance(opCtx, metadata, migrationId, storageMetaObj);

        revertFileMoves.dismiss();
    }
}

SemiFuture<void> keepBackupCursorAlive(CancellationSource cancellationSource,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       HostAndPort hostAndPort,
                                       CursorId cursorId,
                                       NamespaceString namespaceString) {
    executor::RemoteCommandRequest getMoreRequest(
        hostAndPort,
        namespaceString.dbName(),
        std::move(BSON("getMore" << cursorId << "collection" << namespaceString.coll().toString())),
        nullptr);
    getMoreRequest.options.fireAndForget = true;

    return AsyncTry([executor, getMoreRequest, cancellationSource] {
               return executor->scheduleRemoteCommand(getMoreRequest, cancellationSource.token());
           })
        .until([](auto&&) { return false; })
        .withDelayBetweenIterations(Milliseconds(kBackupCursorKeepAliveIntervalMillis))
        .on(executor, cancellationSource.token())
        .onCompletion(
            [](auto&&) { LOGV2_INFO(7675002, "Keep backup cursor alive thread stopped"); })
        .semi();
}
}  // namespace mongo::repl::shard_merge_utils
