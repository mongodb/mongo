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

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <fmt/format.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/cursor_server_params_gen.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/multitenancy.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/tenant_file_cloner.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


// Keep the backup cursor alive by pinging twice as often as the donor's default
// cursor timeout.
constexpr int kBackupCursorKeepAliveIntervalMillis = mongo::kCursorTimeoutMillisDefault / 2;

namespace mongo::repl::shard_merge_utils {
namespace {
using namespace fmt::literals;

void moveFile(const std::string& src, const std::string& dst) {
    LOGV2_DEBUG(6114304, 1, "Moving file", "src"_attr = src, "dst"_attr = dst);

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
 * Computes a boost::filesystem::path generic-style relative path (always uses slashes)
 * from a base path and a relative path.
 */
std::string _getPathRelativeTo(const std::string& path, const std::string& basePath) {
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
}  // namespace

void wiredTigerImportFromBackupCursor(OperationContext* opCtx,
                                      const std::vector<CollectionImportMetadata>& metadatas,
                                      const std::string& importPath) {
    for (auto&& collectionMetadata : metadatas) {
        /*
         * Move one collection file and one or more index files from temp dir to dbpath.
         */

        auto collFileSourcePath =
            constructSourcePath(importPath, collectionMetadata.importArgs.ident);
        auto collFileDestPath = constructDestinationPath(collectionMetadata.importArgs.ident);

        moveFile(collFileSourcePath, collFileDestPath);

        ScopeGuard revertCollFileMove([&] { moveFile(collFileDestPath, collFileSourcePath); });

        auto indexPaths = std::vector<std::tuple<std::string, std::string>>();
        ScopeGuard revertIndexFileMove([&] {
            for (const auto& pathTuple : indexPaths) {
                moveFile(std::get<1>(pathTuple), std::get<0>(pathTuple));
            }
        });
        for (auto&& indexImportArgs : collectionMetadata.indexes) {
            auto indexFileSourcePath = constructSourcePath(importPath, indexImportArgs.ident);
            auto indexFileDestPath = constructDestinationPath(indexImportArgs.ident);
            moveFile(indexFileSourcePath, indexFileDestPath);
            indexPaths.push_back(std::tuple(indexFileSourcePath, indexFileDestPath));
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
            opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
                Top::get(serviceContext).collectionDropped(nss);
            });

            // Create Collection object
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            auto durableCatalog = storageEngine->getCatalog();
            ImportOptions importOptions(ImportOptions::ImportCollectionUUIDOption::kKeepOld);
            importOptions.importTimestampRule = ImportOptions::ImportTimestampRule::kStable;

            auto importResult = uassertStatusOK(
                DurableCatalog::get(opCtx)->importCollection(opCtx,
                                                             collectionMetadata.ns,
                                                             collectionMetadata.catalogObject,
                                                             storageMetadata.done(),
                                                             importOptions));
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

            CollectionCatalog::get(opCtx)->onCreateCollection(opCtx, std::move(ownedCollection));
            wunit.commit();

            LOGV2(6114300,
                  "Imported donor collection",
                  "ns"_attr = nss,
                  "numRecordsApprox"_attr = collectionMetadata.numRecords,
                  "dataSizeApprox"_attr = collectionMetadata.dataSize);
        });

        revertCollFileMove.dismiss();
        revertIndexFileMove.dismiss();
    }
}

void cloneFile(OperationContext* opCtx,
               DBClientConnection* clientConnection,
               ThreadPool* writerPool,
               TenantMigrationSharedData* sharedData,
               const BSONObj& metadataDoc) {
    auto fileName = metadataDoc["filename"].str();
    auto migrationId = UUID(uassertStatusOK(UUID::parse(metadataDoc[kMigrationIdFieldName])));
    auto backupId = UUID(uassertStatusOK(UUID::parse(metadataDoc[kBackupIdFieldName])));
    auto remoteDbpath = metadataDoc["remoteDbpath"].str();
    size_t fileSize = std::max(0ll, metadataDoc["fileSize"].safeNumberLong());
    auto relativePath = _getPathRelativeTo(fileName, metadataDoc[kDonorDbPathFieldName].str());
    LOGV2_DEBUG(6113320,
                1,
                "Cloning file",
                "migrationId"_attr = migrationId,
                "metadata"_attr = metadataDoc,
                "destinationRelativePath"_attr = relativePath);
    invariant(!relativePath.empty());

    auto currentBackupFileCloner =
        std::make_unique<TenantFileCloner>(backupId,
                                           migrationId,
                                           fileName,
                                           fileSize,
                                           relativePath,
                                           sharedData,
                                           clientConnection->getServerHostAndPort(),
                                           clientConnection,
                                           repl::StorageInterface::get(cc().getServiceContext()),
                                           writerPool);

    auto cloneStatus = currentBackupFileCloner->run();
    if (!cloneStatus.isOK()) {
        LOGV2_WARNING(6113321,
                      "Failed to clone file ",
                      "migrationId"_attr = migrationId,
                      "fileName"_attr = fileName,
                      "error"_attr = cloneStatus);
    } else {
        LOGV2_DEBUG(6113322,
                    1,
                    "Cloned file",
                    "migrationId"_attr = migrationId,
                    "fileName"_attr = fileName);
    }

    uassertStatusOK(cloneStatus);
}

SemiFuture<void> keepBackupCursorAlive(CancellationSource cancellationSource,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       HostAndPort hostAndPort,
                                       CursorId cursorId,
                                       NamespaceString namespaceString) {
    executor::RemoteCommandRequest getMoreRequest(
        hostAndPort,
        namespaceString.db().toString(),
        std::move(BSON("getMore" << cursorId << "collection" << namespaceString.coll().toString())),
        nullptr);
    getMoreRequest.options.fireAndForget = true;

    return AsyncTry([executor, getMoreRequest, cancellationSource] {
               return executor->scheduleRemoteCommand(std::move(getMoreRequest),
                                                      cancellationSource.token());
           })
        .until([](auto&&) { return false; })
        .withDelayBetweenIterations(Milliseconds(kBackupCursorKeepAliveIntervalMillis))
        .on(executor, cancellationSource.token())
        .onCompletion([](auto&&) {})
        .semi();
}
}  // namespace mongo::repl::shard_merge_utils
