
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

#pragma once

#include <map>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry_base.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class KVCatalog;
class KVEngine;

struct KVStorageEngineOptions {
    bool directoryPerDB = false;
    bool directoryForIndexes = false;
    bool forRepair = false;
};

/*
 * The actual definition for this function is in
 * `src/mongo/db/storage/kv/kv_database_catalog_entry.cpp` This unusual forward declaration is to
 * facilitate better linker error messages.  Tests need to pass a mock construction factory, whereas
 * main implementations should pass the `default...` factory which is linked in with the main
 * `KVDatabaseCatalogEntry` code.
 */
std::unique_ptr<KVDatabaseCatalogEntryBase> defaultDatabaseCatalogEntryFactory(
    const StringData name, KVStorageEngine* const engine);

using KVDatabaseCatalogEntryFactory = decltype(defaultDatabaseCatalogEntryFactory);

class KVStorageEngine final : public StorageEngine {
public:
    /**
     * @param engine - ownership passes to me
     */
    KVStorageEngine(KVEngine* engine,
                    const KVStorageEngineOptions& options = KVStorageEngineOptions(),
                    stdx::function<KVDatabaseCatalogEntryFactory> databaseCatalogEntryFactory =
                        defaultDatabaseCatalogEntryFactory);

    virtual ~KVStorageEngine();

    virtual void finishInit();

    virtual RecoveryUnit* newRecoveryUnit();

    virtual void listDatabases(std::vector<std::string>* out) const;

    KVDatabaseCatalogEntryBase* getDatabaseCatalogEntry(OperationContext* opCtx,
                                                        StringData db) override;

    virtual bool supportsDocLocking() const {
        return _supportsDocLocking;
    }

    virtual bool supportsDBLocking() const {
        return _supportsDBLocking;
    }

    virtual bool supportsCappedCollections() const {
        return _supportsCappedCollections;
    }

    virtual Status closeDatabase(OperationContext* opCtx, StringData db);

    virtual Status dropDatabase(OperationContext* opCtx, StringData db);

    virtual int flushAllFiles(OperationContext* opCtx, bool sync);

    virtual Status beginBackup(OperationContext* opCtx);

    virtual void endBackup(OperationContext* opCtx);

    virtual StatusWith<std::vector<std::string>> beginNonBlockingBackup(OperationContext* opCtx);

    virtual void endNonBlockingBackup(OperationContext* opCtx);

    virtual bool isDurable() const;

    virtual bool isEphemeral() const;

    virtual Status repairRecordStore(OperationContext* opCtx, const std::string& ns);

    virtual std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(
        OperationContext* opCtx) override;

    virtual void cleanShutdown();

    virtual void setStableTimestamp(Timestamp stableTimestamp,
                                    boost::optional<Timestamp> maximumTruncationTimestamp) override;

    virtual void setInitialDataTimestamp(Timestamp initialDataTimestamp) override;

    virtual void setOldestTimestampFromStable() override;

    virtual void setOldestTimestamp(Timestamp newOldestTimestamp) override;

    virtual bool isCacheUnderPressure(OperationContext* opCtx) const override;

    virtual void setCachePressureForTest(int pressure) override;

    virtual bool supportsRecoverToStableTimestamp() const override;

    virtual bool supportsRecoveryTimestamp() const override;

    virtual StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) override;

    virtual boost::optional<Timestamp> getRecoveryTimestamp() const override;

    virtual boost::optional<Timestamp> getLastStableRecoveryTimestamp() const override;

    virtual Timestamp getAllCommittedTimestamp() const override;

    bool supportsReadConcernSnapshot() const final;

    bool supportsReadConcernMajority() const final;

    virtual void replicationBatchIsComplete() const override;

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

    /**
     * Drop abandoned idents. Returns a parallel list of index name, index spec pairs to rebuild.
     */
    StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>> reconcileCatalogAndIdents(
        OperationContext* opCtx) override;

    /**
     * When loading after an unclean shutdown, this performs cleanup on the KVCatalog and unsets the
     * startingAfterUncleanShutdown decoration on the global ServiceContext.
     */
    void loadCatalog(OperationContext* opCtx) final;

    void closeCatalog(OperationContext* opCtx) final;

private:
    using CollIter = std::list<std::string>::iterator;

    Status _dropCollectionsNoTimestamp(OperationContext* opCtx,
                                       KVDatabaseCatalogEntryBase* dbce,
                                       CollIter begin,
                                       CollIter end);

    Status _dropCollectionsWithTimestamp(OperationContext* opCtx,
                                         KVDatabaseCatalogEntryBase* dbce,
                                         std::list<std::string>& toDrop,
                                         CollIter begin,
                                         CollIter end);

    /**
     * When called in a repair context (_options.forRepair=true), attempts to recover a collection
     * whose entry is present in the KVCatalog, but missing from the KVEngine. Returns an error
     * Status if called outside of a repair context or the implementation of
     * KVEngine::recoverOrphanedIdent returns an error other than DataModifiedByRepair.
     *
     * Returns Status::OK if the collection was recovered in the KVEngine and a new record store was
     * created. Recovery does not make any guarantees about the integrity of the data in the
     * collection.
     */
    Status _recoverOrphanedCollection(OperationContext* opCtx,
                                      const NamespaceString& collectionName,
                                      StringData collectionIdent);

    void _dumpCatalog(OperationContext* opCtx);

    class RemoveDBChange;

    stdx::function<KVDatabaseCatalogEntryFactory> _databaseCatalogEntryFactory;

    KVStorageEngineOptions _options;

    // This must be the first member so it is destroyed last.
    std::unique_ptr<KVEngine> _engine;

    const bool _supportsDocLocking;
    const bool _supportsDBLocking;
    const bool _supportsCappedCollections;
    Timestamp _initialDataTimestamp = Timestamp::kAllowUnstableCheckpointsSentinel;

    std::unique_ptr<RecordStore> _catalogRecordStore;
    std::unique_ptr<KVCatalog> _catalog;

    typedef std::map<std::string, KVDatabaseCatalogEntryBase*> DBMap;
    DBMap _dbs;
    mutable stdx::mutex _dbsLock;

    // Flag variable that states if the storage engine is in backup mode.
    bool _inBackupMode = false;
};
}  // namespace mongo
