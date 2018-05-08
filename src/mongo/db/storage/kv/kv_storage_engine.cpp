/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/kv/kv_storage_engine.h"

#include <algorithm>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::vector;

namespace {
const std::string catalogInfo = "_mdb_catalog";
}

class KVStorageEngine::RemoveDBChange : public RecoveryUnit::Change {
public:
    RemoveDBChange(KVStorageEngine* engine, StringData db, KVDatabaseCatalogEntryBase* entry)
        : _engine(engine), _db(db.toString()), _entry(entry) {}

    virtual void commit() {
        delete _entry;
    }

    virtual void rollback() {
        stdx::lock_guard<stdx::mutex> lk(_engine->_dbsLock);
        _engine->_dbs[_db] = _entry;
    }

    KVStorageEngine* const _engine;
    const std::string _db;
    KVDatabaseCatalogEntryBase* const _entry;
};

KVStorageEngine::KVStorageEngine(
    KVEngine* engine,
    const KVStorageEngineOptions& options,
    stdx::function<KVDatabaseCatalogEntryFactory> databaseCatalogEntryFactory)
    : _databaseCatalogEntryFactory(std::move(databaseCatalogEntryFactory)),
      _options(options),
      _engine(engine),
      _supportsDocLocking(_engine->supportsDocLocking()),
      _supportsDBLocking(_engine->supportsDBLocking()) {
    uassert(28601,
            "Storage engine does not support --directoryperdb",
            !(options.directoryPerDB && !engine->supportsDirectoryPerDB()));

    OperationContextNoop opCtx(_engine->newRecoveryUnit());

    bool catalogExists = engine->hasIdent(&opCtx, catalogInfo);

    if (options.forRepair && catalogExists) {
        log() << "Repairing catalog metadata";
        // TODO should also validate all BSON in the catalog.
        engine->repairIdent(&opCtx, catalogInfo).transitional_ignore();
    }

    if (!catalogExists) {
        WriteUnitOfWork uow(&opCtx);

        Status status = _engine->createGroupedRecordStore(
            &opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);
        // BadValue is usually caused by invalid configuration string.
        // We still fassert() but without a stack trace.
        if (status.code() == ErrorCodes::BadValue) {
            fassertFailedNoTrace(28562);
        }
        fassert(28520, status);
        uow.commit();
    }

    _catalogRecordStore = _engine->getGroupedRecordStore(
        &opCtx, catalogInfo, catalogInfo, CollectionOptions(), KVPrefix::kNotPrefixed);
    _catalog.reset(new KVCatalog(
        _catalogRecordStore.get(), _options.directoryPerDB, _options.directoryForIndexes));
    _catalog->init(&opCtx);

    std::vector<std::string> collections;
    _catalog->getAllCollections(&collections);

    KVPrefix maxSeenPrefix = KVPrefix::kNotPrefixed;
    for (size_t i = 0; i < collections.size(); i++) {
        std::string coll = collections[i];
        NamespaceString nss(coll);
        string dbName = nss.db().toString();

        // No rollback since this is only for committed dbs.
        KVDatabaseCatalogEntryBase*& db = _dbs[dbName];
        if (!db) {
            db = _databaseCatalogEntryFactory(dbName, this).release();
        }

        db->initCollection(&opCtx, coll, options.forRepair);
        auto maxPrefixForCollection = _catalog->getMetaData(&opCtx, coll).getMaxPrefix();
        maxSeenPrefix = std::max(maxSeenPrefix, maxPrefixForCollection);
    }

    KVPrefix::setLargestPrefix(maxSeenPrefix);
    opCtx.recoveryUnit()->abandonSnapshot();
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

    std::set<std::string> catalogIdents;
    {
        std::vector<std::string> vec = _catalog->getAllIdents(opCtx);
        catalogIdents.insert(vec.begin(), vec.end());
    }

    // Drop all idents in the storage engine that are not known to the catalog. This can happen in
    // the case of a collection or index creation being rolled back.
    for (const auto& it : engineIdents) {
        if (catalogIdents.find(it) != catalogIdents.end()) {
            continue;
        }

        if (!_catalog->isUserDataIdent(it)) {
            continue;
        }

        const auto& toRemove = it;
        log() << "Dropping unknown ident: " << toRemove;
        WriteUnitOfWork wuow(opCtx);
        fassertStatusOK(40591, _engine->dropIdent(opCtx, toRemove));
        wuow.commit();
    }

    // Scan all collections in the catalog and make sure their ident is known to the storage
    // engine. An omission here is fatal. A missing ident could mean a collection drop was rolled
    // back. Note that startup already attempts to open tables; this should only catch errors in
    // other contexts such as `recoverToStableTimestamp`.
    std::vector<std::string> collections;
    _catalog->getAllCollections(&collections);
    for (const auto& coll : collections) {
        const auto& identForColl = _catalog->getCollectionIdent(coll);
        if (engineIdents.find(identForColl) == engineIdents.end()) {
            return {ErrorCodes::UnrecoverableRollbackError,
                    str::stream() << "Expected collection does not exist. NS: " << coll
                                  << " Ident: "
                                  << identForColl};
        }
    }

    // Scan all indexes and return those in the catalog where the storage engine does not have the
    // corresponding ident. The caller is expected to rebuild these indexes.
    std::vector<CollectionIndexNamePair> ret;
    for (const auto& coll : collections) {
        const BSONCollectionCatalogEntry::MetaData metaData = _catalog->getMetaData(opCtx, coll);
        for (const auto& indexMetaData : metaData.indexes) {
            const std::string& indexName = indexMetaData.name();
            std::string indexIdent = _catalog->getIndexIdent(opCtx, coll, indexName);
            if (engineIdents.find(indexIdent) != engineIdents.end()) {
                continue;
            }

            log() << "Expected index data is missing, rebuilding. NS: " << coll
                  << " Index: " << indexName << " Ident: " << indexIdent;

            ret.push_back(CollectionIndexNamePair(coll, indexName));
        }
    }

    return ret;
}

void KVStorageEngine::cleanShutdown() {
    for (DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it) {
        delete it->second;
    }
    _dbs.clear();

    _catalog.reset(NULL);
    _catalogRecordStore.reset(NULL);

    _engine->cleanShutdown();
    // intentionally not deleting _engine
}

KVStorageEngine::~KVStorageEngine() {}

void KVStorageEngine::finishInit() {}

RecoveryUnit* KVStorageEngine::newRecoveryUnit() {
    if (!_engine) {
        // shutdown
        return NULL;
    }
    return _engine->newRecoveryUnit();
}

void KVStorageEngine::listDatabases(std::vector<std::string>* out) const {
    stdx::lock_guard<stdx::mutex> lk(_dbsLock);
    for (DBMap::const_iterator it = _dbs.begin(); it != _dbs.end(); ++it) {
        if (it->second->isEmpty())
            continue;
        out->push_back(it->first);
    }
}

KVDatabaseCatalogEntryBase* KVStorageEngine::getDatabaseCatalogEntry(OperationContext* opCtx,
                                                                     StringData dbName) {
    stdx::lock_guard<stdx::mutex> lk(_dbsLock);
    KVDatabaseCatalogEntryBase*& db = _dbs[dbName.toString()];
    if (!db) {
        // Not registering change since db creation is implicit and never rolled back.
        db = _databaseCatalogEntryFactory(dbName, this).release();
    }
    return db;
}

Status KVStorageEngine::closeDatabase(OperationContext* opCtx, StringData db) {
    // This is ok to be a no-op as there is no database layer in kv.
    return Status::OK();
}

Status KVStorageEngine::dropDatabase(OperationContext* opCtx, StringData db) {
    KVDatabaseCatalogEntryBase* entry;
    {
        stdx::lock_guard<stdx::mutex> lk(_dbsLock);
        DBMap::const_iterator it = _dbs.find(db.toString());
        if (it == _dbs.end())
            return Status(ErrorCodes::NamespaceNotFound, "db not found to drop");
        entry = it->second;
    }

    // This is called outside of a WUOW since MMAPv1 has unfortunate behavior around dropping
    // databases. We need to create one here since we want db dropping to all-or-nothing
    // wherever possible. Eventually we want to move this up so that it can include the logOp
    // inside of the WUOW, but that would require making DB dropping happen inside the Dur
    // system for MMAPv1.
    WriteUnitOfWork wuow(opCtx);

    std::list<std::string> toDrop;
    entry->getCollectionNamespaces(&toDrop);

    for (std::list<std::string>::iterator it = toDrop.begin(); it != toDrop.end(); ++it) {
        string coll = *it;
        entry->dropCollection(opCtx, coll).transitional_ignore();
    }
    toDrop.clear();
    entry->getCollectionNamespaces(&toDrop);
    invariant(toDrop.empty());

    {
        stdx::lock_guard<stdx::mutex> lk(_dbsLock);
        opCtx->recoveryUnit()->registerChange(new RemoveDBChange(this, db, entry));
        _dbs.erase(db.toString());
    }

    wuow.commit();
    return Status::OK();
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

bool KVStorageEngine::isDurable() const {
    return _engine->isDurable();
}

bool KVStorageEngine::isEphemeral() const {
    return _engine->isEphemeral();
}

SnapshotManager* KVStorageEngine::getSnapshotManager() const {
    return _engine->getSnapshotManager();
}

Status KVStorageEngine::repairRecordStore(OperationContext* opCtx, const std::string& ns) {
    Status status = _engine->repairIdent(opCtx, _catalog->getCollectionIdent(ns));
    if (!status.isOK())
        return status;

    _dbs[nsToDatabase(ns)]->reinitCollectionAfterRepair(opCtx, ns);
    return Status::OK();
}

void KVStorageEngine::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}

void KVStorageEngine::setStableTimestamp(Timestamp stableTimestamp) {
    _engine->setStableTimestamp(stableTimestamp);
}

void KVStorageEngine::setInitialDataTimestamp(Timestamp initialDataTimestamp) {
    _engine->setInitialDataTimestamp(initialDataTimestamp);
}

void KVStorageEngine::setOldestTimestamp(Timestamp oldestTimestamp) {
    _engine->setOldestTimestamp(oldestTimestamp);
}

bool KVStorageEngine::supportsRecoverToStableTimestamp() const {
    return _engine->supportsRecoverToStableTimestamp();
}

void KVStorageEngine::replicationBatchIsComplete() const {
    return _engine->replicationBatchIsComplete();
}

Timestamp KVStorageEngine::getAllCommittedTimestamp() const {
    return _engine->getAllCommittedTimestamp();
}
}  // namespace mongo
