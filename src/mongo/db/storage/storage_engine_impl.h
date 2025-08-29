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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/db/tenant_id.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class MDBCatalog;
class KVEngine;

struct StorageEngineOptions {
    bool directoryPerDB = false;
    bool directoryForIndexes = false;
    bool forRepair = false;
    bool forRestore = false;
    bool lockFileCreatedByUncleanShutdown = false;
};

class StorageEngineImpl final : public StorageEngine {
public:
    StorageEngineImpl(OperationContext* opCtx,
                      std::unique_ptr<KVEngine> engine,
                      std::unique_ptr<KVEngine> spillEngine,
                      StorageEngineOptions options = StorageEngineOptions());

    ~StorageEngineImpl() override;

    void notifyStorageStartupRecoveryComplete() override;

    void notifyReplStartupRecoveryComplete(RecoveryUnit&) override;

    void setInStandaloneMode() override;

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override;

    bool supportsCappedCollections() const override {
        return _supportsCappedCollections;
    }

    void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) override;

    Status beginBackup() override;

    void endBackup() override;

    Timestamp getBackupCheckpointTimestamp() override;

    Status disableIncrementalBackup() override;

    StatusWith<std::unique_ptr<StreamingCursor>> beginNonBlockingBackup(
        const BackupOptions& options) override;

    void endNonBlockingBackup() override;

    StatusWith<std::deque<std::string>> extendBackupCursor() override;

    bool supportsCheckpoints() const override;

    bool isEphemeral() const override;

    Status repairRecordStore(OperationContext* opCtx,
                             RecordId catalogId,
                             const NamespaceString& nss) override;

    std::unique_ptr<SpillTable> makeSpillTable(OperationContext* opCtx,
                                               KeyFormat keyFormat,
                                               int64_t thresholdBytes) override;

    void dropSpillTable(RecoveryUnit& ru, StringData ident) override;

    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                   StringData ident,
                                                                   KeyFormat keyFormat) override;

    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreForResumableIndexBuild(
        OperationContext* opCtx, KeyFormat keyFormat) override;

    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreFromExistingIdent(
        OperationContext* opCtx, StringData ident, KeyFormat keyFormat) override;

    void cleanShutdown(ServiceContext* svcCtx, bool memLeakAllowed) override;

    void setLastMaterializedLsn(uint64_t lsn) override;

    void setRecoveryCheckpointMetadata(StringData checkpointMetadata) override;

    void promoteToLeader() override;

    void setStableTimestamp(Timestamp stableTimestamp, bool force = false) override;

    Timestamp getStableTimestamp() const override;

    void setInitialDataTimestamp(Timestamp initialDataTimestamp) override;

    Timestamp getInitialDataTimestamp() const override;

    void setOldestTimestampFromStable() override;

    void setOldestTimestamp(Timestamp newOldestTimestamp, bool force) override;

    Timestamp getOldestTimestamp() const override;

    void setOldestActiveTransactionTimestampCallback(
        StorageEngine::OldestActiveTransactionTimestampCallback) override;

    bool supportsRecoverToStableTimestamp() const override;

    bool supportsRecoveryTimestamp() const override;

    StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) override;

    boost::optional<Timestamp> getRecoveryTimestamp() const override;

    boost::optional<Timestamp> getLastStableRecoveryTimestamp() const override;

    Timestamp getAllDurableTimestamp() const override;

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const final;

    Timestamp getPinnedOplog() const final;

    bool supportsReadConcernSnapshot() const final;

    void clearDropPendingState(OperationContext* opCtx) final;

    Status immediatelyCompletePendingDrop(OperationContext* opCtx, StringData ident) final;

    SnapshotManager* getSnapshotManager() const final;

    void setJournalListener(JournalListener* jl) final;

    KVEngine* getEngine() override {
        return _engine.get();
    }

    const KVEngine* getEngine() const override {
        return _engine.get();
    }

    KVEngine* getSpillEngine() override {
        return _spillEngine.get();
    }

    const KVEngine* getSpillEngine() const override {
        return _spillEngine.get();
    }

    void addDropPendingIdent(
        const std::variant<Timestamp, StorageEngine::CheckpointIteration>& dropTime,
        std::shared_ptr<Ident> ident,
        DropIdentCallback&& onDrop) override;

    std::set<std::string> getDropPendingIdents() override {
        return _dropPendingIdentReaper.getAllIdentNames();
    };

    std::shared_ptr<Ident> markIdentInUse(StringData ident) override;

    void startTimestampMonitor(
        std::initializer_list<TimestampMonitor::TimestampListener*> listeners) override;

    void checkpoint() override;

    StorageEngine::CheckpointIteration getCheckpointIteration() const override;

    bool hasDataBeenCheckpointed(
        StorageEngine::CheckpointIteration checkpointIteration) const override;

    std::string getFilesystemPathForDb(const DatabaseName& dbName) const override;

    MDBCatalog* getMDBCatalog() override;

    const MDBCatalog* getMDBCatalog() const override;

    /**
     * When loading after an unclean shutdown, this performs cleanup on the MDBCatalog.
     */
    void loadMDBCatalog(OperationContext* opCtx, LastShutdownState lastShutdownState) final;

    void closeMDBCatalog(OperationContext* opCtx) final;

    TimestampMonitor* getTimestampMonitor() const {
        return _timestampMonitor.get();
    }

    void stopTimestampMonitor() override;

    void restartTimestampMonitor() override;

    std::set<std::string> getDropPendingIdents() const override {
        return _dropPendingIdentReaper.getAllIdentNames();
    }

    size_t getNumDropPendingIdents() const override {
        return _dropPendingIdentReaper.getNumIdents();
    }

    std::string generateNewCollectionIdent(const DatabaseName& dbName) const override;
    std::string generateNewIndexIdent(const DatabaseName& dbName) const override;

    bool storesFilesInDbPath() const override {
        return !_options.directoryForIndexes && !_options.directoryPerDB;
    }

    int64_t getIdentSize(RecoveryUnit& ru, StringData ident) const final {
        return _engine->getIdentSize(ru, ident);
    }

    StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                             const std::string& requestingServiceName,
                                             Timestamp requestedTimestamp,
                                             bool roundUpIfTooOld) override;

    void unpinOldestTimestamp(const std::string& requestingServiceName) override;

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) override;

    Status oplogDiskLocRegister(OperationContext* opCtx,
                                RecordStore* oplogRecordStore,
                                const Timestamp& opTime,
                                bool orderedCommit) override;

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 RecordStore* oplogRecordStore) const override;

    bool waitUntilDurable(OperationContext* opCtx) override;

    bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx, bool stableCheckpoint) override;

    BSONObj setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                    StringData flagName,
                                    boost::optional<bool> flagValue) const override;

    boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                    StringData flagName) const override;

    BSONObj getSanitizedStorageOptionsForSecondaryReplication(
        const BSONObj& options) const override;

    void dump() const override;

    Status autoCompact(RecoveryUnit&, const AutoCompactOptions& options) override;

    bool underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) override;

    size_t getCacheSizeMB() override;

    bool hasOngoingLiveRestore() override;

private:
    using CollIter = std::list<std::string>::iterator;

    /**
     * When called in a repair context (_options.forRepair=true), attempts to recover a collection
     * whose entry is present in the MDBCatalog, but missing from the KVEngine. Returns an
     * error Status if called outside of a repair context or the implementation of
     * KVEngine::recoverOrphanedIdent returns an error other than DataModifiedByRepair.
     *
     * Returns Status::OK if the collection was recovered in the KVEngine and a new record store was
     * created. Recovery does not make any guarantees about the integrity of the data in the
     * collection.
     */
    Status _recoverOrphanedCollection(OperationContext* opCtx,
                                      RecordId catalogId,
                                      const NamespaceString& collectionName,
                                      StringData collectionIdent);

    /**
     * Throws a fatal assertion if there are any missing index idents from the storage engine for
     * the given catalog entry.
     */
    void _checkForIndexFiles(OperationContext* opCtx,
                             const MDBCatalog::EntryIdentifier& entry,
                             std::vector<std::string>& identsKnownToStorageEngine) const;

    void _dumpCatalog(OperationContext* opCtx);

    /**
     * Called when the min of checkpoint timestamp (if exists) and oldest timestamp advances in the
     * KVEngine.
     */
    void _onMinOfCheckpointAndOldestTimestampChanged(OperationContext* opCtx,
                                                     const Timestamp& timestamp);

    class RemoveDBChange;

    // Main KVEngine instance used for all user tables.
    // This must be the first member so it is destroyed last.
    std::unique_ptr<KVEngine> _engine;

    // KVEngine instance that is used for creating SpillTables.
    std::unique_ptr<KVEngine> _spillEngine;

    const StorageEngineOptions _options;

    // Manages drop-pending idents. Requires access to '_engine'.
    KVDropPendingIdentReaper _dropPendingIdentReaper;

    // Listener for min of checkpoint and oldest timestamp changes.
    TimestampMonitor::TimestampListener _minOfCheckpointAndOldestTimestampListener;

    const bool _supportsCappedCollections;

    std::unique_ptr<RecordStore> _catalogRecordStore;
    std::unique_ptr<MDBCatalog> _catalog;

    // Flag variable that states if the storage engine is in backup mode.
    bool _inBackupMode = false;

    std::unique_ptr<TimestampMonitor> _timestampMonitor;

    // Stores a copy of the TimestampMonitor's listeners when temporarily stopping the monitor.
    std::vector<TimestampMonitor::TimestampListener*> _listeners;

    friend class StorageEngineTest;
};
}  // namespace mongo
