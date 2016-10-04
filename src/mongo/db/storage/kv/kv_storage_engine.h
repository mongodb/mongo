// kv_storage_engine.h

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

#pragma once

#include <map>
#include <string>

#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class KVCatalog;
class KVEngine;
class KVDatabaseCatalogEntry;

struct KVStorageEngineOptions {
    KVStorageEngineOptions()
        : directoryPerDB(false), directoryForIndexes(false), forRepair(false) {}

    bool directoryPerDB;
    bool directoryForIndexes;
    bool forRepair;
};

class KVStorageEngine final : public StorageEngine {
public:
    /**
     * @param engine - ownership passes to me
     */
    KVStorageEngine(KVEngine* engine,
                    const KVStorageEngineOptions& options = KVStorageEngineOptions());
    virtual ~KVStorageEngine();

    virtual void finishInit();

    virtual RecoveryUnit* newRecoveryUnit();

    virtual void listDatabases(std::vector<std::string>* out) const;

    virtual DatabaseCatalogEntry* getDatabaseCatalogEntry(OperationContext* opCtx, StringData db);

    virtual bool supportsDocLocking() const {
        return _supportsDocLocking;
    }

    virtual Status closeDatabase(OperationContext* txn, StringData db);

    virtual Status dropDatabase(OperationContext* txn, StringData db);

    virtual int flushAllFiles(bool sync);

    virtual Status beginBackup(OperationContext* txn);

    virtual void endBackup(OperationContext* txn);

    virtual bool isDurable() const;

    virtual bool isEphemeral() const;

    virtual Status repairRecordStore(OperationContext* txn, const std::string& ns);

    virtual void cleanShutdown();

    SnapshotManager* getSnapshotManager() const final;

    void setJournalListener(JournalListener* jl) final;

    // ------ kv ------

    KVEngine* getEngine() {
        return _engine.get();
    }
    const KVEngine* getEngine() const {
        return _engine.get();
    }

    KVCatalog* getCatalog() {
        return _catalog.get();
    }
    const KVCatalog* getCatalog() const {
        return _catalog.get();
    }

private:
    class RemoveDBChange;

    KVStorageEngineOptions _options;

    // This must be the first member so it is destroyed last.
    std::unique_ptr<KVEngine> _engine;

    const bool _supportsDocLocking;

    std::unique_ptr<RecordStore> _catalogRecordStore;
    std::unique_ptr<KVCatalog> _catalog;

    typedef std::map<std::string, KVDatabaseCatalogEntry*> DBMap;
    DBMap _dbs;
    mutable stdx::mutex _dbsLock;

    // Flag variable that states if the storage engine is in backup mode.
    bool _inBackupMode = false;
};
}
