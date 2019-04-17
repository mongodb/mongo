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

#include "mongo/db/storage/kv/kv_storage_engine.h"

#include <algorithm>

#include "mongo/db/catalog/catalog_control.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/catalog/uuid_catalog_helper.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/temporary_kv_record_store.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/unclean_shutdown.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

using std::string;
using std::vector;

namespace {
const std::string catalogInfo = "_mdb_catalog";
const auto kCatalogLogLevel = logger::LogSeverity::Debug(2);
}

KVStorageEngine::KVStorageEngine(KVEngine* engine, KVStorageEngineOptions options)
    : _engine(engine),
      _options(std::move(options)),
      _dropPendingIdentReaper(engine),
      _minOfCheckpointAndOldestTimestampListener(
          TimestampMonitor::TimestampType::kMinOfCheckpointAndOldest,
          [this](Timestamp timestamp) { _onMinOfCheckpointAndOldestTimestampChanged(timestamp); }),
      _supportsDocLocking(_engine->supportsDocLocking()),
      _supportsDBLocking(_engine->supportsDBLocking()),
      _supportsCappedCollections(_engine->supportsCappedCollections()) {
    uassert(28601,
            "Storage engine does not support --directoryperdb",
            !(options.directoryPerDB && !engine->supportsDirectoryPerDB()));

    OperationContextNoop opCtx(_engine->newRecoveryUnit());
    loadCatalog(&opCtx);
}

void KVStorageEngine::loadCatalog(OperationContext* opCtx) {
    bool catalogExists = _engine->hasIdent(opCtx, catalogInfo);
    if (_options.forRepair && catalogExists) {
        auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
        invariant(repairObserver->isIncomplete());

        log() << "Repairing catalog metadata";
        Status status = _engine->repairIdent(opCtx, catalogInfo);

        if (status.code() == ErrorCodes::DataModifiedByRepair) {
            warning() << "Catalog data modified by repair: " << status.reason();
            repairObserver->onModification(str::stream() << "KVCatalog repaired: "
                                                         << status.reason());
        } else {
            fassertNoTrace(50926, status);
        }
    }

    if (!catalogExists) {
        WriteUnitOfWork uow(opCtx);

        auto status = _engine->createGroupedRecordStore(
            opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);

        // BadValue is usually caused by invalid configuration string.
        // We still fassert() but without a stack trace.
        if (status.code() == ErrorCodes::BadValue) {
            fassertFailedNoTrace(28562);
        }
        fassert(28520, status);
        uow.commit();
    }

    _catalogRecordStore = _engine->getGroupedRecordStore(
        opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);
    if (shouldLog(::mongo::logger::LogComponent::kStorageRecovery, kCatalogLogLevel)) {
        LOG_FOR_RECOVERY(kCatalogLogLevel) << "loadCatalog:";
        _dumpCatalog(opCtx);
    }

    _catalog.reset(new KVCatalog(
        _catalogRecordStore.get(), _options.directoryPerDB, _options.directoryForIndexes, this));
    _catalog->init(opCtx);

    // We populate 'identsKnownToStorageEngine' only if we are loading after an unclean shutdown or
    // doing repair.
    const bool loadingFromUncleanShutdownOrRepair =
        startingAfterUncleanShutdown(getGlobalServiceContext()) || _options.forRepair;

    std::vector<std::string> identsKnownToStorageEngine;
    if (loadingFromUncleanShutdownOrRepair) {
        identsKnownToStorageEngine = _engine->getAllIdents(opCtx);
        std::sort(identsKnownToStorageEngine.begin(), identsKnownToStorageEngine.end());
    }

    auto collectionsKnownToCatalog = _catalog->getAllCollections();

    if (_options.forRepair) {
        // It's possible that there are collection files on disk that are unknown to the catalog. In
        // a repair context, if we can't find an ident in the catalog, we generate a catalog entry
        // 'local.orphan.xxxxx' for it. However, in a nonrepair context, the orphaned idents
        // will be dropped in reconcileCatalogAndIdents().
        for (const auto& ident : identsKnownToStorageEngine) {
            if (_catalog->isCollectionIdent(ident)) {
                bool isOrphan = !std::any_of(collectionsKnownToCatalog.begin(),
                                             collectionsKnownToCatalog.end(),
                                             [this, &ident](const auto& coll) {
                                                 return _catalog->getCollectionIdent(
                                                            NamespaceString(coll)) == ident;
                                             });
                if (isOrphan) {
                    // If the catalog does not have information about this
                    // collection, we create an new entry for it.
                    WriteUnitOfWork wuow(opCtx);
                    StatusWith<std::string> statusWithNs = _catalog->newOrphanedIdent(opCtx, ident);
                    if (statusWithNs.isOK()) {
                        wuow.commit();
                        auto orphanCollNs = statusWithNs.getValue();
                        log() << "Successfully created an entry in the catalog for the orphaned "
                                 "collection: "
                              << orphanCollNs;
                        warning() << orphanCollNs
                                  << " does not have the _id index. Please manually "
                                     "build the index.";

                        StorageRepairObserver::get(getGlobalServiceContext())
                            ->onModification(str::stream() << "Orphan collection created: "
                                                           << statusWithNs.getValue());

                    } else {
                        // Log an error message if we cannot create the entry.
                        // reconcileCatalogAndIdents() will later drop this ident.
                        error() << "Cannot create an entry in the catalog for the orphaned "
                                   "collection ident: "
                                << ident << " due to " << statusWithNs.getStatus().reason();
                        error() << "Restarting the server will remove this ident.";
                    }
                }
            }
        }
    }

    KVPrefix maxSeenPrefix = KVPrefix::kNotPrefixed;
    for (const auto& coll : collectionsKnownToCatalog) {
        NamespaceString nss(coll);
        std::string dbName = nss.db().toString();

        if (loadingFromUncleanShutdownOrRepair) {
            // If we are loading the catalog after an unclean shutdown or during repair, it's
            // possible that there are collections in the catalog that are unknown to the storage
            // engine. If we can't find a table in the list of storage engine idents, either
            // attempt to recover the ident or drop it.
            const auto collectionIdent = _catalog->getCollectionIdent(coll);
            bool orphan = !std::binary_search(identsKnownToStorageEngine.begin(),
                                              identsKnownToStorageEngine.end(),
                                              collectionIdent);
            // If the storage engine is missing a collection and is unable to create a new record
            // store, drop it from the catalog and skip initializing it by continuing past the
            // following logic.
            if (orphan) {
                auto status = _recoverOrphanedCollection(opCtx, nss, collectionIdent);
                if (!status.isOK()) {
                    warning() << "Failed to recover orphaned data file for collection '" << coll
                              << "': " << status;
                    WriteUnitOfWork wuow(opCtx);
                    fassert(50716, _catalog->_removeEntry(opCtx, coll));

                    if (_options.forRepair) {
                        StorageRepairObserver::get(getGlobalServiceContext())
                            ->onModification(str::stream() << "Collection " << coll << " dropped: "
                                                           << status.reason());
                    }
                    wuow.commit();
                    continue;
                }
            }
        }

        _catalog->initCollection(opCtx, coll, _options.forRepair);
        auto maxPrefixForCollection = _catalog->getMetaData(opCtx, coll).getMaxPrefix();
        maxSeenPrefix = std::max(maxSeenPrefix, maxPrefixForCollection);

        if (nss.isOrphanCollection()) {
            log() << "Orphaned collection found: " << nss;
        }
    }

    KVPrefix::setLargestPrefix(maxSeenPrefix);
    opCtx->recoveryUnit()->abandonSnapshot();

    // Unset the unclean shutdown flag to avoid executing special behavior if this method is called
    // after startup.
    startingAfterUncleanShutdown(getGlobalServiceContext()) = false;
}

void KVStorageEngine::closeCatalog(OperationContext* opCtx) {
    dassert(opCtx->lockState()->isLocked());
    if (shouldLog(::mongo::logger::LogComponent::kStorageRecovery, kCatalogLogLevel)) {
        LOG_FOR_RECOVERY(kCatalogLogLevel) << "loadCatalog:";
        _dumpCatalog(opCtx);
    }
    UUIDCatalog::get(opCtx).deregisterAllCatalogEntriesAndCollectionObjects();

    _catalog.reset();
    _catalogRecordStore.reset();
}

Status KVStorageEngine::_recoverOrphanedCollection(OperationContext* opCtx,
                                                   const NamespaceString& collectionName,
                                                   StringData collectionIdent) {
    if (!_options.forRepair) {
        return {ErrorCodes::IllegalOperation, "Orphan recovery only supported in repair"};
    }
    log() << "Storage engine is missing collection '" << collectionName
          << "' from its metadata. Attempting to locate and recover the data for "
          << collectionIdent;

    WriteUnitOfWork wuow(opCtx);
    const auto metadata = _catalog->getMetaData(opCtx, collectionName);
    auto status =
        _engine->recoverOrphanedIdent(opCtx, collectionName, collectionIdent, metadata.options);


    bool dataModified = status.code() == ErrorCodes::DataModifiedByRepair;
    if (!status.isOK() && !dataModified) {
        return status;
    }
    if (dataModified) {
        StorageRepairObserver::get(getGlobalServiceContext())
            ->onModification(str::stream() << "Collection " << collectionName << " recovered: "
                                           << status.reason());
    }
    wuow.commit();
    return Status::OK();
}

/**
 * This method reconciles differences between idents the KVEngine is aware of and the
 * KVCatalog. There are three differences to consider:
 *
 * First, a KVEngine may know of an ident that the KVCatalog does not. This method will drop
 * the ident from the KVEngine.
 *
 * Second, a KVCatalog may have a collection ident that the KVEngine does not. This is an
 * illegal state and this method fasserts.
 *
 * Third, a KVCatalog may have an index ident that the KVEngine does not. This method will
 * rebuild the index.
 */
StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>>
KVStorageEngine::reconcileCatalogAndIdents(OperationContext* opCtx) {
    // Gather all tables known to the storage engine and drop those that aren't cross-referenced
    // in the _mdb_catalog. This can happen for two reasons.
    //
    // First, collection creation and deletion happen in two steps. First the storage engine
    // creates/deletes the table, followed by the change to the _mdb_catalog. It's not assumed a
    // storage engine can make these steps atomic.
    //
    // Second, a replica set node in 3.6+ on supported storage engines will only persist "stable"
    // data to disk. That is data which replication guarantees won't be rolled back. The
    // _mdb_catalog will reflect the "stable" set of collections/indexes. However, it's not
    // expected for a storage engine's ability to persist stable data to extend to "stable
    // tables".
    std::set<std::string> engineIdents;
    {
        std::vector<std::string> vec = _engine->getAllIdents(opCtx);
        engineIdents.insert(vec.begin(), vec.end());
        engineIdents.erase(catalogInfo);
    }

    LOG_FOR_RECOVERY(2) << "Reconciling collection and index idents.";
    std::set<std::string> catalogIdents;
    {
        std::vector<std::string> vec = _catalog->getAllIdents(opCtx);
        catalogIdents.insert(vec.begin(), vec.end());
    }
    std::set<std::string> internalIdentsToDrop;

    auto dropPendingIdents = _dropPendingIdentReaper.getAllIdents();

    // Drop all idents in the storage engine that are not known to the catalog. This can happen in
    // the case of a collection or index creation being rolled back.
    for (const auto& it : engineIdents) {
        if (catalogIdents.find(it) != catalogIdents.end()) {
            continue;
        }

        // Internal idents are dropped at the end after those left over from index builds are
        // identified.
        if (_catalog->isInternalIdent(it)) {
            internalIdentsToDrop.insert(it);
            continue;
        }

        if (!_catalog->isUserDataIdent(it)) {
            continue;
        }

        // In repair context, any orphaned collection idents from the engine should already be
        // recovered in the catalog in loadCatalog().
        invariant(!(_catalog->isCollectionIdent(it) && _options.forRepair));

        // Leave drop-pending idents alone.
        // These idents have to be retained as long as the corresponding drops are not part of a
        // checkpoint.
        if (dropPendingIdents.find(it) != dropPendingIdents.cend()) {
            log() << "Not removing ident for uncheckpointed collection or index drop: " << it;
            continue;
        }

        const auto& toRemove = it;
        log() << "Dropping unknown ident: " << toRemove;
        WriteUnitOfWork wuow(opCtx);
        fassert(40591, _engine->dropIdent(opCtx, toRemove));
        wuow.commit();
    }

    // Scan all collections in the catalog and make sure their ident is known to the storage
    // engine. An omission here is fatal. A missing ident could mean a collection drop was rolled
    // back. Note that startup already attempts to open tables; this should only catch errors in
    // other contexts such as `recoverToStableTimestamp`.
    auto collections = _catalog->getAllCollections();
    if (!_options.forRepair) {
        for (const auto& coll : collections) {
            const auto& identForColl = _catalog->getCollectionIdent(coll);
            if (engineIdents.find(identForColl) == engineIdents.end()) {
                return {ErrorCodes::UnrecoverableRollbackError,
                        str::stream() << "Expected collection does not exist. Collection: " << coll
                                      << " Ident: "
                                      << identForColl};
            }
        }
    }

    // Scan all indexes and return those in the catalog where the storage engine does not have the
    // corresponding ident. The caller is expected to rebuild these indexes.
    //
    // Also, remove unfinished builds except those that were background index builds started on a
    // secondary.
    std::vector<CollectionIndexNamePair> ret;
    for (const auto& coll : collections) {
        BSONCollectionCatalogEntry::MetaData metaData = _catalog->getMetaData(opCtx, coll);

        // Batch up the indexes to remove them from `metaData` outside of the iterator.
        std::vector<std::string> indexesToDrop;
        for (const auto& indexMetaData : metaData.indexes) {
            const std::string& indexName = indexMetaData.name();
            std::string indexIdent = _catalog->getIndexIdent(opCtx, coll, indexName);

            const bool foundIdent = engineIdents.find(indexIdent) != engineIdents.end();
            // An index drop will immediately remove the ident, but the `indexMetaData` catalog
            // entry still exists implying the drop hasn't necessarily been replicated to a
            // majority of nodes. The code will rebuild the index, despite potentially
            // encountering another `dropIndex` command.
            if (indexMetaData.ready && !foundIdent) {
                log() << "Expected index data is missing, rebuilding. Collection: " << coll
                      << " Index: " << indexName;
                ret.emplace_back(coll.ns(), indexName);
                continue;
            }

            // If this index was draining, do not delete any internal idents that it may have owned.
            // Instead, the idents can be used later on to resume draining instead of a
            // performing a full rebuild. This is only done for background secondary builds, because
            // the index must be rebuilt, and it is dropped otherwise.
            // TODO: SERVER-37952 Do not drop these idents for background index builds on
            // primaries once index builds are resumable from draining.
            if (!indexMetaData.ready && indexMetaData.isBackgroundSecondaryBuild &&
                indexMetaData.buildPhase ==
                    BSONCollectionCatalogEntry::kIndexBuildDraining.toString()) {

                if (indexMetaData.constraintViolationsIdent) {
                    auto it = internalIdentsToDrop.find(*indexMetaData.constraintViolationsIdent);
                    if (it != internalIdentsToDrop.end()) {
                        internalIdentsToDrop.erase(it);
                    }
                }

                if (indexMetaData.sideWritesIdent) {
                    auto it = internalIdentsToDrop.find(*indexMetaData.sideWritesIdent);
                    if (it != internalIdentsToDrop.end()) {
                        internalIdentsToDrop.erase(it);
                    }
                }
            }

            // If the index was kicked off as a background secondary index build, replication
            // recovery will not run into the oplog entry to recreate the index. If the index
            // table is not found, or the index build did not successfully complete, this code
            // will return the index to be rebuilt.
            if (indexMetaData.isBackgroundSecondaryBuild && (!foundIdent || !indexMetaData.ready)) {
                log()
                    << "Expected background index build did not complete, rebuilding. Collection: "
                    << coll << " Index: " << indexName;
                ret.emplace_back(coll.ns(), indexName);
                continue;
            }

            // The last anomaly is when the index build did not complete, nor was the index build
            // a secondary background index build. This implies the index build was on a primary
            // and the `createIndexes` command never successfully returned, or the index build was
            // a foreground secondary index build, meaning replication recovery will build the
            // index when it replays the oplog. In these cases the index entry in the catalog
            // should be dropped.
            if (!indexMetaData.ready && !indexMetaData.isBackgroundSecondaryBuild) {
                log() << "Dropping unfinished index. Collection: " << coll
                      << " Index: " << indexName;
                // Ensure the `ident` is dropped while we have the `indexIdent` value.
                fassert(50713, _engine->dropIdent(opCtx, indexIdent));
                indexesToDrop.push_back(indexName);
                continue;
            }
        }

        for (auto&& indexName : indexesToDrop) {
            invariant(metaData.eraseIndex(indexName),
                      str::stream() << "Index is missing. Collection: " << coll << " Index: "
                                    << indexName);
        }
        if (indexesToDrop.size() > 0) {
            WriteUnitOfWork wuow(opCtx);
            _catalog->putMetaData(opCtx, coll, metaData);
            wuow.commit();
        }
    }

    for (auto&& temp : internalIdentsToDrop) {
        log() << "Dropping internal ident: " << temp;
        WriteUnitOfWork wuow(opCtx);
        fassert(51067, _engine->dropIdent(opCtx, temp));
        wuow.commit();
    }

    return ret;
}

std::string KVStorageEngine::getFilesystemPathForDb(const std::string& dbName) const {
    return _catalog->getFilesystemPathForDb(dbName);
}

void KVStorageEngine::cleanShutdown() {
    if (_timestampMonitor) {
        _timestampMonitor->removeListener(&_minOfCheckpointAndOldestTimestampListener);
    }

    UUIDCatalog::get(getGlobalServiceContext()).deregisterAllCatalogEntriesAndCollectionObjects();

    _catalog.reset();
    _catalogRecordStore.reset();

    _timestampMonitor.reset();

    _engine->cleanShutdown();
    // intentionally not deleting _engine
}

KVStorageEngine::~KVStorageEngine() {}

void KVStorageEngine::finishInit() {
    if (_engine->supportsRecoveryTimestamp()) {
        _timestampMonitor = std::make_unique<TimestampMonitor>(
            _engine.get(), getGlobalServiceContext()->getPeriodicRunner());
        _timestampMonitor->startup();
        _timestampMonitor->addListener(&_minOfCheckpointAndOldestTimestampListener);
    }
}

RecoveryUnit* KVStorageEngine::newRecoveryUnit() {
    if (!_engine) {
        // shutdown
        return nullptr;
    }
    return _engine->newRecoveryUnit();
}

std::vector<std::string> KVStorageEngine::listDatabases() const {
    return UUIDCatalog::get(getGlobalServiceContext()).getAllDbNames();
}

Status KVStorageEngine::closeDatabase(OperationContext* opCtx, StringData db) {
    // This is ok to be a no-op as there is no database layer in kv.
    return Status::OK();
}

Status KVStorageEngine::dropDatabase(OperationContext* opCtx, StringData db) {
    {
        auto dbs = UUIDCatalog::get(opCtx).getAllDbNames();
        if (std::count(dbs.begin(), dbs.end(), db.toString()) == 0) {
            return Status(ErrorCodes::NamespaceNotFound, "db not found to drop");
        }
    }

    std::vector<NamespaceString> toDrop =
        UUIDCatalog::get(opCtx).getAllCollectionNamesFromDb(opCtx, db);

    // Do not timestamp any of the following writes. This will remove entries from the catalog as
    // well as drop any underlying tables. It's not expected for dropping tables to be reversible
    // on crash/recoverToStableTimestamp.
    return _dropCollectionsNoTimestamp(opCtx, toDrop);
}

/**
 * Returns the first `dropCollection` error that this method encounters. This method will attempt
 * to drop all collections, regardless of the error status.
 */
Status KVStorageEngine::_dropCollectionsNoTimestamp(OperationContext* opCtx,
                                                    std::vector<NamespaceString>& toDrop) {
    // On primaries, this method will be called outside of any `TimestampBlock` state meaning the
    // "commit timestamp" will not be set. For this case, this method needs no special logic to
    // avoid timestamping the upcoming writes.
    //
    // On secondaries, there will be a wrapping `TimestampBlock` and the "commit timestamp" will
    // be set. Carefully save that to the side so the following writes can go through without that
    // context.
    const Timestamp commitTs = opCtx->recoveryUnit()->getCommitTimestamp();
    if (!commitTs.isNull()) {
        opCtx->recoveryUnit()->clearCommitTimestamp();
    }

    // Ensure the method exits with the same "commit timestamp" state that it was called with.
    auto addCommitTimestamp = makeGuard([&opCtx, commitTs] {
        if (!commitTs.isNull()) {
            opCtx->recoveryUnit()->setCommitTimestamp(commitTs);
        }
    });

    Status firstError = Status::OK();
    WriteUnitOfWork untimestampedDropWuow(opCtx);
    for (auto& nss : toDrop) {
        invariant(getCatalog());
        Status result = getCatalog()->dropCollection(opCtx, nss);
        if (!result.isOK() && firstError.isOK()) {
            firstError = result;
        }
    }

    untimestampedDropWuow.commit();
    return firstError;
}

int KVStorageEngine::flushAllFiles(OperationContext* opCtx, bool sync) {
    return _engine->flushAllFiles(opCtx, sync);
}

Status KVStorageEngine::beginBackup(OperationContext* opCtx) {
    // We should not proceed if we are already in backup mode
    if (_inBackupMode)
        return Status(ErrorCodes::BadValue, "Already in Backup Mode");
    Status status = _engine->beginBackup(opCtx);
    if (status.isOK())
        _inBackupMode = true;
    return status;
}

void KVStorageEngine::endBackup(OperationContext* opCtx) {
    // We should never reach here if we aren't already in backup mode
    invariant(_inBackupMode);
    _engine->endBackup(opCtx);
    _inBackupMode = false;
}

StatusWith<std::vector<std::string>> KVStorageEngine::beginNonBlockingBackup(
    OperationContext* opCtx) {
    return _engine->beginNonBlockingBackup(opCtx);
}

void KVStorageEngine::endNonBlockingBackup(OperationContext* opCtx) {
    return _engine->endNonBlockingBackup(opCtx);
}

StatusWith<std::vector<std::string>> KVStorageEngine::extendBackupCursor(OperationContext* opCtx) {
    return _engine->extendBackupCursor(opCtx);
}

bool KVStorageEngine::isDurable() const {
    return _engine->isDurable();
}

bool KVStorageEngine::isEphemeral() const {
    return _engine->isEphemeral();
}

SnapshotManager* KVStorageEngine::getSnapshotManager() const {
    return _engine->getSnapshotManager();
}

Status KVStorageEngine::repairRecordStore(OperationContext* opCtx, const NamespaceString& nss) {
    auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
    invariant(repairObserver->isIncomplete());

    Status status = _engine->repairIdent(opCtx, _catalog->getCollectionIdent(nss));
    bool dataModified = status.code() == ErrorCodes::DataModifiedByRepair;
    if (!status.isOK() && !dataModified) {
        return status;
    }

    if (dataModified) {
        repairObserver->onModification(str::stream() << "Collection " << nss << ": "
                                                     << status.reason());
    }
    _catalog->reinitCollectionAfterRepair(opCtx, nss);

    return Status::OK();
}

std::unique_ptr<TemporaryRecordStore> KVStorageEngine::makeTemporaryRecordStore(
    OperationContext* opCtx) {
    std::unique_ptr<RecordStore> rs =
        _engine->makeTemporaryRecordStore(opCtx, _catalog->newInternalIdent());
    LOG(1) << "created temporary record store: " << rs->getIdent();
    return std::make_unique<TemporaryKVRecordStore>(getEngine(), std::move(rs));
}

void KVStorageEngine::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}

void KVStorageEngine::setStableTimestamp(Timestamp stableTimestamp, bool force) {
    _engine->setStableTimestamp(stableTimestamp, force);
}

void KVStorageEngine::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    _initialDataTimestamp = initialDataTimestamp;
    _engine->setInitialDataTimestamp(initialDataTimestamp);
}

void KVStorageEngine::setOldestTimestampFromStable() {
    _engine->setOldestTimestampFromStable();
}

void KVStorageEngine::setOldestTimestamp(Timestamp newOldestTimestamp) {
    const bool force = true;
    _engine->setOldestTimestamp(newOldestTimestamp, force);
}

void KVStorageEngine::setOldestActiveTransactionTimestampCallback(
    StorageEngine::OldestActiveTransactionTimestampCallback callback) {
    _engine->setOldestActiveTransactionTimestampCallback(callback);
}

bool KVStorageEngine::isCacheUnderPressure(OperationContext* opCtx) const {
    return _engine->isCacheUnderPressure(opCtx);
}

void KVStorageEngine::setCachePressureForTest(int pressure) {
    return _engine->setCachePressureForTest(pressure);
}

bool KVStorageEngine::supportsRecoverToStableTimestamp() const {
    return _engine->supportsRecoverToStableTimestamp();
}

bool KVStorageEngine::supportsRecoveryTimestamp() const {
    return _engine->supportsRecoveryTimestamp();
}

StatusWith<Timestamp> KVStorageEngine::recoverToStableTimestamp(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());

    // The "feature document" should not be rolled back. Perform a non-timestamped update to the
    // feature document to lock in the current state.
    KVCatalog::FeatureTracker::FeatureBits featureInfo;
    {
        WriteUnitOfWork wuow(opCtx);
        featureInfo = _catalog->getFeatureTracker()->getInfo(opCtx);
        _catalog->getFeatureTracker()->putInfo(opCtx, featureInfo);
        wuow.commit();
    }

    auto state = catalog::closeCatalog(opCtx);

    StatusWith<Timestamp> swTimestamp = _engine->recoverToStableTimestamp(opCtx);
    if (!swTimestamp.isOK()) {
        return swTimestamp;
    }

    catalog::openCatalog(opCtx, state);

    log() << "recoverToStableTimestamp successful. Stable Timestamp: " << swTimestamp.getValue();
    return {swTimestamp.getValue()};
}

boost::optional<Timestamp> KVStorageEngine::getRecoveryTimestamp() const {
    return _engine->getRecoveryTimestamp();
}

boost::optional<Timestamp> KVStorageEngine::getLastStableRecoveryTimestamp() const {
    return _engine->getLastStableRecoveryTimestamp();
}

bool KVStorageEngine::supportsReadConcernSnapshot() const {
    return _engine->supportsReadConcernSnapshot();
}

bool KVStorageEngine::supportsReadConcernMajority() const {
    return _engine->supportsReadConcernMajority();
}

bool KVStorageEngine::supportsPendingDrops() const {
    return supportsReadConcernMajority();
}

void KVStorageEngine::clearDropPendingState() {
    _dropPendingIdentReaper.clearDropPendingState();
}

void KVStorageEngine::replicationBatchIsComplete() const {
    return _engine->replicationBatchIsComplete();
}

Timestamp KVStorageEngine::getAllCommittedTimestamp() const {
    return _engine->getAllCommittedTimestamp();
}

Timestamp KVStorageEngine::getOldestOpenReadTimestamp() const {
    return _engine->getOldestOpenReadTimestamp();
}

void KVStorageEngine::_dumpCatalog(OperationContext* opCtx) {
    auto catalogRs = _catalogRecordStore.get();
    auto cursor = catalogRs->getCursor(opCtx);
    boost::optional<Record> rec = cursor->next();
    while (rec) {
        // This should only be called by a parent that's done an appropriate `shouldLog` check. Do
        // not duplicate the log level policy.
        LOG_FOR_RECOVERY(kCatalogLogLevel) << "\tId: " << rec->id
                                           << " Value: " << rec->data.toBson();
        rec = cursor->next();
    }
    opCtx->recoveryUnit()->abandonSnapshot();
}

void KVStorageEngine::addDropPendingIdent(const Timestamp& dropTimestamp,
                                          const NamespaceString& nss,
                                          StringData ident) {
    _dropPendingIdentReaper.addDropPendingIdent(dropTimestamp, nss, ident);
}

void KVStorageEngine::_onMinOfCheckpointAndOldestTimestampChanged(const Timestamp& timestamp) {
    if (timestamp.isNull()) {
        return;
    }

    // No drop-pending idents present if getEarliestDropTimestamp() returns boost::none.
    if (auto earliestDropTimestamp = _dropPendingIdentReaper.getEarliestDropTimestamp()) {
        if (timestamp > *earliestDropTimestamp) {
            log() << "Removing drop-pending idents with drop timestamps before timestamp "
                  << timestamp;
            auto opCtx = cc().getOperationContext();
            mongo::ServiceContext::UniqueOperationContext uOpCtx;
            if (!opCtx) {
                uOpCtx = cc().makeOperationContext();
                opCtx = uOpCtx.get();
            }
            _dropPendingIdentReaper.dropIdentsOlderThan(opCtx, timestamp);
        }
    }
}

KVStorageEngine::TimestampMonitor::TimestampMonitor(KVEngine* engine, PeriodicRunner* runner)
    : _engine(engine), _running(false), _periodicRunner(runner) {
    _currentTimestamps.checkpoint = _engine->getCheckpointTimestamp();
    _currentTimestamps.oldest = _engine->getOldestTimestamp();
    _currentTimestamps.stable = _engine->getStableTimestamp();
    _currentTimestamps.minOfCheckpointAndOldest =
        (_currentTimestamps.checkpoint.isNull() ||
         (_currentTimestamps.checkpoint > _currentTimestamps.oldest))
        ? _currentTimestamps.oldest
        : _currentTimestamps.checkpoint;
}

KVStorageEngine::TimestampMonitor::~TimestampMonitor() {
    log() << "Timestamp monitor shutting down";
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    invariant(_listeners.empty());
}

void KVStorageEngine::TimestampMonitor::startup() {
    invariant(!_running);

    log() << "Timestamp monitor starting";
    PeriodicRunner::PeriodicJob job(
        "TimestampMonitor",
        [&](Client* client) {
            {
                stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
                if (_listeners.empty()) {
                    return;
                }
            }

            Timestamp checkpoint = _currentTimestamps.checkpoint;
            Timestamp oldest = _currentTimestamps.oldest;
            Timestamp stable = _currentTimestamps.stable;

            // Take a global lock in MODE_IS while fetching timestamps to guarantee that
            // rollback-to-stable isn't running concurrently.
            {
                auto opCtx = client->getOperationContext();
                mongo::ServiceContext::UniqueOperationContext uOpCtx;
                if (!opCtx) {
                    uOpCtx = client->makeOperationContext();
                    opCtx = uOpCtx.get();
                }
                Lock::GlobalLock lock(opCtx, MODE_IS);

                // The checkpoint timestamp is not cached in mongod and needs to be fetched with a
                // call into WiredTiger, all the other timestamps are cached in mongod.
                checkpoint = _engine->getCheckpointTimestamp();
                oldest = _engine->getOldestTimestamp();
                stable = _engine->getStableTimestamp();
            }

            Timestamp minOfCheckpointAndOldest =
                (checkpoint.isNull() || (checkpoint > oldest)) ? oldest : checkpoint;

            // Notify listeners if the timestamps changed.
            if (_currentTimestamps.checkpoint != checkpoint) {
                _currentTimestamps.checkpoint = checkpoint;
                notifyAll(TimestampType::kCheckpoint, checkpoint);
            }

            if (_currentTimestamps.oldest != oldest) {
                _currentTimestamps.oldest = oldest;
                notifyAll(TimestampType::kOldest, oldest);
            }

            if (_currentTimestamps.stable != stable) {
                _currentTimestamps.stable = stable;
                notifyAll(TimestampType::kStable, stable);
            }

            if (_currentTimestamps.minOfCheckpointAndOldest != minOfCheckpointAndOldest) {
                _currentTimestamps.minOfCheckpointAndOldest = minOfCheckpointAndOldest;
                notifyAll(TimestampType::kMinOfCheckpointAndOldest, minOfCheckpointAndOldest);
            }
        },
        Seconds(1));

    _periodicRunner->scheduleJob(std::move(job));
    _running = true;
}

void KVStorageEngine::TimestampMonitor::notifyAll(TimestampType type, Timestamp newTimestamp) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    for (auto& listener : _listeners) {
        if (listener->getType() == type) {
            listener->notify(newTimestamp);
        }
    }
}

void KVStorageEngine::TimestampMonitor::addListener(TimestampListener* listener) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    if (std::find(_listeners.begin(), _listeners.end(), listener) != _listeners.end()) {
        bool listenerAlreadyRegistered = true;
        invariant(!listenerAlreadyRegistered);
    }
    _listeners.push_back(listener);
}

void KVStorageEngine::TimestampMonitor::removeListener(TimestampListener* listener) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    if (std::find(_listeners.begin(), _listeners.end(), listener) == _listeners.end()) {
        bool listenerNotRegistered = true;
        invariant(!listenerNotRegistered);
    }
    _listeners.erase(std::remove(_listeners.begin(), _listeners.end(), listener));
}

int64_t KVStorageEngine::sizeOnDiskForDb(OperationContext* opCtx, StringData dbName) {
    int64_t size = 0;

    catalog::forEachCollectionFromDb(
        opCtx, dbName, MODE_IS, [&](Collection* collection, CollectionCatalogEntry* catalogEntry) {
            size += catalogEntry->getRecordStore()->storageSize(opCtx);

            std::vector<std::string> indexNames;
            catalogEntry->getAllIndexes(opCtx, &indexNames);

            for (size_t i = 0; i < indexNames.size(); i++) {
                std::string ident =
                    _catalog->getIndexIdent(opCtx, catalogEntry->ns(), indexNames[i]);
                size += _engine->getIdentSize(opCtx, ident);
            }

            return true;
        });

    return size;
}

}  // namespace mongo
