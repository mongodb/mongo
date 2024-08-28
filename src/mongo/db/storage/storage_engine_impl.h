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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_drop_pending_ident_reaper.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_interface.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/uuid.h"

namespace mongo {

class DurableCatalog;
class KVEngine;

struct StorageEngineOptions {
    bool directoryPerDB = false;
    bool directoryForIndexes = false;
    bool forRepair = false;
    bool forRestore = false;
    bool lockFileCreatedByUncleanShutdown = false;
};

class StorageEngineImpl final : public StorageEngineInterface, public StorageEngine {
public:
    StorageEngineImpl(OperationContext* opCtx,
                      std::unique_ptr<KVEngine> engine,
                      StorageEngineOptions options = StorageEngineOptions());

    ~StorageEngineImpl() override;

    void notifyStorageStartupRecoveryComplete() override;

    void notifyReplStartupRecoveryComplete(OperationContext* opCtx) override;

    RecoveryUnit* newRecoveryUnit() override;

    std::vector<DatabaseName> listDatabases(
        boost::optional<TenantId> tenantId = boost::none) const override;

    bool supportsCappedCollections() const override {
        return _supportsCappedCollections;
    }

    Status dropDatabase(OperationContext* opCtx, const DatabaseName& dbName) override;
    Status dropCollectionsWithPrefix(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const std::string& collectionNamePrefix) override;

    void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) override;

    Status beginBackup(OperationContext* opCtx) override;

    void endBackup(OperationContext* opCtx) override;

    Status disableIncrementalBackup(OperationContext* opCtx) override;

    StatusWith<std::unique_ptr<StreamingCursor>> beginNonBlockingBackup(
        OperationContext* opCtx, const BackupOptions& options) override;

    void endNonBlockingBackup(OperationContext* opCtx) override;

    StatusWith<std::deque<std::string>> extendBackupCursor(OperationContext* opCtx) override;

    bool supportsCheckpoints() const override;

    bool isEphemeral() const override;

    Status repairRecordStore(OperationContext* opCtx,
                             RecordId catalogId,
                             const NamespaceString& nss) override;

    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                   KeyFormat keyFormat) override;

    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreForResumableIndexBuild(
        OperationContext* opCtx, KeyFormat keyFormat) override;

    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreFromExistingIdent(
        OperationContext* opCtx, StringData ident, KeyFormat keyFormat) override;

    void cleanShutdown(ServiceContext* svcCtx) override;

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

    bool supportsReadConcernSnapshot() const final;

    bool supportsOplogTruncateMarkers() const final;

    void clearDropPendingState(OperationContext* opCtx) final;

    SnapshotManager* getSnapshotManager() const final;

    void setJournalListener(JournalListener* jl) final;

    /**
     * A TimestampMonitor is used to listen for any changes in the timestamps implemented by the
     * storage engine and to notify any registered listeners upon changes to these timestamps.
     *
     * The monitor follows the same lifecycle as the storage engine, started when the storage
     * engine starts and stopped when the storage engine stops.
     *
     * The PeriodicRunner must be started before the Storage Engine is started, and the Storage
     * Engine must be shutdown after the PeriodicRunner is shutdown.
     */
    class TimestampMonitor {
    public:
        /**
         * Timestamps that can be listened to for changes.
         */
        enum class TimestampType { kCheckpoint, kOldest, kStable, kMinOfCheckpointAndOldest };

        /**
         * A TimestampListener is used to listen for changes in a given timestamp and to execute the
         * user-provided callback to the change with a custom user-provided callback.
         *
         * The TimestampListener must be registered in the TimestampMonitor in order to be notified
         * of timestamp changes and react to changes for the duration it's part of the monitor.
         *
         * Listeners expected to run in standalone mode should handle Timestamp::min() notifications
         * appropriately.
         */
        class TimestampListener {
        public:
            // Caller must ensure that the lifetime of the variables used in the callback are valid.
            using Callback = std::function<void(OperationContext* opCtx, Timestamp timestamp)>;

            /**
             * A TimestampListener saves a 'callback' that will be executed whenever the specified
             * 'type' timestamp changes. The 'callback' function will be passed the new 'type'
             * timestamp.
             */
            TimestampListener(TimestampType type, Callback callback)
                : _type(type), _callback(std::move(callback)) {}

            /**
             * Executes the appropriate function with the callback of the listener with the new
             * timestamp.
             */
            void notify(OperationContext* opCtx, Timestamp newTimestamp) {
                if (_type == TimestampType::kCheckpoint)
                    _onCheckpointTimestampChanged(opCtx, newTimestamp);
                else if (_type == TimestampType::kOldest)
                    _onOldestTimestampChanged(opCtx, newTimestamp);
                else if (_type == TimestampType::kStable)
                    _onStableTimestampChanged(opCtx, newTimestamp);
                else if (_type == TimestampType::kMinOfCheckpointAndOldest)
                    _onMinOfCheckpointAndOldestTimestampChanged(opCtx, newTimestamp);
            }

            TimestampType getType() const {
                return _type;
            }

        private:
            void _onCheckpointTimestampChanged(OperationContext* opCtx, Timestamp newTimestamp) {
                _callback(opCtx, newTimestamp);
            }

            void _onOldestTimestampChanged(OperationContext* opCtx, Timestamp newTimestamp) {
                _callback(opCtx, newTimestamp);
            }

            void _onStableTimestampChanged(OperationContext* opCtx, Timestamp newTimestamp) {
                _callback(opCtx, newTimestamp);
            }

            void _onMinOfCheckpointAndOldestTimestampChanged(OperationContext* opCtx,
                                                             Timestamp newTimestamp) {
                _callback(opCtx, newTimestamp);
            }

            // Timestamp type this listener monitors.
            TimestampType _type;

            // Function to execute when the timestamp changes.
            Callback _callback;
        };

        /**
         * Starts monitoring timestamp changes in the background with an initial listener.
         */
        TimestampMonitor(KVEngine* engine, PeriodicRunner* runner);

        ~TimestampMonitor();

        /**
         * Adds a new listener to the monitor if it isn't already registered. A listener can only be
         * bound to one type of timestamp at a time.
         */
        void addListener(TimestampListener* listener);

        /**
         * Remove a listener.
         */
        void removeListener(TimestampListener* listener);

        bool isRunning_forTestOnly() const {
            return _running;
        }

    private:
        /**
         * Monitor changes in timestamps and to notify the listeners on change. Notifies all
         * listeners on Timestamp::min() in order to support standalone mode that is untimestamped.
         */
        void _startup();

        KVEngine* _engine;
        bool _running = false;
        bool _shuttingDown = false;

        // Periodic runner that the timestamp monitor schedules its job on.
        PeriodicRunner* _periodicRunner;

        // Protects access to _listeners below.
        Mutex _monitorMutex = MONGO_MAKE_LATCH("TimestampMonitor::_monitorMutex");
        std::vector<TimestampListener*> _listeners;

        // This should remain as the last member variable so that its destructor gets executed first
        // when the class instance is being deconstructed. This causes the PeriodicJobAnchor to stop
        // the PeriodicJob, preventing us from accessing any destructed variables if this were to
        // run during the destruction of this class instance.
        PeriodicJobAnchor _job;
    };

    StorageEngine* getStorageEngine() override {
        return this;
    }

    KVEngine* getEngine() override {
        return _engine.get();
    }

    const KVEngine* getEngine() const override {
        return _engine.get();
    }

    void addDropPendingIdent(
        const std::variant<Timestamp, StorageEngine::CheckpointIteration>& dropTime,
        std::shared_ptr<Ident> ident,
        DropIdentCallback&& onDrop) override;

    void dropIdentsOlderThan(OperationContext* opCtx, const Timestamp& ts) override;

    std::shared_ptr<Ident> markIdentInUse(StringData ident) override;

    void startTimestampMonitor() override;

    void checkpoint() override;

    StorageEngine::CheckpointIteration getCheckpointIteration() const override;

    bool hasDataBeenCheckpointed(
        StorageEngine::CheckpointIteration checkpointIteration) const override;

    StatusWith<ReconcileResult> reconcileCatalogAndIdents(
        OperationContext* opCtx, Timestamp stableTs, LastShutdownState lastShutdownState) override;

    std::string getFilesystemPathForDb(const DatabaseName& dbName) const override;

    DurableCatalog* getCatalog() override;

    const DurableCatalog* getCatalog() const override;

    /**
     * When loading after an unclean shutdown, this performs cleanup on the DurableCatalog.
     */
    void loadCatalog(OperationContext* opCtx,
                     boost::optional<Timestamp> stableTs,
                     LastShutdownState lastShutdownState) final;

    void closeCatalog(OperationContext* opCtx) final;

    TimestampMonitor* getTimestampMonitor() const {
        return _timestampMonitor.get();
    }

    std::set<std::string> getDropPendingIdents() const override {
        return _dropPendingIdentReaper.getAllIdentNames();
    }

    size_t getNumDropPendingIdents() const override {
        return _dropPendingIdentReaper.getNumIdents();
    }

    int64_t sizeOnDiskForDb(OperationContext* opCtx, const DatabaseName& dbName) override;

    bool isUsingDirectoryPerDb() const override {
        return _options.directoryPerDB;
    }

    bool isUsingDirectoryForIndexes() const override {
        return _options.directoryForIndexes;
    }

    StatusWith<Timestamp> pinOldestTimestamp(OperationContext* opCtx,
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

    BSONObj getSanitizedStorageOptionsForSecondaryReplication(
        const BSONObj& options) const override;

    void dump() const override;

    Status autoCompact(OperationContext* opCtx, const AutoCompactOptions& options) override;

private:
    using CollIter = std::list<std::string>::iterator;

    void _initCollection(OperationContext* opCtx,
                         RecordId catalogId,
                         const NamespaceString& nss,
                         bool forRepair,
                         Timestamp minValidTs);

    Status _dropCollections(OperationContext* opCtx,
                            const std::vector<UUID>& toDrop,
                            const std::string& collectionNamePrefix = "");

    /**
     * When called in a repair context (_options.forRepair=true), attempts to recover a collection
     * whose entry is present in the DurableCatalog, but missing from the KVEngine. Returns an
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
                             const DurableCatalog::EntryIdentifier& entry,
                             std::vector<std::string>& identsKnownToStorageEngine) const;

    void _dumpCatalog(OperationContext* opCtx);

    /**
     * Called when the min of checkpoint timestamp (if exists) and oldest timestamp advances in the
     * KVEngine.
     */
    void _onMinOfCheckpointAndOldestTimestampChanged(OperationContext* opCtx,
                                                     const Timestamp& timestamp);

    /**
     * Returns whether the given ident is an internal ident and if it should be dropped or used to
     * resume an index build.
     */
    bool _handleInternalIdent(OperationContext* opCtx,
                              const std::string& ident,
                              LastShutdownState lastShutdownState,
                              ReconcileResult* reconcileResult,
                              std::set<std::string>* internalIdentsToKeep,
                              std::set<std::string>* allInternalIdents);

    class RemoveDBChange;

    // This must be the first member so it is destroyed last.
    std::unique_ptr<KVEngine> _engine;

    const StorageEngineOptions _options;

    // Manages drop-pending idents. Requires access to '_engine'.
    KVDropPendingIdentReaper _dropPendingIdentReaper;

    // Listener for min of checkpoint and oldest timestamp changes.
    TimestampMonitor::TimestampListener _minOfCheckpointAndOldestTimestampListener;

    // Listener for cleanup of CollectionCatalog when oldest timestamp advances.
    TimestampMonitor::TimestampListener _collectionCatalogCleanupTimestampListener;

    const bool _supportsCappedCollections;

    std::unique_ptr<RecordStore> _catalogRecordStore;
    std::unique_ptr<DurableCatalog> _catalog;

    // Flag variable that states if the storage engine is in backup mode.
    bool _inBackupMode = false;

    std::unique_ptr<TimestampMonitor> _timestampMonitor;
};
}  // namespace mongo
