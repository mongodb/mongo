// kv_storage_engine.cpp

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
    RemoveDBChange(KVStorageEngine* engine, StringData db, KVDatabaseCatalogEntry* entry)
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
    KVDatabaseCatalogEntry* const _entry;
};

KVStorageEngine::KVStorageEngine(KVEngine* engine, const KVStorageEngineOptions& options)
    : _options(options), _engine(engine), _supportsDocLocking(_engine->supportsDocLocking()) {
    uassert(28601,
            "Storage engine does not support --directoryperdb",
            !(options.directoryPerDB && !engine->supportsDirectoryPerDB()));

    OperationContextNoop opCtx(_engine->newRecoveryUnit());

    bool catalogExists = engine->hasIdent(&opCtx, catalogInfo);

    if (options.forRepair && catalogExists) {
        log() << "Repairing catalog metadata";
        // TODO should also validate all BSON in the catalog.
        engine->repairIdent(&opCtx, catalogInfo);
    }

    if (!catalogExists) {
        WriteUnitOfWork uow(&opCtx);

        Status status =
            _engine->createRecordStore(&opCtx, catalogInfo, catalogInfo, CollectionOptions());
        // BadValue is usually caused by invalid configuration string.
        // We still fassert() but without a stack trace.
        if (status.code() == ErrorCodes::BadValue) {
            fassertFailedNoTrace(28562);
        }
        fassert(28520, status);
        uow.commit();
    }

    _catalogRecordStore.reset(
        _engine->getRecordStore(&opCtx, catalogInfo, catalogInfo, CollectionOptions()));
    _catalog.reset(new KVCatalog(_catalogRecordStore.get(),
                                 _supportsDocLocking,
                                 _options.directoryPerDB,
                                 _options.directoryForIndexes));
    _catalog->init(&opCtx);

    std::vector<std::string> collections;
    _catalog->getAllCollections(&collections);

    for (size_t i = 0; i < collections.size(); i++) {
        std::string coll = collections[i];
        NamespaceString nss(coll);
        string dbName = nss.db().toString();

        // No rollback since this is only for committed dbs.
        KVDatabaseCatalogEntry*& db = _dbs[dbName];
        if (!db) {
            db = new KVDatabaseCatalogEntry(dbName, this);
        }

        db->initCollection(&opCtx, coll, options.forRepair);
    }

    opCtx.recoveryUnit()->abandonSnapshot();

    // now clean up orphaned idents
    // we don't do this in readOnly mode.
    if (storageGlobalParams.readOnly) {
        return;
    }
    {
        // get all idents
        std::set<std::string> allIdents;
        {
            std::vector<std::string> v = _engine->getAllIdents(&opCtx);
            allIdents.insert(v.begin(), v.end());
            allIdents.erase(catalogInfo);
        }

        // remove ones still in use
        {
            vector<string> idents = _catalog->getAllIdents(&opCtx);
            for (size_t i = 0; i < idents.size(); i++) {
                allIdents.erase(idents[i]);
            }
        }

        for (std::set<std::string>::const_iterator it = allIdents.begin(); it != allIdents.end();
             ++it) {
            const std::string& toRemove = *it;
            if (!_catalog->isUserDataIdent(toRemove))
                continue;
            log() << "dropping unused ident: " << toRemove;
            WriteUnitOfWork wuow(&opCtx);
            _engine->dropIdent(&opCtx, toRemove);
            wuow.commit();
        }
    }
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

DatabaseCatalogEntry* KVStorageEngine::getDatabaseCatalogEntry(OperationContext* opCtx,
                                                               StringData dbName) {
    stdx::lock_guard<stdx::mutex> lk(_dbsLock);
    KVDatabaseCatalogEntry*& db = _dbs[dbName.toString()];
    if (!db) {
        // Not registering change since db creation is implicit and never rolled back.
        db = new KVDatabaseCatalogEntry(dbName, this);
    }
    return db;
}

Status KVStorageEngine::closeDatabase(OperationContext* txn, StringData db) {
    // This is ok to be a no-op as there is no database layer in kv.
    return Status::OK();
}

Status KVStorageEngine::dropDatabase(OperationContext* txn, StringData db) {
    KVDatabaseCatalogEntry* entry;
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
    WriteUnitOfWork wuow(txn);

    std::list<std::string> toDrop;
    entry->getCollectionNamespaces(&toDrop);

    for (std::list<std::string>::iterator it = toDrop.begin(); it != toDrop.end(); ++it) {
        string coll = *it;
        entry->dropCollection(txn, coll);
    }
    toDrop.clear();
    entry->getCollectionNamespaces(&toDrop);
    invariant(toDrop.empty());

    {
        stdx::lock_guard<stdx::mutex> lk(_dbsLock);
        txn->recoveryUnit()->registerChange(new RemoveDBChange(this, db, entry));
        _dbs.erase(db.toString());
    }

    wuow.commit();
    return Status::OK();
}

int KVStorageEngine::flushAllFiles(bool sync) {
    return _engine->flushAllFiles(sync);
}

Status KVStorageEngine::beginBackup(OperationContext* txn) {
    // We should not proceed if we are already in backup mode
    if (_inBackupMode)
        return Status(ErrorCodes::BadValue, "Already in Backup Mode");
    Status status = _engine->beginBackup(txn);
    if (status.isOK())
        _inBackupMode = true;
    return status;
}

void KVStorageEngine::endBackup(OperationContext* txn) {
    // We should never reach here if we aren't already in backup mode
    invariant(_inBackupMode);
    _engine->endBackup(txn);
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

Status KVStorageEngine::repairRecordStore(OperationContext* txn, const std::string& ns) {
    Status status = _engine->repairIdent(txn, _catalog->getCollectionIdent(ns));
    if (!status.isOK())
        return status;

    _dbs[nsToDatabase(ns)]->reinitCollectionAfterRepair(txn, ns);
    return Status::OK();
}

void KVStorageEngine::setJournalListener(JournalListener* jl) {
    _engine->setJournalListener(jl);
}
}
