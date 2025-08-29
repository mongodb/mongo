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


#include "mongo/db/storage/storage_engine_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/storage/backup_cursor_hooks.h"
#include "mongo/db/storage/deferred_drop_record_store.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>

#ifdef _WIN32
#define NVALGRIND
#endif

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/vector.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <valgrind/valgrind.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


#define LOGV2_FOR_RECOVERY(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(ID, DLEVEL, {logv2::LogComponent::kStorageRecovery}, MESSAGE, ##__VA_ARGS__)

namespace mongo {

using std::string;
using std::vector;

MONGO_FAIL_POINT_DEFINE(pauseTimestampMonitor);

namespace {
const auto kCatalogLogLevel = logv2::LogSeverity::Debug(2);

// Returns true if the ident refers to a resumable index build table.
bool isResumableIndexBuildIdent(StringData ident) {
    return ident::isInternalIdent(ident, kResumableIndexIdentStem);
}

// Idents corresponding to resumable index build tables are encoded as an 'internal' table and
// tagged 'kResumableIndexIdentStem'.
// Generates an ident to unique identify a new resumable index build table.
std::string generateNewResumableIndexBuildIdent() {
    const auto resumableIndexIdent = ident::generateNewInternalIdent(kResumableIndexIdentStem);
    invariant(isResumableIndexBuildIdent(resumableIndexIdent));
    return resumableIndexIdent;
}
}  // namespace

StorageEngineImpl::StorageEngineImpl(OperationContext* opCtx,
                                     std::unique_ptr<KVEngine> engine,
                                     std::unique_ptr<KVEngine> spillEngine,
                                     StorageEngineOptions options)
    : _engine(std::move(engine)),
      _spillEngine(std::move(spillEngine)),
      _options(std::move(options)),
      _dropPendingIdentReaper(_engine.get()),
      _minOfCheckpointAndOldestTimestampListener(
          TimestampMonitor::TimestampType::kMinOfCheckpointAndOldest,
          [this](OperationContext* opCtx, Timestamp timestamp) {
              _onMinOfCheckpointAndOldestTimestampChanged(opCtx, timestamp);
          }),
      _supportsCappedCollections(_engine->supportsCappedCollections()) {

    // Replace the noop recovery unit for the startup operation context now that the storage engine
    // has been initialized. This is needed because at the time of startup, when the operation
    // context gets created, the storage engine initialization has not yet begun and so it gets
    // assigned a noop recovery unit. See the StorageClientObserver class.
    auto prevRecoveryUnit = shard_role_details::releaseRecoveryUnit(opCtx);
    invariant(prevRecoveryUnit->isNoop());
    shard_role_details::setRecoveryUnit(
        opCtx, _engine->newRecoveryUnit(), WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    auto& rss = rss::ReplicatedStorageService::get(opCtx->getServiceContext());
    if (rss.getPersistenceProvider().shouldDelayDataAccessDuringStartup()) {
        LOGV2(10985326,
              "Skip loading catalog on startup; it will be handled later when WT loads the "
              "checkpoint");
        return;
    }

    // If we throw in this constructor, make sure to destroy the RecoveryUnit instance created above
    // before '_engine' is destroyed.
    ScopeGuard recoveryUnitResetGuard([&] {
        shard_role_details::setRecoveryUnit(opCtx,
                                            std::move(prevRecoveryUnit),
                                            WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
    });

    // If we are loading the catalog after an unclean shutdown, it's possible that there are
    // collections in the catalog that are unknown to the storage engine. We should attempt to
    // recover these orphaned idents.
    // Allowing locking in write mode as reinitializeStorageEngine will be called while holding the
    // global lock in exclusive mode.
    invariant(!shard_role_details::getLocker(opCtx)->isLocked() ||
              shard_role_details::getLocker(opCtx)->isW());
    Lock::GlobalWrite globalLk(opCtx);
    loadMDBCatalog(opCtx,
                   _options.lockFileCreatedByUncleanShutdown ? LastShutdownState::kUnclean
                                                             : LastShutdownState::kClean);

    // We can dismiss recoveryUnitResetGuard now.
    recoveryUnitResetGuard.dismiss();
}

void StorageEngineImpl::loadMDBCatalog(OperationContext* opCtx,
                                       LastShutdownState lastShutdownState) {
    bool catalogExists =
        _engine->hasIdent(*shard_role_details::getRecoveryUnit(opCtx), ident::kMbdCatalog);
    if (_options.forRepair && catalogExists) {
        auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
        invariant(repairObserver->isIncomplete());

        LOGV2(22246, "Repairing catalog metadata");
        Status status =
            _engine->repairIdent(*shard_role_details::getRecoveryUnit(opCtx), ident::kMbdCatalog);

        if (status.code() == ErrorCodes::DataModifiedByRepair) {
            LOGV2_WARNING(22264, "Catalog data modified by repair", "error"_attr = status);
            repairObserver->invalidatingModification(str::stream()
                                                     << "MDBCatalog repaired: " << status.reason());
        } else {
            fassertNoTrace(50926, status);
        }
    }

    // The '_mdb_' catalog is generated and retrieved with a default 'RecordStore' configuration.
    // This maintains current and earlier behavior of a MongoD.
    const auto catalogRecordStoreOpts = RecordStore::Options{};
    if (!catalogExists) {
        WriteUnitOfWork uow(opCtx);

        auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
        auto status = _engine->createRecordStore(
            provider, kCatalogInfoNamespace, ident::kMbdCatalog, catalogRecordStoreOpts);

        // BadValue is usually caused by invalid configuration string.
        // We still fassert() but without a stack trace.
        if (status.code() == ErrorCodes::BadValue) {
            fassertFailedNoTrace(28562);
        }
        fassert(28520, status);
        uow.commit();
    }

    _catalogRecordStore = _engine->getRecordStore(opCtx,
                                                  kCatalogInfoNamespace,
                                                  ident::kMbdCatalog,
                                                  catalogRecordStoreOpts,
                                                  boost::none /* uuid */);

    if (shouldLog(::mongo::logv2::LogComponent::kStorageRecovery, kCatalogLogLevel)) {
        LOGV2_FOR_RECOVERY(4615631, kCatalogLogLevel.toInt(), "loadMDBCatalog:");
        _dumpCatalog(opCtx);
    }

    LOGV2(9529901,
          "Initializing durable catalog",
          "numRecords"_attr = _catalogRecordStore->numRecords());
    _catalog = std::make_unique<MDBCatalog>(_catalogRecordStore.get(), _engine.get());
    _catalog->init(opCtx);

    LOGV2(9529902, "Retrieving all idents from storage engine");
    std::vector<std::string> identsKnownToStorageEngine =
        _engine->getAllIdents(*shard_role_details::getRecoveryUnit(opCtx));
    std::sort(identsKnownToStorageEngine.begin(), identsKnownToStorageEngine.end());

    std::vector<MDBCatalog::EntryIdentifier> catalogEntries = _catalog->getAllCatalogEntries(opCtx);

    // Perform a read on the catalog at the `oldestTimestamp` and record the record stores (via
    // their catalogId) that existed.
    std::set<RecordId> existedAtOldestTs;
    if (!_engine->getOldestTimestamp().isNull()) {
        ReadSourceScope snapshotScope(
            opCtx, RecoveryUnit::ReadSource::kProvided, _engine->getOldestTimestamp());
        auto entriesAtOldest = _catalog->getAllCatalogEntries(opCtx);
        LOGV2_FOR_RECOVERY(5380110,
                           kCatalogLogLevel.toInt(),
                           "Catalog entries at the oldest timestamp",
                           "oldestTimestamp"_attr = _engine->getOldestTimestamp());
        for (const auto& entry : entriesAtOldest) {
            existedAtOldestTs.insert(entry.catalogId);
            LOGV2_FOR_RECOVERY(5380109,
                               kCatalogLogLevel.toInt(),
                               "Historical entry",
                               "catalogId"_attr = entry.catalogId,
                               "ident"_attr = entry.ident,
                               logAttrs(entry.nss));
        }
    }

    if (_options.forRepair) {
        // It's possible that there are collection files on disk that are unknown to the catalog. In
        // a repair context, if we can't find an ident in the catalog, we generate a catalog entry
        // 'local.orphan.xxxxx' for it. However, in a nonrepair context, the orphaned idents
        // will be dropped in catalog::reconcileCatalogAndIdents().
        for (const auto& ident : identsKnownToStorageEngine) {
            if (ident::isCollectionIdent(ident)) {
                bool isOrphan = !std::any_of(catalogEntries.begin(),
                                             catalogEntries.end(),
                                             [this, &ident](MDBCatalog::EntryIdentifier entry) {
                                                 return entry.ident == ident;
                                             });
                if (isOrphan) {
                    // If the catalog does not have information about this
                    // collection, we create an new entry for it.
                    WriteUnitOfWork wuow(opCtx);

                    auto keyFormat =
                        _engine->getKeyFormat(*shard_role_details::getRecoveryUnit(opCtx), ident);
                    // TODO SERVER-105436 investigate usage of isClustered
                    bool isClustered = keyFormat == KeyFormat::String;
                    StatusWith<std::string> statusWithNs =
                        _catalog->newOrphanedIdent(opCtx, ident, isClustered);

                    if (statusWithNs.isOK()) {
                        wuow.commit();
                        auto orphanCollNs = statusWithNs.getValue();
                        LOGV2(22247,
                              "Successfully created an entry in the catalog for orphaned "
                              "collection",
                              "namespace"_attr = orphanCollNs,
                              "keyFormat"_attr = keyFormat);

                        if (!isClustered) {
                            // The _id index is already implicitly created on collections clustered
                            // by _id.
                            LOGV2_WARNING(22265,
                                          "Collection does not have an _id index. Please manually "
                                          "build the index",
                                          "namespace"_attr = orphanCollNs);
                        }
                        StorageRepairObserver::get(getGlobalServiceContext())
                            ->benignModification(str::stream() << "Orphan collection created: "
                                                               << statusWithNs.getValue());

                    } else {
                        // Log an error message if we cannot create the entry.
                        // catalog::reconcileCatalogAndIdents() will later drop this ident.
                        LOGV2_ERROR(
                            22268,
                            "Cannot create an entry in the catalog for orphaned ident. Restarting "
                            "the server will remove this ident",
                            "ident"_attr = ident,
                            "error"_attr = statusWithNs.getStatus());
                    }
                }
            }
        }
    }

    const auto loadingFromUncleanShutdownOrRepair =
        lastShutdownState == LastShutdownState::kUnclean || _options.forRepair;

    LOGV2(9529903,
          "Initializing all collections in durable catalog",
          "numEntries"_attr = catalogEntries.size());
    for (MDBCatalog::EntryIdentifier entry : catalogEntries) {
        if (_options.forRestore) {
            // When restoring a subset of user collections from a backup, the collections not
            // restored are in the catalog but are unknown to the storage engine. The catalog
            // entries for these collections will be removed.
            const auto collectionIdent = entry.ident;
            bool restoredIdent = std::binary_search(identsKnownToStorageEngine.begin(),
                                                    identsKnownToStorageEngine.end(),
                                                    collectionIdent);

            if (!restoredIdent) {
                LOGV2(6260800,
                      "Removing catalog entry for collection not restored",
                      logAttrs(entry.nss),
                      "ident"_attr = collectionIdent);

                WriteUnitOfWork wuow(opCtx);
                fassert(6260801, _catalog->removeEntry(opCtx, entry.catalogId));
                wuow.commit();

                continue;
            }

            // A collection being restored needs to also restore all of its indexes.
            _checkForIndexFiles(opCtx, entry, identsKnownToStorageEngine);
        }

        if (loadingFromUncleanShutdownOrRepair) {
            // If we are loading the catalog after an unclean shutdown or during repair, it's
            // possible that there are collections in the catalog that are unknown to the storage
            // engine. If we can't find a table in the list of storage engine idents, either
            // attempt to recover the ident or drop it.
            const auto collectionIdent = entry.ident;
            bool orphan = !std::binary_search(identsKnownToStorageEngine.begin(),
                                              identsKnownToStorageEngine.end(),
                                              collectionIdent);
            // If the storage engine is missing a collection and is unable to create a new record
            // store, drop it from the catalog and skip initializing it by continuing past the
            // following logic.
            if (orphan) {
                auto status =
                    _recoverOrphanedCollection(opCtx, entry.catalogId, entry.nss, collectionIdent);
                if (!status.isOK()) {
                    LOGV2_WARNING(22266,
                                  "Failed to recover orphaned data file for collection",
                                  logAttrs(entry.nss),
                                  "error"_attr = status);
                    WriteUnitOfWork wuow(opCtx);
                    fassert(50716, _catalog->removeEntry(opCtx, entry.catalogId));

                    if (_options.forRepair) {
                        StorageRepairObserver::get(getGlobalServiceContext())
                            ->invalidatingModification(
                                str::stream() << "Collection " << entry.nss.toStringForErrorMsg()
                                              << " dropped: " << status.reason());
                    }
                    wuow.commit();
                    continue;
                }
            }
        }

        if (!entry.nss.isReplicated() &&
            !std::binary_search(identsKnownToStorageEngine.begin(),
                                identsKnownToStorageEngine.end(),
                                entry.ident)) {
            // All collection drops are non-transactional and unreplicated collections are dropped
            // immediately as they do not use two-phase drops. It's possible to run into a situation
            // where there are collections in the catalog that are unknown to the storage engine
            // after restoring from backed up data files. See SERVER-55552.
            WriteUnitOfWork wuow(opCtx);
            fassert(5555200, _catalog->removeEntry(opCtx, entry.catalogId));
            wuow.commit();

            LOGV2_INFO(5555201,
                       "Removed unknown unreplicated collection from the catalog",
                       "catalogId"_attr = entry.catalogId,
                       logAttrs(entry.nss),
                       "ident"_attr = entry.ident);
            continue;
        }

        if (entry.nss.isOrphanCollection()) {
            LOGV2(22248, "Orphaned collection found", logAttrs(entry.nss));
        }
    }

    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
}

void StorageEngineImpl::closeMDBCatalog(OperationContext* opCtx) {
    dassert(shard_role_details::getLocker(opCtx)->isLocked());
    if (shouldLog(::mongo::logv2::LogComponent::kStorageRecovery, kCatalogLogLevel)) {
        LOGV2_FOR_RECOVERY(4615632, kCatalogLogLevel.toInt(), "closeMDBCatalog:");
        _dumpCatalog(opCtx);
    }

    _catalog.reset();
    _catalogRecordStore.reset();
}

Status StorageEngineImpl::_recoverOrphanedCollection(OperationContext* opCtx,
                                                     RecordId catalogId,
                                                     const NamespaceString& collectionName,
                                                     StringData collectionIdent) {
    if (!_options.forRepair) {
        return {ErrorCodes::IllegalOperation, "Orphan recovery only supported in repair"};
    }
    LOGV2(22249,
          "Storage engine is missing collection from its metadata. Attempting to locate and "
          "recover the data",
          logAttrs(collectionName),
          "ident"_attr = collectionIdent);

    WriteUnitOfWork wuow(opCtx);
    const auto recordStoreOptions =
        _catalog->getParsedRecordStoreOptions(opCtx, catalogId, collectionName);
    auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
    Status status = _engine->recoverOrphanedIdent(
        provider, collectionName, collectionIdent, recordStoreOptions);

    bool dataModified = status.code() == ErrorCodes::DataModifiedByRepair;
    if (!status.isOK() && !dataModified) {
        return status;
    }
    if (dataModified) {
        StorageRepairObserver::get(getGlobalServiceContext())
            ->invalidatingModification(str::stream()
                                       << "Collection " << collectionName.toStringForErrorMsg()
                                       << " recovered: " << status.reason());
    }
    wuow.commit();
    return Status::OK();
}

void StorageEngineImpl::_checkForIndexFiles(
    OperationContext* opCtx,
    const MDBCatalog::EntryIdentifier& entry,
    std::vector<std::string>& identsKnownToStorageEngine) const {
    std::vector<std::string> indexIdents = _catalog->getIndexIdents(opCtx, entry.catalogId);
    for (const std::string& indexIdent : indexIdents) {
        bool restoredIndexIdent = std::binary_search(
            identsKnownToStorageEngine.begin(), identsKnownToStorageEngine.end(), indexIdent);

        if (restoredIndexIdent) {
            continue;
        }

        LOGV2_FATAL_NOTRACE(6261000,
                            "Collection is missing an index file",
                            logAttrs(entry.nss),
                            "collectionIdent"_attr = entry.ident,
                            "missingIndexIdent"_attr = indexIdent);
    }
}

std::string StorageEngineImpl::getFilesystemPathForDb(const DatabaseName& dbName) const {
    if (_options.directoryPerDB) {
        return storageGlobalParams.dbpath + '/' + ident::createDBNamePathComponent(dbName);
    } else {
        return storageGlobalParams.dbpath;
    }
}

std::string StorageEngineImpl::generateNewCollectionIdent(const DatabaseName& dbName) const {
    return ident::generateNewCollectionIdent(
        dbName, _options.directoryPerDB, _options.directoryForIndexes);
}

std::string StorageEngineImpl::generateNewIndexIdent(const DatabaseName& dbName) const {
    return ident::generateNewIndexIdent(
        dbName, _options.directoryPerDB, _options.directoryForIndexes);
}

void StorageEngineImpl::cleanShutdown(ServiceContext* svcCtx, bool memLeakAllowed) {
    _timestampMonitor.reset();

    _catalog.reset();
    _catalogRecordStore.reset();

#if __has_feature(address_sanitizer)
    memLeakAllowed = false;
#endif
    if (RUNNING_ON_VALGRIND) {  // NOLINT
        memLeakAllowed = false;
    }

    if (_spillEngine) {
        _spillEngine->cleanShutdown(memLeakAllowed);
    }

    _engine->cleanShutdown(memLeakAllowed);
    // intentionally not deleting _engine
}

StorageEngineImpl::~StorageEngineImpl() {}

void StorageEngineImpl::startTimestampMonitor(
    std::initializer_list<TimestampMonitor::TimestampListener*> listeners) {
    // Unless explicitly disabled, all storage engines should create a TimestampMonitor for
    // drop-pending internal idents, even if they do not support pending drops for collections
    // and indexes.
    _timestampMonitor = std::make_unique<TimestampMonitor>(
        _engine.get(), getGlobalServiceContext()->getPeriodicRunner());

    _timestampMonitor->addListener(&_minOfCheckpointAndOldestTimestampListener);

    // Caller must provide listener for cleanup of CollectionCatalog when oldest timestamp advances.
    invariant(!std::empty(listeners));
    for (auto listener : listeners) {
        _timestampMonitor->addListener(listener);
    }
}

void StorageEngineImpl::stopTimestampMonitor() {
    _listeners = _timestampMonitor->getListeners();
    _timestampMonitor.reset();
}

void StorageEngineImpl::restartTimestampMonitor() {
    _timestampMonitor = std::make_unique<TimestampMonitor>(
        _engine.get(), getGlobalServiceContext()->getPeriodicRunner());

    invariant(!std::empty(_listeners));
    for (auto listener : _listeners) {
        _timestampMonitor->addListener(listener);
    }
    _listeners.clear();
}

void StorageEngineImpl::notifyStorageStartupRecoveryComplete() {
    _engine->notifyStorageStartupRecoveryComplete();
}

void StorageEngineImpl::notifyReplStartupRecoveryComplete(RecoveryUnit& ru) {
    _engine->notifyReplStartupRecoveryComplete(ru);
}

void StorageEngineImpl::setInStandaloneMode() {
    _engine->setInStandaloneMode();
}

std::unique_ptr<RecoveryUnit> StorageEngineImpl::newRecoveryUnit() {
    if (!_engine) {
        // shutdown
        return nullptr;
    }
    return _engine->newRecoveryUnit();
}

void StorageEngineImpl::flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) {
    _engine->flushAllFiles(opCtx, callerHoldsReadLock);
}

Status StorageEngineImpl::beginBackup() {
    // We should not proceed if we are already in backup mode
    if (_inBackupMode)
        return Status(ErrorCodes::BadValue, "Already in Backup Mode");
    Status status = _engine->beginBackup();
    if (status.isOK())
        _inBackupMode = true;
    return status;
}

void StorageEngineImpl::endBackup() {
    // We should never reach here if we aren't already in backup mode
    invariant(_inBackupMode);
    _engine->endBackup();
    _inBackupMode = false;
}

Timestamp StorageEngineImpl::getBackupCheckpointTimestamp() {
    return _engine->getBackupCheckpointTimestamp();
}

Status StorageEngineImpl::disableIncrementalBackup() {
    LOGV2(9538600, "Disabling incremental backup");
    return _engine->disableIncrementalBackup();
}

StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>>
StorageEngineImpl::beginNonBlockingBackup(const StorageEngine::BackupOptions& options) {
    return _engine->beginNonBlockingBackup(options);
}

void StorageEngineImpl::endNonBlockingBackup() {
    return _engine->endNonBlockingBackup();
}

StatusWith<std::deque<std::string>> StorageEngineImpl::extendBackupCursor() {
    return _engine->extendBackupCursor();
}

bool StorageEngineImpl::supportsCheckpoints() const {
    return _engine->supportsCheckpoints();
}

bool StorageEngineImpl::isEphemeral() const {
    return _engine->isEphemeral();
}

SnapshotManager* StorageEngineImpl::getSnapshotManager() const {
    return _engine->getSnapshotManager();
}

Status StorageEngineImpl::repairRecordStore(OperationContext* opCtx,
                                            RecordId catalogId,
                                            const NamespaceString& nss) {
    auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
    invariant(repairObserver->isIncomplete());

    Status status = _engine->repairIdent(*shard_role_details::getRecoveryUnit(opCtx),
                                         _catalog->getEntry(catalogId).ident);
    bool dataModified = status.code() == ErrorCodes::DataModifiedByRepair;
    if (!status.isOK() && !dataModified) {
        return status;
    }

    if (dataModified) {
        repairObserver->invalidatingModification(
            str::stream() << "Collection " << nss.toStringForErrorMsg() << ": " << status.reason());
    }

    return status;
}

std::unique_ptr<SpillTable> StorageEngineImpl::makeSpillTable(OperationContext* opCtx,
                                                              KeyFormat keyFormat,
                                                              int64_t thresholdBytes) {
    auto ru = _spillEngine->newRecoveryUnit();
    auto rs =
        _spillEngine->makeTemporaryRecordStore(*ru, ident::generateNewInternalIdent(), keyFormat);
    LOGV2_DEBUG(10380301, 1, "Created spill table", "ident"_attr = rs->getIdent());

    return std::make_unique<SpillTable>(std::move(ru),
                                        std::move(rs),
                                        *this,
                                        *DiskSpaceMonitor::get(opCtx->getServiceContext()),
                                        thresholdBytes);
}

void StorageEngineImpl::dropSpillTable(RecoveryUnit& ru, StringData ident) {
    // Dropping the spill table may transiently return ObjectIsBusy if another spill engine user has
    // a storage snapshot from before an earlier write to this table. Retry until the drop succeeds.
    for (size_t retries = 0;; ++retries) {
        auto status = _spillEngine->dropIdent(ru,
                                              ident,
                                              false, /* identHasSizeInfo */
                                              nullptr /* onDrop */);
        if (status.isOK()) {
            return;
        }
        if (status != ErrorCodes::ObjectIsBusy) {
            uassertStatusOK(status);
        }

        logAndBackoff(10327300,
                      logv2::LogComponent::kStorage,
                      logv2::LogSeverity::Log(),
                      retries,
                      "Failed to drop spill table, retrying",
                      "error"_attr = status);
    }
}

std::unique_ptr<TemporaryRecordStore> StorageEngineImpl::makeTemporaryRecordStore(
    OperationContext* opCtx, StringData ident, KeyFormat keyFormat) {
    tassert(10709200,
            "Cannot use a non-internal ident to create a temporary RecordStore instance",
            ident::isInternalIdent(ident));

    std::unique_ptr<RecordStore> rs = _engine->makeTemporaryRecordStore(
        *shard_role_details::getRecoveryUnit(opCtx), ident, keyFormat);
    LOGV2_DEBUG(22258, 1, "Created temporary record store", "ident"_attr = rs->getIdent());
    return std::make_unique<DeferredDropRecordStore>(std::move(rs), this);
}

std::unique_ptr<TemporaryRecordStore>
StorageEngineImpl::makeTemporaryRecordStoreForResumableIndexBuild(OperationContext* opCtx,
                                                                  KeyFormat keyFormat) {
    std::unique_ptr<RecordStore> rs =
        _engine->makeTemporaryRecordStore(*shard_role_details::getRecoveryUnit(opCtx),
                                          generateNewResumableIndexBuildIdent(),
                                          keyFormat);
    LOGV2_DEBUG(4921500,
                1,
                "Created temporary record store for resumable index build",
                "ident"_attr = rs->getIdent());
    return std::make_unique<DeferredDropRecordStore>(std::move(rs), this);
}

std::unique_ptr<TemporaryRecordStore> StorageEngineImpl::makeTemporaryRecordStoreFromExistingIdent(
    OperationContext* opCtx, StringData ident, KeyFormat keyFormat) {
    auto rs = _engine->getTemporaryRecordStore(
        *shard_role_details::getRecoveryUnit(opCtx), ident, keyFormat);
    return std::make_unique<DeferredDropRecordStore>(std::move(rs), this);
}

void StorageEngineImpl::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}

void StorageEngineImpl::setLastMaterializedLsn(uint64_t lsn) {
    _engine->setLastMaterializedLsn(lsn);
}

void StorageEngineImpl::setRecoveryCheckpointMetadata(StringData checkpointMetadata) {
    _engine->setRecoveryCheckpointMetadata(checkpointMetadata);
}

void StorageEngineImpl::promoteToLeader() {
    _engine->promoteToLeader();
}

void StorageEngineImpl::setStableTimestamp(Timestamp stableTimestamp, bool force) {
    _engine->setStableTimestamp(stableTimestamp, force);
}

Timestamp StorageEngineImpl::getStableTimestamp() const {
    return _engine->getStableTimestamp();
}

void StorageEngineImpl::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    _engine->setInitialDataTimestamp(initialDataTimestamp);
}

Timestamp StorageEngineImpl::getInitialDataTimestamp() const {
    return _engine->getInitialDataTimestamp();
}

void StorageEngineImpl::setOldestTimestampFromStable() {
    _engine->setOldestTimestampFromStable();
}

void StorageEngineImpl::setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {
    _engine->setOldestTimestamp(newOldestTimestamp, force);
}

Timestamp StorageEngineImpl::getOldestTimestamp() const {
    return _engine->getOldestTimestamp();
};

void StorageEngineImpl::setOldestActiveTransactionTimestampCallback(
    StorageEngine::OldestActiveTransactionTimestampCallback callback) {
    _engine->setOldestActiveTransactionTimestampCallback(callback);
}

bool StorageEngineImpl::supportsRecoverToStableTimestamp() const {
    return _engine->supportsRecoverToStableTimestamp();
}

bool StorageEngineImpl::supportsRecoveryTimestamp() const {
    return _engine->supportsRecoveryTimestamp();
}

StatusWith<Timestamp> StorageEngineImpl::recoverToStableTimestamp(OperationContext* opCtx) {
    invariant(shard_role_details::getLocker(opCtx)->isW());

    // SERVER-58311: Reset the recovery unit to unposition storage engine cursors. This allows WT to
    // assert it has sole access when performing rollback_to_stable().
    shard_role_details::replaceRecoveryUnit(opCtx);

    StatusWith<Timestamp> swTimestamp = _engine->recoverToStableTimestamp(*opCtx);
    if (!swTimestamp.isOK()) {
        return swTimestamp;
    }

    LOGV2(22259,
          "recoverToStableTimestamp successful",
          "stableTimestamp"_attr = swTimestamp.getValue());
    return {swTimestamp.getValue()};
}

boost::optional<Timestamp> StorageEngineImpl::getRecoveryTimestamp() const {
    return _engine->getRecoveryTimestamp();
}

boost::optional<Timestamp> StorageEngineImpl::getLastStableRecoveryTimestamp() const {
    return _engine->getLastStableRecoveryTimestamp();
}

bool StorageEngineImpl::supportsReadConcernSnapshot() const {
    return _engine->supportsReadConcernSnapshot();
}

void StorageEngineImpl::clearDropPendingState(OperationContext* opCtx) {
    _dropPendingIdentReaper.clearDropPendingState(opCtx);
}

Status StorageEngineImpl::immediatelyCompletePendingDrop(OperationContext* opCtx,
                                                         StringData ident) {
    return _dropPendingIdentReaper.immediatelyCompletePendingDrop(opCtx, ident);
}

Timestamp StorageEngineImpl::getAllDurableTimestamp() const {
    return _engine->getAllDurableTimestamp();
}

boost::optional<Timestamp> StorageEngineImpl::getOplogNeededForCrashRecovery() const {
    return _engine->getOplogNeededForCrashRecovery();
}

Timestamp StorageEngineImpl::getPinnedOplog() const {
    return _engine->getPinnedOplog();
}

void StorageEngineImpl::_dumpCatalog(OperationContext* opCtx) {
    auto catalogRs = _catalogRecordStore.get();
    auto cursor = catalogRs->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    boost::optional<Record> rec = cursor->next();
    stdx::unordered_set<std::string> nsMap;
    while (rec) {
        // This should only be called by a parent that's done an appropriate `shouldLog` check. Do
        // not duplicate the log level policy.
        LOGV2_FOR_RECOVERY(4615634,
                           kCatalogLogLevel.toInt(),
                           "Catalog entry",
                           "catalogId"_attr = rec->id,
                           "value"_attr = rec->data.toBson());
        auto valueBson = rec->data.toBson();
        if (valueBson.hasField("md")) {
            std::string ns = valueBson.getField("md").Obj().getField("ns").String();
            invariant(!nsMap.count(ns), str::stream() << "Found duplicate namespace: " << ns);
            nsMap.insert(ns);
        }
        rec = cursor->next();
    }
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
}

void StorageEngineImpl::addDropPendingIdent(
    const std::variant<Timestamp, StorageEngine::CheckpointIteration>& dropTime,
    std::shared_ptr<Ident> ident,
    DropIdentCallback&& onDrop) {
    _dropPendingIdentReaper.addDropPendingIdent(dropTime, ident, std::move(onDrop));
}

std::shared_ptr<Ident> StorageEngineImpl::markIdentInUse(StringData ident) {
    return _dropPendingIdentReaper.markIdentInUse(ident);
}

void StorageEngineImpl::checkpoint() {
    _engine->checkpoint();
}

StorageEngine::CheckpointIteration StorageEngineImpl::getCheckpointIteration() const {
    return _engine->getCheckpointIteration();
}

bool StorageEngineImpl::hasDataBeenCheckpointed(
    StorageEngine::CheckpointIteration checkpointIteration) const {
    return _engine->hasDataBeenCheckpointed(checkpointIteration);
}

void StorageEngineImpl::_onMinOfCheckpointAndOldestTimestampChanged(OperationContext* opCtx,
                                                                    const Timestamp& timestamp) {
    if (_dropPendingIdentReaper.hasExpiredIdents(timestamp)) {
        LOGV2(22260,
              "Removing drop-pending idents with drop timestamps before timestamp",
              "timestamp"_attr = timestamp);

        _dropPendingIdentReaper.dropIdentsOlderThan(opCtx, timestamp);
    } else {
        LOGV2_DEBUG(8097401,
                    1,
                    "No drop-pending idents have expired",
                    "timestamp"_attr = timestamp,
                    "pendingIdentsCount"_attr = _dropPendingIdentReaper.getNumIdents());
    }
}

StorageEngineImpl::TimestampMonitor::TimestampMonitor(KVEngine* engine, PeriodicRunner* runner)
    : _engine(engine), _periodicRunner(runner) {
    _startup();
}

StorageEngineImpl::TimestampMonitor::~TimestampMonitor() {
    LOGV2(22261, "Timestamp monitor shutting down");
}

void StorageEngineImpl::TimestampMonitor::_startup() {
    invariant(!_running);

    LOGV2(22262, "Timestamp monitor starting");
    PeriodicRunner::PeriodicJob job(
        "TimestampMonitor",
        [&](Client* client) {
            if (MONGO_unlikely(pauseTimestampMonitor.shouldFail())) {
                LOGV2(6321800,
                      "Pausing the timestamp monitor due to the pauseTimestampMonitor fail point");
                pauseTimestampMonitor.pauseWhileSet();
            }

            {
                stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
                if (_listeners.empty()) {
                    return;
                }
            }

            try {
                auto uniqueOpCtx = client->makeOperationContext();
                auto opCtx = uniqueOpCtx.get();

                auto backupCursorHooks = BackupCursorHooks::get(opCtx->getServiceContext());
                if (backupCursorHooks->isBackupCursorOpen()) {
                    LOGV2_DEBUG(9810500, 1, "Backup in progress, skipping table drops.");
                    return;
                }

                // The TimestampMonitor is an important background cleanup task for the storage
                // engine and needs to be able to make progress to free up resources.
                ScopedAdmissionPriority<ExecutionAdmissionContext> immediatePriority(
                    opCtx, AdmissionContext::Priority::kExempt);

                // The checkpoint timestamp is not cached in mongod and needs to be fetched with
                // a call into WiredTiger, all the other timestamps are cached in mongod.
                Timestamp checkpoint = _engine->getCheckpointTimestamp();
                Timestamp oldest = _engine->getOldestTimestamp();
                Timestamp stable = _engine->getStableTimestamp();
                Timestamp minOfCheckpointAndOldest =
                    (checkpoint.isNull() || (checkpoint > oldest)) ? oldest : checkpoint;

                {
                    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
                    for (const auto& listener : _listeners) {
                        if (listener->getType() == TimestampType::kCheckpoint) {
                            listener->notify(opCtx, checkpoint);
                        } else if (listener->getType() == TimestampType::kOldest) {
                            listener->notify(opCtx, oldest);
                        } else if (listener->getType() == TimestampType::kStable) {
                            listener->notify(opCtx, stable);
                        } else if (listener->getType() ==
                                   TimestampType::kMinOfCheckpointAndOldest) {
                            listener->notify(opCtx, minOfCheckpointAndOldest);
                        } else if (stable == Timestamp::min()) {
                            // Special case notification of all listeners when writes do not
                            // have timestamps. This handles standalone mode and storage engines
                            // that don't support timestamps.
                            listener->notify(opCtx, Timestamp::min());
                        }
                    }
                }

            } catch (const ExceptionFor<ErrorCodes::Interrupted>&) {
                LOGV2(6183600, "Timestamp monitor got interrupted, retrying");
                return;
            } catch (const ExceptionFor<ErrorCodes::InterruptedDueToReplStateChange>&) {
                LOGV2(6183601,
                      "Timestamp monitor got interrupted due to repl state change, retrying");
                return;
            } catch (const ExceptionFor<ErrorCodes::InterruptedAtShutdown>& ex) {
                if (_shuttingDown) {
                    return;
                }
                _shuttingDown = true;
                LOGV2(22263, "Timestamp monitor is stopping", "error"_attr = ex);
            } catch (const ExceptionFor<ErrorCategory::CancellationError>&) {
                return;
            } catch (const DBException& ex) {
                // Logs and rethrows the exceptions of other types.
                LOGV2_ERROR(5802500, "Timestamp monitor threw an exception", "error"_attr = ex);
                throw;
            }
        },
        Seconds(1),
        true /*isKillableByStepdown*/);

    _job = _periodicRunner->makeJob(std::move(job));
    _job.start();
    _running = true;
}

void StorageEngineImpl::TimestampMonitor::addListener(TimestampListener* listener) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    if (std::find(_listeners.begin(), _listeners.end(), listener) != _listeners.end()) {
        bool listenerAlreadyRegistered = true;
        invariant(!listenerAlreadyRegistered);
    }
    _listeners.push_back(listener);
}

void StorageEngineImpl::TimestampMonitor::removeListener(TimestampListener* listener) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    if (auto it = std::find(_listeners.begin(), _listeners.end(), listener);
        it != _listeners.end()) {
        _listeners.erase(it);
    }
}

std::vector<StorageEngineImpl::TimestampMonitor::TimestampListener*>
StorageEngineImpl::TimestampMonitor::getListeners() {
    return _listeners;
}

StatusWith<Timestamp> StorageEngineImpl::pinOldestTimestamp(
    RecoveryUnit& ru,
    const std::string& requestingServiceName,
    Timestamp requestedTimestamp,
    bool roundUpIfTooOld) {
    return _engine->pinOldestTimestamp(
        ru, requestingServiceName, requestedTimestamp, roundUpIfTooOld);
}

void StorageEngineImpl::unpinOldestTimestamp(const std::string& requestingServiceName) {
    _engine->unpinOldestTimestamp(requestingServiceName);
}

void StorageEngineImpl::setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) {
    _engine->setPinnedOplogTimestamp(pinnedTimestamp);
}

Status StorageEngineImpl::oplogDiskLocRegister(OperationContext* opCtx,
                                               RecordStore* oplogRecordStore,
                                               const Timestamp& opTime,
                                               bool orderedCommit) {
    // Callers should be updating visibility as part of a write operation. We want to ensure that
    // we never get here while holding an uninterruptible, read-ticketed lock. That would indicate
    // that we are operating with the wrong global lock semantics, and either hold too weak a lock
    // (e.g. IS) or that we upgraded in a way we shouldn't (e.g. IS -> IX).
    invariant(!shard_role_details::getLocker(opCtx)->hasReadTicket() ||
              !opCtx->uninterruptibleLocksRequested_DO_NOT_USE());  // NOLINT

    return _engine->oplogDiskLocRegister(
        *shard_role_details::getRecoveryUnit(opCtx), oplogRecordStore, opTime, orderedCommit);
}

void StorageEngineImpl::waitForAllEarlierOplogWritesToBeVisible(
    OperationContext* opCtx, RecordStore* oplogRecordStore) const {
    // Callers are waiting for other operations to finish updating visibility. We want to ensure
    // that we never get here while holding an uninterruptible, write-ticketed lock. That could
    // indicate we are holding a stronger lock than we need to, and that we could actually
    // contribute to ticket-exhaustion. That could prevent the write we are waiting on from
    // acquiring the lock it needs to update the oplog visibility.
    invariant(!shard_role_details::getLocker(opCtx)->hasWriteTicket() ||
              !opCtx->uninterruptibleLocksRequested_DO_NOT_USE());  // NOLINT

    // Make sure that callers do not hold an active snapshot so it will be able to see the oplog
    // entries it waited for afterwards.
    if (shard_role_details::getRecoveryUnit(opCtx)->isActive()) {
        shard_role_details::getLocker(opCtx)->dump();
        invariant(!shard_role_details::getRecoveryUnit(opCtx)->isActive(),
                  str::stream() << "Unexpected open storage txn. RecoveryUnit state: "
                                << RecoveryUnit::toString(
                                       shard_role_details::getRecoveryUnit(opCtx)->getState())
                                << ", inMultiDocumentTransaction:"
                                << (opCtx->inMultiDocumentTransaction() ? "true" : "false"));
    }

    _engine->waitForAllEarlierOplogWritesToBeVisible(opCtx, oplogRecordStore);
}

bool StorageEngineImpl::waitUntilDurable(OperationContext* opCtx) {
    // Don't block while holding a lock unless we are the only active operation in the system.
    auto locker = shard_role_details::getLocker(opCtx);
    invariant(!locker->isLocked() || locker->isW());
    return _engine->waitUntilDurable(opCtx);
}

bool StorageEngineImpl::waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                                          bool stableCheckpoint) {
    // Don't block while holding a lock unless we are the only active operation in the system.
    auto locker = shard_role_details::getLocker(opCtx);
    invariant(!locker->isLocked() || locker->isW());
    return _engine->waitUntilUnjournaledWritesDurable(opCtx, stableCheckpoint);
}

MDBCatalog* StorageEngineImpl::getMDBCatalog() {
    return _catalog.get();
}

const MDBCatalog* StorageEngineImpl::getMDBCatalog() const {
    return _catalog.get();
}

boost::optional<bool> StorageEngineImpl::getFlagFromStorageOptions(
    const BSONObj& storageEngineOptions, StringData flagName) const {
    return _engine->getFlagFromStorageOptions(storageEngineOptions, flagName);
}

BSONObj StorageEngineImpl::setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                                   StringData flagName,
                                                   boost::optional<bool> flagValue) const {
    return _engine->setFlagToStorageOptions(storageEngineOptions, flagName, flagValue);
}

BSONObj StorageEngineImpl::getSanitizedStorageOptionsForSecondaryReplication(
    const BSONObj& options) const {
    return _engine->getSanitizedStorageOptionsForSecondaryReplication(options);
}

void StorageEngineImpl::dump() const {
    _engine->dump();
}

Status StorageEngineImpl::autoCompact(RecoveryUnit& ru, const AutoCompactOptions& options) {
    return _engine->autoCompact(ru, options);
}

bool StorageEngineImpl::underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) {
    return _engine->underCachePressure(concurrentWriteOuts, concurrentReadOuts);
};

size_t StorageEngineImpl::getCacheSizeMB() {
    return _engine->getCacheSizeMB();
}

bool StorageEngineImpl::hasOngoingLiveRestore() {
    return _engine->hasOngoingLiveRestore();
}

}  // namespace mongo
