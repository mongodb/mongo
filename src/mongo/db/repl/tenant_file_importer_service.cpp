/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/repl/tenant_file_importer_service.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/catalog/import_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_shard_merge_util.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_file_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_import.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/cluster_parameter_synchronization_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

MONGO_FAIL_POINT_DEFINE(hangBeforeFileImporterThreadExit);
MONGO_FAIL_POINT_DEFINE(skipCloneFiles);
MONGO_FAIL_POINT_DEFINE(hangBeforeVoteImportedFiles);
MONGO_FAIL_POINT_DEFINE(skipImportFiles);
MONGO_FAIL_POINT_DEFINE(hangBeforeImportingFiles);

namespace mongo::repl {

using namespace fmt::literals;
using namespace shard_merge_utils;

namespace {
const auto _TenantFileImporterService =
    ServiceContext::declareDecoration<TenantFileImporterService>();

const ReplicaSetAwareServiceRegistry::Registerer<TenantFileImporterService>
    _TenantFileImporterServiceRegisterer("TenantFileImporterService");

template <class Promise>
void setPromiseOkifNotReady(WithLock lk, Promise& promise) {
    if (promise.getFuture().isReady()) {
        return;
    }

    promise.emplaceValue();
}

/**
 * Connect to the donor source and uses the default authentication mode.
 */
void connectAndAuth(const HostAndPort& source, DBClientConnection* client) {
    client->connect(source, "TenantFileImporterService", boost::none);
    uassertStatusOK(replAuthenticate(client).withContext(
        str::stream() << "TenantFileImporterService failed to authenticate to " << source));
}

void buildStorageMetadata(const WTimportArgs& importArgs, BSONObjBuilder& bob) {
    bob << importArgs.ident
        << BSON("tableMetadata" << importArgs.tableMetadata << "fileMetadata"
                                << importArgs.fileMetadata);
}

/**
 * Generate a new ident and move the file.
 * Performs an fsync on the destination file and the parent directories of both 'srcFilePath' and
 * 'destFilePath'.
 */
std::string fsyncMoveWithNewIdent(OperationContext* opCtx,
                                  const boost::filesystem::path& tempWTDirectory,
                                  const mongo::NamespaceString& metadataNS,
                                  const std::string& oldIdent,
                                  const char* kind,
                                  std::vector<boost::filesystem::path>& movedFiles) {
    auto srcFilePath = constructSourcePath(tempWTDirectory, oldIdent);

    while (true) {
        try {
            auto newIdent = DurableCatalog::get(opCtx)->generateUniqueIdent(metadataNS, kind);
            auto destFilePath = constructDestinationPath(newIdent);

            LOGV2_DEBUG(6114304,
                        1,
                        "Moving file",
                        "from"_attr = srcFilePath.string(),
                        "to"_attr = destFilePath.string());

            uassert(6114401,
                    "Destination file '{}' already exists"_format(destFilePath.string()),
                    !boost::filesystem::exists(destFilePath));

            writeMovingFilesMarker(
                tempWTDirectory, newIdent, (strcmp(kind, "collection") == 0 ? true : false));

            uassertStatusOK(fsyncRename(srcFilePath, destFilePath)
                                .withContext(str::stream()
                                             << "Failed to move file from: " << srcFilePath.string()
                                             << " to: " << destFilePath.string()));

            // Note the list of files to be cleaned in case of failure to import collection and it's
            // indexes.
            movedFiles.emplace_back(std::move(destFilePath));

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
 * Import the collection and its indexes into the main wiredTiger instance.
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
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
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
        shard_role_details::getRecoveryUnit(opCtx)->registerChange(
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
                                                                            *ownedCollection);
        }

        LOGV2(6114300,
              "Imported donor collection",
              "ns"_attr = nss,
              "numRecordsApprox"_attr = metadata.numRecords,
              "dataSizeApprox"_attr = metadata.dataSize);
    });
}
}  // namespace

TenantFileImporterService* TenantFileImporterService::get(ServiceContext* serviceContext) {
    return &_TenantFileImporterService(serviceContext);
}

TenantFileImporterService* TenantFileImporterService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

TenantFileImporterService::TenantFileImporterService()
    : _createConnectionFn(
          []() { return std::make_unique<DBClientConnection>(true /* autoReconnect */); }) {}

TenantFileImporterService::MigrationHandle::MigrationHandle(const UUID& migrationId,
                                                            const OpTime& startMigrationOpTime)
    : migrationId(migrationId),
      startMigrationOpTime(startMigrationOpTime),
      eventQueue(std::make_unique<Queue>()),
      workerPool(
          makeReplWorkerPool(tenantApplierThreadCount, "TenantFileImporterServiceWriter"_sd)),
      sharedData(std::make_unique<TenantMigrationSharedData>(
          getGlobalServiceContext()->getFastClockSource(), migrationId)) {
    stats.fileCopyStart = Date_t::now();
}

void TenantFileImporterService::_makeMigrationHandleIfNotPresent(
    WithLock, const UUID& migrationId, const OpTime& startMigrationOpTime) {
    if (_mh)
        return;
    _mh = std::make_unique<MigrationHandle>(migrationId, startMigrationOpTime);
}

void TenantFileImporterService::startMigration(const UUID& migrationId,
                                               const OpTime& startMigrationOpTime) {
    stdx::lock_guard lk(_mutex);
    if (_isShuttingDown) {
        LOGV2_DEBUG(6690701,
                    3,
                    "TenantFileImporterService:: Not starting migration due to shutdown",
                    "migrationId"_attr = migrationId);
        return;
    }

    _makeMigrationHandleIfNotPresent(lk, migrationId, startMigrationOpTime);
    auto prevState = _transitionToState(lk, migrationId, State::kStarted);
    if (prevState == State::kStarted)
        return;

    _mh->workerThread = std::make_unique<stdx::thread>([this, migrationId, startMigrationOpTime] {
        Client::initThread("TenantFileImporterService",
                           getGlobalServiceContext()->getService(ClusterRole::ShardServer));
        LOGV2_INFO(6378904,
                   "TenantFileImporterService worker thread started",
                   "migrationId"_attr = migrationId,
                   "startMigrationOpTime"_attr = startMigrationOpTime);

        {
            stdx::lock_guard<Client> lk(cc());
            cc().setSystemOperationUnkillableByStepdown(lk);
        }

        try {
            _handleEvents(migrationId);
        } catch (...) {
            LOGV2_ERROR(6615001,
                        "TenantFileImporterService::_handleEvents encountered an error",
                        "migrationId"_attr = migrationId,
                        "error"_attr = redact(exceptionToStatus()));
        }

        LOGV2_INFO(7800203,
                   "TenantFileImporterService worker thread exiting",
                   "migrationId"_attr = migrationId);
        hangBeforeFileImporterThreadExit.pauseWhileSet();
    });
}

void TenantFileImporterService::learnedFilename(const UUID& migrationId,
                                                const BSONObj& metadataDoc) {
    stdx::lock_guard lk(_mutex);
    // Migration handle can be empty only if the node restarts,rolls back, or resyncs while a shard
    // merge is in progress.
    if (!_mh) {
        LOGV2_DEBUG(7800204,
                    3,
                    "TenantFileImporterService:: Skipping learned filename",
                    "migrationId"_attr = migrationId,
                    "filename"_attr = metadataDoc["filename"]);
        return;
    }

    (void)_transitionToState(lk, migrationId, State::kLearnedFilename);
    _mh->stats.totalDataSize += std::max(0ll, metadataDoc["fileSize"].safeNumberLong());

    ImporterEvent event{ImporterEvent::Type::kLearnedFileName, migrationId};
    event.metadataDoc = metadataDoc.getOwned();
    auto success = _mh->eventQueue->tryPush(std::move(event));

    uassert(
        6378903,
        "TenantFileImporterService failed to push '{}' event without blocking for migrationId :{}"_format(
            stateToString(_mh->state), migrationId.toString()),
        success);
}

void TenantFileImporterService::learnedAllFilenames(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    // Migration handle can be empty only if the node restarts,rolls back, or resyncs while a shard
    // merge is in progress.
    if (!_mh) {
        LOGV2_DEBUG(7800205,
                    3,
                    "TenantFileImporterService:: Skipping learned all filenames",
                    "migrationId"_attr = migrationId);
        return;
    }

    auto prevState = _transitionToState(lk, migrationId, State::kLearnedAllFilenames);
    if (prevState == State::kLearnedAllFilenames)
        return;

    auto success =
        _mh->eventQueue->tryPush({ImporterEvent::Type::kLearnedAllFilenames, migrationId});
    uassert(
        6378902,
        "TenantFileImporterService failed to push '{}' event without blocking for migrationId :{}"_format(
            stateToString(_mh->state), migrationId.toString()),
        success);
}

void TenantFileImporterService::interruptMigration(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    // Migration handle can be empty only if the node restarts,rolls back, or resyncs while a shard
    // merge is in progress.
    if (!_mh) {
        LOGV2_DEBUG(7800206,
                    3,
                    "TenantFileImporterService:: Skipping interrupting migration",
                    "migrationId"_attr = migrationId);
        return;
    }
    _interrupt(lk, migrationId);
}

void TenantFileImporterService::resetMigration(const UUID& migrationId) {
    _resetMigrationHandle(migrationId);
}

void TenantFileImporterService::interruptAll() {
    stdx::lock_guard lk(_mutex);
    if (!_mh) {
        return;
    }
    _interrupt(lk, _mh->migrationId);
}

void TenantFileImporterService::_handleEvents(const UUID& migrationId) {
    auto uniqueOpCtx = cc().makeOperationContext();
    OperationContext* opCtx = uniqueOpCtx.get();

    std::unique_ptr<DBClientConnection> donorConnection;
    Queue* eventQueue;
    ThreadPool* workerPool;
    TenantMigrationSharedData* sharedData;

    ON_BLOCK_EXIT([this, opId = opCtx->getOpID(), &migrationId] {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);

        _mh->stats.fileCopyEnd = Date_t::now();

        _mh->opCtx = nullptr;
        _mh->donorConnection = nullptr;
    });

    {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        uassert(ErrorCodes::Interrupted,
                str::stream() << "TenantFileImporterService was interrupted for migrationId:\""
                              << migrationId << "\"",
                _mh->state < State::kInterrupted);
        _mh->opCtx = opCtx;

        eventQueue = _mh->eventQueue.get();
        workerPool = _mh->workerPool.get();
        sharedData = _mh->sharedData.get();
    }

    auto setUpDonorConnectionIfNeeded = [&](const BSONObj& metadataDoc) {
        // Return early if we have already set up the donor connection.
        if (donorConnection) {
            return;
        }

        donorConnection = _createConnectionFn();
        auto source = HostAndPort::parseThrowing(metadataDoc[kDonorHostNameFieldName].str());
        connectAndAuth(source, donorConnection.get());

        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        uassert(ErrorCodes::Interrupted,
                str::stream() << "TenantFileImporterService was interrupted for migrationId=\""
                              << migrationId << "\"",
                _mh->state < State::kInterrupted);
        _mh->donorConnection = donorConnection.get();
    };

    using eventType = ImporterEvent::Type;
    while (true) {
        opCtx->checkForInterrupt();

        auto event = eventQueue->pop(opCtx);

        // Out-of-order events for a different migration are not permitted.
        invariant(event.migrationId == migrationId);

        switch (event.type) {
            case eventType::kNone:
                continue;
            case eventType::kLearnedFileName: {
                // We won't have valid donor metadata until the first
                // 'TenantFileImporterService::learnedFilename' call, so we need to set up the
                // connection for the first kLearnedFileName event.
                setUpDonorConnectionIfNeeded(event.metadataDoc);

                _cloneFile(opCtx,
                           migrationId,
                           donorConnection.get(),
                           workerPool,
                           sharedData,
                           event.metadataDoc);
                continue;
            }
            case eventType::kLearnedAllFilenames: {
                if (MONGO_unlikely(hangBeforeImportingFiles.shouldFail())) {
                    LOGV2(8101400, "'hangBeforeImportingFiles' failpoint enabled");
                    hangBeforeImportingFiles.pauseWhileSet();
                }

                // This step prevents accidental deletion of committed donor data during startup and
                // rollback recovery.
                //
                // For example, if a migration was initially aborted and retried
                // successfully, a node restart or rollback could risk deleting committed donor data
                // during oplog replay if recovery/stable timestamp < failed migration's
                // abortOpTime. To prevent this data corruption case, a barrier is created by
                // checkpointing the startMigrationTimestamp before importing collection for the
                // ongoing migration attempt. This prevents startup/rollback recovery from
                // replaying oplog entries from various migration attempts.
                //
                // Note: Since StartMigrationTimestamp is majority committed (given that all
                // recipient state document writes are majority committed by the recipient state
                // machine), it's safe to await its checkpointing without requiring a no-op write.
                _waitUntilStartMigrationTimestampIsCheckpointed(opCtx, migrationId);

                _runRollbackAndThenImportFiles(opCtx, migrationId);
                createImportDoneMarkerLocalCollection(opCtx, migrationId);
                // Take a stable checkpoint to persist both the imported donor collections and the
                // marker collection to disk.
                opCtx->getServiceContext()->getStorageEngine()->waitUntilUnjournaledWritesDurable(
                    opCtx,
                    /*stableCheckpoint*/ true);
                _voteImportedFiles(opCtx, migrationId);
                return;
            }
        }
        MONGO_UNREACHABLE;
    }
}

void TenantFileImporterService::_cloneFile(OperationContext* opCtx,
                                           const UUID& migrationId,
                                           DBClientConnection* clientConnection,
                                           ThreadPool* workerPool,
                                           TenantMigrationSharedData* sharedData,
                                           const BSONObj& metadataDoc) {
    if (MONGO_unlikely(skipCloneFiles.shouldFail())) {
        LOGV2(7800201,
              "Skipping file cloning due to 'skipCloneFiles' failpoint enabled",
              "migrationId"_attr = migrationId);
        return;
    }

    const auto fileName = metadataDoc["filename"].str();
    const auto backupId = UUID(uassertStatusOK(UUID::parse(metadataDoc[kBackupIdFieldName])));
    const auto remoteDbpath = metadataDoc["remoteDbpath"].str();
    const size_t fileSize = std::max(0ll, metadataDoc["fileSize"].safeNumberLong());
    const auto relativePath =
        boost::filesystem::relative(fileName, metadataDoc[kDonorDbPathFieldName].str()).string();
    LOGV2_DEBUG(6113320,
                1,
                "Cloning file",
                "migrationId"_attr = migrationId,
                "metadata"_attr = metadataDoc,
                "destinationRelativePath"_attr = relativePath);
    invariant(!relativePath.empty());

    auto currentTenantFileCloner =
        std::make_unique<TenantFileCloner>(backupId,
                                           migrationId,
                                           fileName,
                                           fileSize,
                                           relativePath,
                                           sharedData,
                                           clientConnection->getServerHostAndPort(),
                                           clientConnection,
                                           repl::StorageInterface::get(cc().getServiceContext()),
                                           workerPool);

    ON_BLOCK_EXIT([this, &migrationId] {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        if (_mh->currentTenantFileCloner) {
            _mh->stats.totalBytesCopied += _mh->currentTenantFileCloner->getStats().bytesCopied;
            _mh->currentTenantFileCloner = nullptr;
        }
    });

    {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        _mh->currentTenantFileCloner = currentTenantFileCloner.get();
    }

    auto cloneStatus = currentTenantFileCloner->run();
    uassertStatusOK(cloneStatus.withContext(str::stream()
                                            << "Failed to clone file, migrationId: " << migrationId
                                            << ", fileName: " << fileName));
}


void TenantFileImporterService::_waitUntilStartMigrationTimestampIsCheckpointed(
    OperationContext* opCtx, const UUID& migrationId) {
    const auto startMigrationTs = [&] {
        stdx::lock_guard<Latch> lg(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        return _mh->startMigrationOpTime.getTimestamp();
    }();

    bool firstWait = true;
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    while (true) {
        const auto& recoveryTs = storageEngine->getLastStableRecoveryTimestamp();
        if (recoveryTs && *recoveryTs >= startMigrationTs) {
            break;
        }

        if (firstWait) {
            LOGV2_DEBUG(7458500,
                        2,
                        "Wait for start migration timestamp to be checkpointed",
                        "startMigrationTimestamp"_attr = startMigrationTs,
                        "lastCheckpointTimestamp"_attr = recoveryTs);
            firstWait = false;
        }

        // Sleep a bit so we do not keep hammering the system.
        opCtx->sleepFor(Milliseconds(100));
        opCtx->getServiceContext()->getStorageEngine()->waitUntilUnjournaledWritesDurable(
            opCtx,
            /*stableCheckpoint*/ true);
    }
}

void TenantFileImporterService::_runRollbackAndThenImportFiles(OperationContext* opCtx,
                                                               const UUID& migrationId) {
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

    ON_BLOCK_EXIT([&tempWTDirectory, &migrationId] {
        LOGV2_INFO(6113324,
                   "Done importing files, removing the temporary WT dbpath",
                   "migrationId"_attr = migrationId,
                   "tempDbPath"_attr = tempWTDirectory.string());
        fsyncRemoveDirectory(tempWTDirectory);
    });

    auto metadatas =
        wiredTigerRollbackToStableAndGetMetadata(opCtx, tempWTDirectory.string(), migrationId);

    {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        _mh->importStarted = true;
    }

    ON_BLOCK_EXIT([&] {
        stdx::lock_guard lk(_mutex);
        invariant(_mh && migrationId == _mh->migrationId);
        setPromiseOkifNotReady(lk, _mh->importCompletedPromise);
    });

    // Disable replication because this logic is executed on all nodes during a Shard Merge.
    repl::UnreplicatedWritesBlock uwb(opCtx);

    for (auto&& metadata : metadatas) {

        // Check for migration interrupt before importing the collection.
        opCtx->checkForInterrupt();

        std::vector<boost::filesystem::path> movedFiles;
        ScopeGuard removeFilesGuard([&] {
            for (const auto& filePath : movedFiles) {
                removeFile(filePath);
            }
            if (!movedFiles.empty())
                fsyncDataDirectory();
        });

        BSONObjBuilder catalogMetaBuilder;
        BSONObjBuilder storageMetaBuilder;

        // Moves the collection file and it's associated index files from temp dir to dbpath.
        // And, regenerate metadata info with new unique ident id.
        auto newCollIdent = fsyncMoveWithNewIdent(opCtx,
                                                  tempWTDirectory,
                                                  metadata.ns,
                                                  metadata.collection.ident,
                                                  "collection",
                                                  movedFiles);

        catalogMetaBuilder.append("ident", newCollIdent);
        // Update the collection ident id.
        metadata.collection.ident = std::move(newCollIdent);
        buildStorageMetadata(metadata.collection, storageMetaBuilder);

        BSONObjBuilder newIndexIdentMap;
        for (auto&& index : metadata.indexes) {
            auto newIndexIdent = fsyncMoveWithNewIdent(
                opCtx, tempWTDirectory, metadata.ns, index.ident, "index", movedFiles);
            newIndexIdentMap.append(index.indexName, newIndexIdent);
            // Update the index ident id.
            index.ident = std::move(newIndexIdent);
            buildStorageMetadata(index, storageMetaBuilder);
        }

        catalogMetaBuilder.append("idxIdent", newIndexIdentMap.obj());
        metadata.catalogObject = metadata.catalogObject.addFields(catalogMetaBuilder.obj());
        const auto storageMetaObj = storageMetaBuilder.done();

        importCollectionAndItsIndexesInMainWTInstance(opCtx, metadata, migrationId, storageMetaObj);

        removeFilesGuard.dismiss();
    }
}

void TenantFileImporterService::_voteImportedFiles(OperationContext* opCtx,
                                                   const UUID& migrationId) {
    if (MONGO_unlikely(hangBeforeVoteImportedFiles.shouldFail())) {
        LOGV2(7675000, "'hangBeforeVoteImportedFiles' failpoint enabled");
        hangBeforeVoteImportedFiles.pauseWhileSet();
    }

    // Build the command request.
    auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());
    RecipientVoteImportedFiles cmd(migrationId, replCoord->getMyHostAndPort());

    Backoff exponentialBackoff(Seconds(1), Milliseconds::max());

    while (true) {

        opCtx->checkForInterrupt();

        try {
            auto voteResponse = replCoord->runCmdOnPrimaryAndAwaitResponse(
                opCtx,
                DatabaseName::kAdmin,
                cmd.toBSON(),
                [](executor::TaskExecutor::CallbackHandle handle) {},
                [](executor::TaskExecutor::CallbackHandle handle) {});

            uassertStatusOK(getStatusFromCommandResult(voteResponse));
        } catch (DBException& ex) {
            if (ErrorCodes::isNetworkError(ex)) {
                LOGV2_INFO(7675001,
                           "Retrying 'recipientVoteImportedFiles' command",
                           "retryError"_attr = redact(ex));

                // Don't hammer the network.
                opCtx->sleepFor(exponentialBackoff.nextSleep());
                continue;
            }

            ex.addContext("Failed to run 'recipientVoteImportedFiles' command");
            throw;
        }
        break;
    }
}

void TenantFileImporterService::_interrupt(WithLock lk, const UUID& migrationId) {
    auto prevState = _transitionToState(lk, migrationId, State::kInterrupted);
    if (prevState == State::kInterrupted)
        return;

    if (_mh->donorConnection) {
        _mh->donorConnection->shutdownAndDisallowReconnect();
    }

    if (_mh->workerPool) {
        _mh->workerPool->shutdown();
    }

    if (_mh->sharedData) {
        stdx::lock_guard<TenantMigrationSharedData> sharedDatalk(*_mh->sharedData);
        // Prevent the TenantFileCloner from getting retried on retryable errors.
        _mh->sharedData->setStatusIfOK(
            sharedDatalk, Status{ErrorCodes::CallbackCanceled, "TenantFileCloner canceled"});
    }

    if (_mh->eventQueue) {
        _mh->eventQueue->closeConsumerEnd();
    }

    if (_mh->opCtx) {
        stdx::lock_guard<Client> lk(*_mh->opCtx->getClient());
        _mh->opCtx->markKilled(ErrorCodes::Interrupted);
    }

    // _runRollbackAndThenImportFiles() will fulfill the promise if importStarted is true.
    if (!_mh->importStarted) {
        setPromiseOkifNotReady(lk, _mh->importCompletedPromise);
    }
}

void TenantFileImporterService::_resetMigrationHandle(boost::optional<const UUID&> migrationId) {
    stdx::unique_lock<Latch> lk(_mutex);
    _resetCV.wait(lk, [this]() { return _resetInProgress == false; });
    if (!_mh) {
        return;
    }
    if (!migrationId) {
        migrationId = _mh->migrationId;
    }

    (void)_transitionToState(lk, migrationId.value(), State::kStopped, true /*dryRun*/);
    _resetInProgress = true;

    auto workerThread = _mh->workerThread.get();
    auto workerPool = _mh->workerPool.get();
    lk.unlock();

    LOGV2(7800207,
          "TenantFileImporterService::Waiting for worker threads to join",
          "migrationId"_attr = migrationId);
    if (workerThread && workerThread->joinable()) {
        workerThread->join();
    }

    if (workerPool) {
        workerPool->join();
    }

    lk.lock();
    (void)_transitionToState(lk, migrationId.value(), State::kStopped);
    _mh.reset();

    _resetInProgress = false;
    _resetCV.notify_all();
}

TenantFileImporterService::State TenantFileImporterService::_transitionToState(
    WithLock, const UUID& migrationId, State targetState, const bool dryRun) {
    const auto isValid = [&] {
        if (!_mh || migrationId != _mh->migrationId)
            return false;

        switch (targetState) {
            case State::kUninitialized:
                return _mh->state == State::kUninitialized;
            case State::kStarted:
                return _mh->state <= State::kStarted;
            case State::kLearnedFilename:
                return _mh->state <= State::kLearnedFilename;
            case State::kLearnedAllFilenames:
                return _mh->state == State::kLearnedFilename ||
                    _mh->state == State::kLearnedAllFilenames;
            case State::kInterrupted:
                return _mh->state <= State::kInterrupted;
            case State::kStopped:
                return _mh->state == State::kUninitialized || _mh->state >= State::kInterrupted;
            default:
                MONGO_UNREACHABLE;
        }
    }();

    std::stringstream errMsg;
    errMsg << "Failed state transition check for migrationID: " << migrationId
           << ", state: " << stateToString(targetState);
    if (_mh) {
        errMsg << ", current migrationId: " << _mh->migrationId
               << ", current state: " << stateToString(_mh->state);
    }
    uassert(7800210, errMsg.str(), isValid);

    if (dryRun)
        return _mh->state;
    if (targetState != _mh->state) {
        LOGV2(7800208,
              "TenantFileImporterService:: Transitioning state to",
              "migrationId"_attr = migrationId,
              "state"_attr = stateToString(targetState));
    }
    std::swap(_mh->state, targetState);
    return targetState;
}

boost::optional<SharedSemiFuture<void>> TenantFileImporterService::getImportCompletedFuture(
    const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    return (_mh && _mh->migrationId == migrationId)
        ? boost::make_optional(_mh->importCompletedPromise.getFuture())
        : boost::none;
}

bool TenantFileImporterService::hasActiveMigration(const UUID& migrationId) {
    stdx::lock_guard lk(_mutex);
    return (_mh && _mh->migrationId == migrationId) ? true : false;
}

BSONObj TenantFileImporterService::getStats(boost::optional<const UUID&> migrationId) {
    BSONObjBuilder bob;
    getStats(bob, migrationId);
    return bob.obj();
}

void TenantFileImporterService::getStats(BSONObjBuilder& bob,
                                         boost::optional<const UUID&> migrationId) {
    stdx::lock_guard lk(_mutex);
    if (!_mh || (migrationId && migrationId.value() != _mh->migrationId))
        return;

    bob.append("approxTotalDataSize", static_cast<long long>(_mh->stats.totalDataSize));

    auto approxTotalBytesCopied = _mh->stats.totalBytesCopied;
    if (_mh->currentTenantFileCloner) {
        approxTotalBytesCopied += _mh->currentTenantFileCloner->getStats().bytesCopied;
    }
    bob.append("approxTotalBytesCopied", static_cast<long long>(approxTotalBytesCopied));

    auto fileCopyEnd = [&]() {
        return _mh->stats.fileCopyEnd == Date_t() ? Date_t::now() : _mh->stats.fileCopyEnd;
    }();
    auto elapsedMillis =
        duration_cast<Milliseconds>(fileCopyEnd - _mh->stats.fileCopyStart).count();
    bob.append("totalReceiveElapsedMillis", static_cast<long long>(elapsedMillis));


    if (approxTotalBytesCopied > _mh->stats.totalDataSize) {
        LOGV2_ERROR(7800209,
                    "TenantFileImporterService::Bytes copied is greater than actual data size",
                    "migrationId"_attr = _mh->migrationId,
                    "totalDataSize"_attr = _mh->stats.totalDataSize,
                    "totalBytesCopied"_attr = _mh->stats.totalDataSize);
    }
    int64_t timeRemainingMillis =
        ((_mh->stats.totalDataSize - approxTotalBytesCopied) * elapsedMillis) /
        (approxTotalBytesCopied + 1);
    bob.append("remainingReceiveEstimatedMillis", static_cast<long long>(timeRemainingMillis));
}

}  // namespace mongo::repl
