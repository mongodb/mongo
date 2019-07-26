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

#include "mongo/db/storage/storage_engine_impl.h"

#include <algorithm>

#include "mongo/db/catalog/catalog_control.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/durable_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/temporary_kv_record_store.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/db/unclean_shutdown.h"
#include "mongo/stdx/unordered_map.h"
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
}  // namespace

StorageEngineImpl::StorageEngineImpl(KVEngine* engine, StorageEngineOptions options)
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

void StorageEngineImpl::loadCatalog(OperationContext* opCtx) {
    bool catalogExists = _engine->hasIdent(opCtx, catalogInfo);
    if (_options.forRepair && catalogExists) {
        auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
        invariant(repairObserver->isIncomplete());

        log() << "Repairing catalog metadata";
        Status status = _engine->repairIdent(opCtx, catalogInfo);

        if (status.code() == ErrorCodes::DataModifiedByRepair) {
            warning() << "Catalog data modified by repair: " << status.reason();
            repairObserver->onModification(str::stream()
                                           << "DurableCatalog repaired: " << status.reason());
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

    _catalog.reset(new DurableCatalogImpl(
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
    for (const auto& nss : collectionsKnownToCatalog) {
        std::string dbName = nss.db().toString();

        if (loadingFromUncleanShutdownOrRepair) {
            // If we are loading the catalog after an unclean shutdown or during repair, it's
            // possible that there are collections in the catalog that are unknown to the storage
            // engine. If we can't find a table in the list of storage engine idents, either
            // attempt to recover the ident or drop it.
            const auto collectionIdent = _catalog->getCollectionIdent(nss);
            bool orphan = !std::binary_search(identsKnownToStorageEngine.begin(),
                                              identsKnownToStorageEngine.end(),
                                              collectionIdent);
            // If the storage engine is missing a collection and is unable to create a new record
            // store, drop it from the catalog and skip initializing it by continuing past the
            // following logic.
            if (orphan) {
                auto status = _recoverOrphanedCollection(opCtx, nss, collectionIdent);
                if (!status.isOK()) {
                    warning() << "Failed to recover orphaned data file for collection '" << nss
                              << "': " << status;
                    WriteUnitOfWork wuow(opCtx);
                    fassert(50716, _catalog->_removeEntry(opCtx, nss));

                    if (_options.forRepair) {
                        StorageRepairObserver::get(getGlobalServiceContext())
                            ->onModification(str::stream() << "Collection " << nss
                                                           << " dropped: " << status.reason());
                    }
                    wuow.commit();
                    continue;
                }
            }
        }

        _initCollection(opCtx, nss, _options.forRepair);
        auto maxPrefixForCollection = _catalog->getMetaData(opCtx, nss).getMaxPrefix();
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

void StorageEngineImpl::_initCollection(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        bool forRepair) {
    BSONCollectionCatalogEntry::MetaData md = _catalog->getMetaData(opCtx, nss);
    uassert(ErrorCodes::MustDowngrade,
            str::stream() << "Collection does not have UUID in KVCatalog. Collection: " << nss,
            md.options.uuid);

    auto ident = _catalog->getCollectionIdent(nss);

    std::unique_ptr<RecordStore> rs;
    if (forRepair) {
        // Using a NULL rs since we don't want to open this record store before it has been
        // repaired. This also ensures that if we try to use it, it will blow up.
        rs = nullptr;
    } else {
        rs = _engine->getGroupedRecordStore(opCtx, nss.ns(), ident, md.options, md.prefix);
        invariant(rs);
    }

    auto uuid = _catalog->getCollectionOptions(opCtx, nss).uuid.get();

    auto collectionFactory = Collection::Factory::get(getGlobalServiceContext());
    auto collection = collectionFactory->make(opCtx, nss, uuid, std::move(rs));

    auto& collectionCatalog = CollectionCatalog::get(getGlobalServiceContext());
    collectionCatalog.registerCollection(uuid, std::move(collection));
}

void StorageEngineImpl::closeCatalog(OperationContext* opCtx) {
    dassert(opCtx->lockState()->isLocked());
    if (shouldLog(::mongo::logger::LogComponent::kStorageRecovery, kCatalogLogLevel)) {
        LOG_FOR_RECOVERY(kCatalogLogLevel) << "loadCatalog:";
        _dumpCatalog(opCtx);
    }
    CollectionCatalog::get(opCtx).deregisterAllCollections();

    _catalog.reset();
    _catalogRecordStore.reset();
}

Status StorageEngineImpl::_recoverOrphanedCollection(OperationContext* opCtx,
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
            ->onModification(str::stream() << "Collection " << collectionName
                                           << " recovered: " << status.reason());
    }
    wuow.commit();
    return Status::OK();
}

/**
 * This method reconciles differences between idents the KVEngine is aware of and the
 * DurableCatalog. There are three differences to consider:
 *
 * First, a KVEngine may know of an ident that the DurableCatalog does not. This method will drop
 * the ident from the KVEngine.
 *
 * Second, a DurableCatalog may have a collection ident that the KVEngine does not. This is an
 * illegal state and this method fasserts.
 *
 * Third, a DurableCatalog may have an index ident that the KVEngine does not. This method will
 * rebuild the index.
 */
StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>>
StorageEngineImpl::reconcileCatalogAndIdents(OperationContext* opCtx) {
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
                                      << " Ident: " << identForColl};
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
                if (!serverGlobalParams.indexBuildRetry) {
                    log() << "Dropping an unfinished index because --noIndexBuildRetry is set. "
                             "Collection: "
                          << coll << " Index: " << indexName;
                    fassert(51197, _engine->dropIdent(opCtx, indexIdent));
                    indexesToDrop.push_back(indexName);
                    continue;
                }

                log() << "Expected background index build did not complete, rebuilding. "
                         "Collection: "
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
                      str::stream()
                          << "Index is missing. Collection: " << coll << " Index: " << indexName);
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

std::string StorageEngineImpl::getFilesystemPathForDb(const std::string& dbName) const {
    return _catalog->getFilesystemPathForDb(dbName);
}

void StorageEngineImpl::cleanShutdown() {
    if (_timestampMonitor) {
        _timestampMonitor->removeListener(&_minOfCheckpointAndOldestTimestampListener);
    }

    CollectionCatalog::get(getGlobalServiceContext()).deregisterAllCollections();

    _catalog.reset();
    _catalogRecordStore.reset();

    _timestampMonitor.reset();

    _engine->cleanShutdown();
    // intentionally not deleting _engine
}

StorageEngineImpl::~StorageEngineImpl() {}

void StorageEngineImpl::finishInit() {
    if (_engine->supportsRecoveryTimestamp()) {
        _timestampMonitor = std::make_unique<TimestampMonitor>(
            _engine.get(), getGlobalServiceContext()->getPeriodicRunner());
        _timestampMonitor->startup();
        _timestampMonitor->addListener(&_minOfCheckpointAndOldestTimestampListener);
    }
}

RecoveryUnit* StorageEngineImpl::newRecoveryUnit() {
    if (!_engine) {
        // shutdown
        return nullptr;
    }
    return _engine->newRecoveryUnit();
}

std::vector<std::string> StorageEngineImpl::listDatabases() const {
    return CollectionCatalog::get(getGlobalServiceContext()).getAllDbNames();
}

Status StorageEngineImpl::closeDatabase(OperationContext* opCtx, StringData db) {
    // This is ok to be a no-op as there is no database layer in kv.
    return Status::OK();
}

Status StorageEngineImpl::dropDatabase(OperationContext* opCtx, StringData db) {
    {
        auto dbs = CollectionCatalog::get(opCtx).getAllDbNames();
        if (std::count(dbs.begin(), dbs.end(), db.toString()) == 0) {
            return Status(ErrorCodes::NamespaceNotFound, "db not found to drop");
        }
    }

    std::vector<NamespaceString> toDrop =
        CollectionCatalog::get(opCtx).getAllCollectionNamesFromDb(opCtx, db);

    // Do not timestamp any of the following writes. This will remove entries from the catalog as
    // well as drop any underlying tables. It's not expected for dropping tables to be reversible
    // on crash/recoverToStableTimestamp.
    return _dropCollectionsNoTimestamp(opCtx, toDrop);
}

/**
 * Returns the first `dropCollection` error that this method encounters. This method will attempt
 * to drop all collections, regardless of the error status.
 */
Status StorageEngineImpl::_dropCollectionsNoTimestamp(OperationContext* opCtx,
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
        auto uuid = CollectionCatalog::get(opCtx).lookupUUIDByNSS(nss).get();
        Status result = getCatalog()->dropCollection(opCtx, nss);

        if (!result.isOK() && firstError.isOK()) {
            firstError = result;
        }

        auto removedColl = CollectionCatalog::get(opCtx).deregisterCollection(uuid);
        opCtx->recoveryUnit()->registerChange(
            CollectionCatalog::get(opCtx).makeFinishDropCollectionChange(std::move(removedColl),
                                                                         uuid));
    }

    untimestampedDropWuow.commit();
    return firstError;
}

int StorageEngineImpl::flushAllFiles(OperationContext* opCtx, bool sync) {
    return _engine->flushAllFiles(opCtx, sync);
}

Status StorageEngineImpl::beginBackup(OperationContext* opCtx) {
    // We should not proceed if we are already in backup mode
    if (_inBackupMode)
        return Status(ErrorCodes::BadValue, "Already in Backup Mode");
    Status status = _engine->beginBackup(opCtx);
    if (status.isOK())
        _inBackupMode = true;
    return status;
}

void StorageEngineImpl::endBackup(OperationContext* opCtx) {
    // We should never reach here if we aren't already in backup mode
    invariant(_inBackupMode);
    _engine->endBackup(opCtx);
    _inBackupMode = false;
}

StatusWith<std::vector<std::string>> StorageEngineImpl::beginNonBlockingBackup(
    OperationContext* opCtx) {
    return _engine->beginNonBlockingBackup(opCtx);
}

void StorageEngineImpl::endNonBlockingBackup(OperationContext* opCtx) {
    return _engine->endNonBlockingBackup(opCtx);
}

StatusWith<std::vector<std::string>> StorageEngineImpl::extendBackupCursor(
    OperationContext* opCtx) {
    return _engine->extendBackupCursor(opCtx);
}

bool StorageEngineImpl::isDurable() const {
    return _engine->isDurable();
}

bool StorageEngineImpl::isEphemeral() const {
    return _engine->isEphemeral();
}

SnapshotManager* StorageEngineImpl::getSnapshotManager() const {
    return _engine->getSnapshotManager();
}

Status StorageEngineImpl::repairRecordStore(OperationContext* opCtx, const NamespaceString& nss) {
    auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
    invariant(repairObserver->isIncomplete());

    Status status = _engine->repairIdent(opCtx, _catalog->getCollectionIdent(nss));
    bool dataModified = status.code() == ErrorCodes::DataModifiedByRepair;
    if (!status.isOK() && !dataModified) {
        return status;
    }

    if (dataModified) {
        repairObserver->onModification(str::stream()
                                       << "Collection " << nss << ": " << status.reason());
    }

    // After repairing, re-initialize the collection with a valid RecordStore.
    auto& collectionCatalog = CollectionCatalog::get(getGlobalServiceContext());
    auto uuid = collectionCatalog.lookupUUIDByNSS(nss).get();
    collectionCatalog.deregisterCollection(uuid);
    _initCollection(opCtx, nss, false);
    return Status::OK();
}

std::unique_ptr<TemporaryRecordStore> StorageEngineImpl::makeTemporaryRecordStore(
    OperationContext* opCtx) {
    std::unique_ptr<RecordStore> rs =
        _engine->makeTemporaryRecordStore(opCtx, _catalog->newInternalIdent());
    LOG(1) << "created temporary record store: " << rs->getIdent();
    return std::make_unique<TemporaryKVRecordStore>(getEngine(), std::move(rs));
}

void StorageEngineImpl::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}

void StorageEngineImpl::setStableTimestamp(Timestamp stableTimestamp, bool force) {
    _engine->setStableTimestamp(stableTimestamp, force);
}

void StorageEngineImpl::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    _initialDataTimestamp = initialDataTimestamp;
    _engine->setInitialDataTimestamp(initialDataTimestamp);
}

void StorageEngineImpl::setOldestTimestampFromStable() {
    _engine->setOldestTimestampFromStable();
}

void StorageEngineImpl::setOldestTimestamp(Timestamp newOldestTimestamp) {
    const bool force = true;
    _engine->setOldestTimestamp(newOldestTimestamp, force);
}

void StorageEngineImpl::setOldestActiveTransactionTimestampCallback(
    StorageEngine::OldestActiveTransactionTimestampCallback callback) {
    _engine->setOldestActiveTransactionTimestampCallback(callback);
}

bool StorageEngineImpl::isCacheUnderPressure(OperationContext* opCtx) const {
    return _engine->isCacheUnderPressure(opCtx);
}

void StorageEngineImpl::setCachePressureForTest(int pressure) {
    return _engine->setCachePressureForTest(pressure);
}

bool StorageEngineImpl::supportsRecoverToStableTimestamp() const {
    return _engine->supportsRecoverToStableTimestamp();
}

bool StorageEngineImpl::supportsRecoveryTimestamp() const {
    return _engine->supportsRecoveryTimestamp();
}

StatusWith<Timestamp> StorageEngineImpl::recoverToStableTimestamp(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());

    // The "feature document" should not be rolled back. Perform a non-timestamped update to the
    // feature document to lock in the current state.
    DurableCatalogImpl::FeatureTracker::FeatureBits featureInfo;
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

boost::optional<Timestamp> StorageEngineImpl::getRecoveryTimestamp() const {
    return _engine->getRecoveryTimestamp();
}

boost::optional<Timestamp> StorageEngineImpl::getLastStableRecoveryTimestamp() const {
    return _engine->getLastStableRecoveryTimestamp();
}

bool StorageEngineImpl::supportsReadConcernSnapshot() const {
    return _engine->supportsReadConcernSnapshot();
}

bool StorageEngineImpl::supportsReadConcernMajority() const {
    return _engine->supportsReadConcernMajority();
}

bool StorageEngineImpl::supportsOplogStones() const {
    return _engine->supportsOplogStones();
}

bool StorageEngineImpl::supportsPendingDrops() const {
    return supportsReadConcernMajority();
}

void StorageEngineImpl::clearDropPendingState() {
    _dropPendingIdentReaper.clearDropPendingState();
}

void StorageEngineImpl::replicationBatchIsComplete() const {
    return _engine->replicationBatchIsComplete();
}

Timestamp StorageEngineImpl::getAllDurableTimestamp() const {
    return _engine->getAllDurableTimestamp();
}

Timestamp StorageEngineImpl::getOldestOpenReadTimestamp() const {
    return _engine->getOldestOpenReadTimestamp();
}

boost::optional<Timestamp> StorageEngineImpl::getOplogNeededForCrashRecovery() const {
    return _engine->getOplogNeededForCrashRecovery();
}

void StorageEngineImpl::_dumpCatalog(OperationContext* opCtx) {
    auto catalogRs = _catalogRecordStore.get();
    auto cursor = catalogRs->getCursor(opCtx);
    boost::optional<Record> rec = cursor->next();
    stdx::unordered_set<std::string> nsMap;
    while (rec) {
        // This should only be called by a parent that's done an appropriate `shouldLog` check. Do
        // not duplicate the log level policy.
        LOG_FOR_RECOVERY(kCatalogLogLevel)
            << "\tId: " << rec->id << " Value: " << rec->data.toBson();
        auto valueBson = rec->data.toBson();
        if (valueBson.hasField("md")) {
            std::string ns = valueBson.getField("md").Obj().getField("ns").String();
            invariant(!nsMap.count(ns), str::stream() << "Found duplicate namespace: " << ns);
            nsMap.insert(ns);
        }
        rec = cursor->next();
    }
    opCtx->recoveryUnit()->abandonSnapshot();
}

void StorageEngineImpl::addDropPendingIdent(const Timestamp& dropTimestamp,
                                            const NamespaceString& nss,
                                            StringData ident) {
    _dropPendingIdentReaper.addDropPendingIdent(dropTimestamp, nss, ident);
}

void StorageEngineImpl::_onMinOfCheckpointAndOldestTimestampChanged(const Timestamp& timestamp) {
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

StorageEngineImpl::TimestampMonitor::TimestampMonitor(KVEngine* engine, PeriodicRunner* runner)
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

StorageEngineImpl::TimestampMonitor::~TimestampMonitor() {
    log() << "Timestamp monitor shutting down";
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    invariant(_listeners.empty());
}

void StorageEngineImpl::TimestampMonitor::startup() {
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
            try {
                auto opCtx = client->getOperationContext();
                mongo::ServiceContext::UniqueOperationContext uOpCtx;
                if (!opCtx) {
                    uOpCtx = client->makeOperationContext();
                    opCtx = uOpCtx.get();
                }

                ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
                    opCtx->lockState());
                Lock::GlobalLock lock(opCtx, MODE_IS);

                // The checkpoint timestamp is not cached in mongod and needs to be fetched with a
                // call into WiredTiger, all the other timestamps are cached in mongod.
                checkpoint = _engine->getCheckpointTimestamp();
                oldest = _engine->getOldestTimestamp();
                stable = _engine->getStableTimestamp();
            } catch (const ExceptionFor<ErrorCodes::InterruptedAtShutdown>&) {
                // If we're interrupted at shutdown, it's fine to give up on future notifications
                return;
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

    _job = _periodicRunner->makeJob(std::move(job));
    _job.start();
    _running = true;
}

void StorageEngineImpl::TimestampMonitor::notifyAll(TimestampType type, Timestamp newTimestamp) {
    stdx::lock_guard<stdx::mutex> lock(_monitorMutex);
    for (auto& listener : _listeners) {
        if (listener->getType() == type) {
            listener->notify(newTimestamp);
        }
    }
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
    if (std::find(_listeners.begin(), _listeners.end(), listener) == _listeners.end()) {
        bool listenerNotRegistered = true;
        invariant(!listenerNotRegistered);
    }
    _listeners.erase(std::remove(_listeners.begin(), _listeners.end(), listener));
}

int64_t StorageEngineImpl::sizeOnDiskForDb(OperationContext* opCtx, StringData dbName) {
    int64_t size = 0;

    catalog::forEachCollectionFromDb(opCtx, dbName, MODE_IS, [&](const Collection* collection) {
        size += collection->getRecordStore()->storageSize(opCtx);

        std::vector<std::string> indexNames;
        _catalog->getAllIndexes(opCtx, collection->ns(), &indexNames);

        for (size_t i = 0; i < indexNames.size(); i++) {
            std::string ident = _catalog->getIndexIdent(opCtx, collection->ns(), indexNames[i]);
            size += _engine->getIdentSize(opCtx, ident);
        }

        return true;
    });

    return size;
}

}  // namespace mongo
