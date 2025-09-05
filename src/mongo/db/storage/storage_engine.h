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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index_builds/index_builds.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/str.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/serialization/strong_typedef.hpp>

namespace mongo {

class KVBackupBlock;
class JournalListener;
class MDBCatalog;
class KVEngine;
class OperationContext;
class RecoveryUnit;
class SnapshotManager;
class StorageEngineLockFile;
class StorageEngineMetadata;

struct StorageGlobalParams;

// StorageEngine constants
const NamespaceString kCatalogInfoNamespace = NamespaceString(DatabaseName::kMdbCatalog);
const auto kResumableIndexIdentStem = "resumable-index-build-"_sd;

/**
 * The StorageEngine class is the top level interface for creating a new storage engine. All
 * StorageEngine(s) must be registered by calling registerFactory in order to possibly be
 * activated.
 */
class StorageEngine {
public:
    /**
     * This is the minimum valid timestamp; it can be used for reads that need to see all
     * untimestamped data but no timestamped data. We cannot use 0 here because 0 means see all
     * timestamped data. The high-order 4 bytes are for the seconds field in a Timestamp object.
     */
    static const unsigned long long kMinimumTimestamp = 1ULL << 32;

    /**
     * When the storage engine needs to know how much oplog to preserve for the sake of active
     * transactions, it executes a callback that returns either the oldest active transaction
     * timestamp, or boost::none if there is no active transaction, or an error if it fails.
     */
    using OldestActiveTransactionTimestampResult = StatusWith<boost::optional<Timestamp>>;
    using OldestActiveTransactionTimestampCallback =
        std::function<OldestActiveTransactionTimestampResult(Timestamp stableTimestamp)>;

    using DropIdentCallback = std::function<void()>;

    /**
     * Information on last storage engine shutdown state that is relevant to the recovery process.
     * Determined by initializeStorageEngine() during mongod.lock initialization.
     */
    enum class LastShutdownState { kClean, kUnclean };

    /**
     * The interface for creating new instances of storage engines.
     *
     * A storage engine provides an instance of this class (along with an associated
     * name) to the global environment, which then sets the global storage engine
     * according to the provided configuration parameter.
     */
    class Factory {
    public:
        virtual ~Factory() {}

        /**
         * Return a new instance of the StorageEngine. Caller owns the returned pointer.
         */
        virtual std::unique_ptr<StorageEngine> create(OperationContext* opCtx,
                                                      const StorageGlobalParams& params,
                                                      const StorageEngineLockFile* lockFile,
                                                      bool isReplSet,
                                                      bool shouldRecoverFromOplogAsStandalone,
                                                      bool inStandaloneMode) const = 0;

        /**
         * Returns the name of the storage engine.
         *
         * Implementations that change the value of the returned string can cause
         * data file incompatibilities.
         */
        virtual StringData getCanonicalName() const = 0;

        /**
         * Validates creation options for a collection in the StorageEngine.
         * Returns an error if the creation options are not valid.
         *
         * Default implementation only accepts empty objects (no options).
         */
        virtual Status validateCollectionStorageOptions(const BSONObj& options) const {
            if (options.isEmpty())
                return Status::OK();
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "storage engine " << getCanonicalName()
                                        << " does not support any collection storage options");
        }

        /**
         * Validates creation options for an index in the StorageEngine.
         * Returns an error if the creation options are not valid.
         *
         * Default implementation only accepts empty objects (no options).
         */
        virtual Status validateIndexStorageOptions(const BSONObj& options) const {
            if (options.isEmpty())
                return Status::OK();
            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "storage engine " << getCanonicalName()
                                        << " does not support any index storage options");
        }

        /**
         * Validates existing metadata in the data directory against startup options.
         * Returns an error if the storage engine initialization should not proceed
         * due to any inconsistencies between the current startup options and the creation
         * options stored in the metadata.
         */
        virtual Status validateMetadata(const StorageEngineMetadata& metadata,
                                        const StorageGlobalParams& params) const = 0;

        /**
         * Returns a new document suitable for storing in the data directory metadata.
         * This document will be used by validateMetadata() to check startup options
         * on restart.
         */
        virtual BSONObj createMetadataOptions(const StorageGlobalParams& params) const = 0;

        /**
         * Returns whether the engine supports queryable backup mode. If queryable backup mode is
         * enabled, user writes are not permitted but internally generated writes are still
         * permitted.
         */
        virtual bool supportsQueryableBackupMode() const {
            return false;
        }
    };

    /**
     * The destructor should only be called if we are tearing down but not exiting the process.
     */
    virtual ~StorageEngine() {}

    /**
     * During the startup process, the storage engine is one of the first components to be started
     * up and fully initialized. But that fully initialized storage engine may not be recognized as
     * the end for the remaining storage startup tasks that still need to be performed.
     *
     * For example, after the storage engine has been fully initialized, we need to access it in
     * order to set up all of the collections and indexes based on the metadata, or perform some
     * corrective measures on the data files, etc.
     *
     * When all of the storage startup tasks are completed as a whole, then this function is called
     * by the external force managing the startup process.
     */
    virtual void notifyStorageStartupRecoveryComplete() {}

    /**
     * Perform any operations in the storage layer that are unblocked now that the server has exited
     * replication startup recovery and considers itself stable.
     *
     * This will be called during a node's transition to steady state replication.
     *
     * This function may race with shutdown. As a result, any steps within this function that should
     * not race with shutdown should obtain the global lock.
     */
    virtual void notifyReplStartupRecoveryComplete(RecoveryUnit&) {}

    /**
     * The storage engine can save several elements of ReplSettings on construction.  Standalone
     * mode is one such setting that can change after construction and need to be updated.
     */
    virtual void setInStandaloneMode() {}

    /**
     * Returns a new interface to the storage engine's recovery unit.  The recovery
     * unit is the durability interface.  For details, see recovery_unit.h
     */
    virtual std::unique_ptr<RecoveryUnit> newRecoveryUnit() = 0;

    /**
     * Returns whether the storage engine supports capped collections.
     */
    virtual bool supportsCappedCollections() const = 0;

    /**
     * Returns whether the storage engine supports checkpoints.
     */
    virtual bool supportsCheckpoints() const = 0;

    /**
     * Returns true if the KVEngine is ephemeral -- that is, it is NOT persistent and all data is
     * lost after shutdown. Otherwise, returns false.
     */
    virtual bool isEphemeral() const = 0;

    /**
     * Populates and tears down in-memory data structures, respectively. Only required for storage
     * engines that support recoverToStableTimestamp().
     *
     * Must be called with the global lock acquired in exclusive mode.
     *
     * Unrecognized idents require special handling based on the context known only to the
     * caller. For example, on starting from a previous unclean shutdown, we may try to recover
     * orphaned idents, which are known to the storage engine but not referenced in the catalog.
     */
    virtual void loadMDBCatalog(OperationContext* opCtx, LastShutdownState lastShutdownState) = 0;
    virtual void closeMDBCatalog(OperationContext* opCtx) = 0;

    /**
     * Checkpoints the data to disk.
     *
     * 'callerHoldsReadLock' signals whether the caller holds a read lock. A write lock may be taken
     * internally, but will be skipped for callers holding a read lock because a write lock would
     * conflict. The JournalListener will not be updated in this case.
     */
    virtual void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) = 0;

    /**
     * Transitions the storage engine into backup mode.
     *
     * During backup mode the storage engine must stabilize its on-disk files, and avoid
     * any internal processing that may involve file I/O, such as online compaction, so
     * a filesystem level backup may be performed.
     *
     * Storage engines that do not support this feature should use the default implementation.
     * Storage engines that implement this must also implement endBackup().
     *
     * For Storage engines that implement beginBackup the _inBackupMode variable is provided
     * to avoid multiple instance enterting/leaving backup concurrently.
     *
     * If this function returns an OK status, MongoDB can call endBackup to signal the storage
     * engine that filesystem writes may continue. This function should return a non-OK status if
     * filesystem changes cannot be stopped to allow for online backup. If the function should be
     * retried, returns a non-OK status. This function may throw a WriteConflictException, which
     * should trigger a retry by the caller. All other exceptions should be treated as errors.
     */
    virtual Status beginBackup() = 0;

    /**
     * Transitions the storage engine out of backup mode.
     *
     * Storage engines that do not support this feature should use the default implementation.
     *
     * Storage engines implementing this feature should fassert when unable to leave backup mode.
     */
    virtual void endBackup() = 0;

    /**
     * Disables the storage of incremental backup history until a subsequent incremental backup
     * cursor is requested.
     *
     * The storage engine must release all incremental backup information and resources.
     */
    virtual Status disableIncrementalBackup() = 0;

    /**
     * Returns the timestamp of the checkpoint that the backup cursor is opened on.
     */
    virtual Timestamp getBackupCheckpointTimestamp() = 0;

    /**
     * Represents the options that the storage engine can use during full and incremental backups.
     *
     * When performing a full backup where incrementalBackup=false, the values of 'blockSizeMB',
     * 'thisBackupName', and 'srcBackupName' should not be modified.
     *
     * When performing an incremental backup where incrementalBackup=true, we first need a basis for
     * future incremental backups. This first basis (named 'thisBackupName'), which is a full
     * backup, must pass incrementalBackup=true and should not set 'srcBackupName'. An incremental
     * backup will include changed blocks since 'srcBackupName' was taken. This backup (also named
     * 'thisBackupName') will then become the basis for future incremental backups.
     *
     * Note that 'thisBackupName' must exist if and only if incrementalBackup=true while
     * 'srcBackupName' must not exist if incrementalBackup=false but may or may not exist if
     * incrementalBackup=true.
     */
    struct BackupOptions {
        bool disableIncrementalBackup = false;
        bool incrementalBackup = false;
        int blockSizeMB = 16;
        bool takeCheckpoint = true;
        boost::optional<std::string> thisBackupName;
        boost::optional<std::string> srcBackupName;
    };

    /**
     * Abstract class required for streaming both full and incremental backups. The function
     * getNextBatch() returns a vector containing 'batchSize' or less BackupBlocks. The
     * StreamingCursor has been exhausted if getNextBatch() returns an empty vector.
     */
    class StreamingCursor {
    public:
        StreamingCursor() = delete;
        explicit StreamingCursor(BackupOptions options) : options(options) {};

        virtual ~StreamingCursor() = default;

        virtual StatusWith<std::deque<KVBackupBlock>> getNextBatch(std::size_t batchSize) = 0;

    protected:
        BackupOptions options;
    };

    virtual StatusWith<std::unique_ptr<StreamingCursor>> beginNonBlockingBackup(
        const BackupOptions& options) = 0;

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

        /**
         * Returns registered listeners.
         */
        std::vector<TimestampListener*> getListeners();

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
        stdx::mutex _monitorMutex;
        std::vector<TimestampListener*> _listeners;

        // This should remain as the last member variable so that its destructor gets executed first
        // when the class instance is being deconstructed. This causes the PeriodicJobAnchor to stop
        // the PeriodicJob, preventing us from accessing any destructed variables if this were to
        // run during the destruction of this class instance.
        PeriodicJobAnchor _job;
    };

    virtual void endNonBlockingBackup() = 0;

    virtual StatusWith<std::deque<std::string>> extendBackupCursor() = 0;

    /**
     * Recover as much data as possible from a potentially corrupt RecordStore.
     * This only recovers the record data, not indexes or anything else.
     *
     * The Collection object for this namespace should be destructed and recreated after the call,
     * because the old object has no associated RecordStore when started in repair mode.
     */
    virtual Status repairRecordStore(OperationContext* opCtx,
                                     RecordId catalogId,
                                     const NamespaceString& nss) = 0;

    /**
     * Creates a temporary table that can be used for spilling in-memory state to disk. A table
     * created using this API does not interfere with the reads/writes happening on the main
     * KVEngine instance. This table is automatically dropped when the returned handle is
     * destructed. If the available disk space falls below thresholdBytes, writes to the spill table
     * will fail.
     */
    virtual std::unique_ptr<SpillTable> makeSpillTable(OperationContext* opCtx,
                                                       KeyFormat keyFormat,
                                                       int64_t thresholdBytes) = 0;

    /**
     * Removes knowledge of this ident from the SpillEngine. The ident and RecoveryUnit must
     * belong to the same SpillTable. To call this method, the StorageEngine must be initialized
     * with a spill engine.
     */
    virtual void dropSpillTable(RecoveryUnit& ru, StringData ident) = 0;

    /**
     * Creates a temporary RecordStore on the storage engine. If an ident is provided, uses it for
     * the new table. If an ident is not provided, generates a new unique ident. On startup after an
     * unclean shutdown, the storage engine will drop any un-dropped temporary record stores.
     */
    virtual std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                           StringData ident,
                                                                           KeyFormat keyFormat) = 0;

    /**
     * Creates a temporary RecordStore on the storage engine for a resumable index build. On
     * startup after an unclean shutdown, the storage engine will drop any un-dropped temporary
     * record stores.
     */
    virtual std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreForResumableIndexBuild(
        OperationContext* opCtx, KeyFormat keyFormat) = 0;

    /**
     * Creates a temporary RecordStore on the storage engine from an existing ident on disk. On
     * startup after an unclean shutdown, the storage engine will drop any un-dropped temporary
     * record stores.
     */
    virtual std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreFromExistingIdent(
        OperationContext* opCtx, StringData ident, KeyFormat keyFormat) = 0;

    /**
     * This method will be called before there is a clean shutdown.  Storage engines should
     * override this method if they have clean-up to do that is different from unclean shutdown.
     * MongoDB will not call into the storage subsystem after calling this function.
     *
     * On error, the storage engine should assert and crash.
     * There is intentionally no uncleanShutdown().
     */
    virtual void cleanShutdown(ServiceContext* svcCtx, bool memLeakAllowed) = 0;

    /**
     * Returns the SnapshotManager for this StorageEngine or NULL if not supported.
     *
     * Pointer remains owned by the StorageEngine, not the caller.
     */
    virtual SnapshotManager* getSnapshotManager() const = 0;

    /**
     * Sets a new JournalListener, which is used by the storage engine to alert the rest of the
     * system about journaled write progress.
     *
     * This may only be set once.
     */
    virtual void setJournalListener(JournalListener* jl) = 0;

    /**
     * Returns whether the storage engine supports "recover to stable timestamp". Returns true
     * if the storage engine supports "recover to stable timestamp" but does not currently have
     * a stable timestamp. In that case StorageEngine::recoverToStableTimestamp() will return
     * a bad status.
     */
    virtual bool supportsRecoverToStableTimestamp() const = 0;

    /**
     * Returns whether the storage engine can provide a recovery timestamp.
     */
    virtual bool supportsRecoveryTimestamp() const = 0;

    /**
     * Returns true if the storage engine supports the readConcern level "snapshot".
     */
    virtual bool supportsReadConcernSnapshot() const = 0;

    /**
     * Returns a set of drop pending idents inside the storage engine.
     */
    virtual std::set<std::string> getDropPendingIdents() const = 0;

    /**
     * Returns the number of drop pending idents inside the storage engine.
     */
    virtual size_t getNumDropPendingIdents() const = 0;

    /**
     * Removes the drop-pending state for indexes / collections with a timestamped drop time.
     * Effectively aborts the second phase of a two phase table drop for eligible idents.
     *
     * This function is primarily used for rollback after recovering to a stable timestamp - and
     * clears drop-pending entries in case drops were rolled back. Because of this, only timestamped
     * drops are affected as non-timestamped drops cannot be rolled back.
     */
    virtual void clearDropPendingState(OperationContext* opCtx) = 0;

    /**
     * If the given ident has been registered with the reaper, attempts to immediately drop it,
     * possibly blocking while the background thread is reaping idents. Returns ObjectIsBusy if the
     * ident could not be dropped due to being in use. Returns Status::OK() if the ident was not
     * tracked as the reaper cannot distinguish "ident has already been dropped" from "ident was
     * never drop pending".
     */
    virtual Status immediatelyCompletePendingDrop(OperationContext* opCtx, StringData ident) = 0;

    BOOST_STRONG_TYPEDEF(uint64_t, CheckpointIteration);

    /**
     * Adds 'ident' to a list of indexes/collections whose data will be dropped when:
     * - the 'dropTime' is sufficiently old to ensure no future data accesses
     * - and no holders of 'ident' remain (the index/collection is no longer in active use)
     *
     * 'dropTime' can be either a CheckpointIteration or a Timestamp. In the case of a Timestamp the
     * ident will be dropped when we can guarantee that no other operation can access the ident.
     * CheckpointIteration should be chosen when performing untimestamped drops as they
     * will make the ident wait for a catalog checkpoint before proceeding with the ident drop.
     */
    virtual void addDropPendingIdent(const std::variant<Timestamp, CheckpointIteration>& dropTime,
                                     std::shared_ptr<Ident> ident,
                                     DropIdentCallback&& onDrop = nullptr) = 0;

    /**
     * Marks the ident as in use and prevents the reaper from dropping the ident.
     *
     * Returns nullptr if the ident is not known to the reaper, is already being dropped, or is
     * already dropped.
     */
    virtual std::shared_ptr<Ident> markIdentInUse(StringData ident) = 0;

    /**
     * Starts the timestamp monitor. This periodically drops idents queued by addDropPendingIdent,
     * and removes historical ident entries no longer necessary.
     */
    virtual void startTimestampMonitor(
        std::initializer_list<TimestampMonitor::TimestampListener*> listeners) = 0;

    /**
     * Used by recoverToStableTimestamp() to prevent the TimestampMonitor from accessing the catalog
     * concurrently during rollback.
     */
    virtual void stopTimestampMonitor() = 0;
    virtual void restartTimestampMonitor() = 0;

    /**
     * Called when the checkpoint thread instructs the storage engine to take a checkpoint. The
     * underlying storage engine must take a checkpoint at this point.
     * Acquires a resource mutex before taking the checkpoint.
     */
    virtual void checkpoint() = 0;

    /**
     * Returns the checkpoint iteration the committed write will be part of.
     *
     * This token is only meaningful if obtained after a WriteUnitOfWork commit. You can use the
     * number with StorageEngine::hasDataBeenCheckpointed(CheckpointIteration) in order to check
     * whether the write has been checkpointed or not.
     *
     * Mostly of use for writes that are untimestamped. Timestamped writes should use the commit
     * time used and the durable timestamp.
     */
    virtual CheckpointIteration getCheckpointIteration() const = 0;

    /**
     * Returns whether the given checkpoint iteration has been durably flushed to disk.
     */
    virtual bool hasDataBeenCheckpointed(CheckpointIteration checkpointIteration) const = 0;

    /**
     * Recovers the storage engine state to the last stable timestamp. "Stable" in this case
     * refers to a timestamp that is guaranteed to never be rolled back. The stable timestamp
     * used should be one provided by StorageEngine::setStableTimestamp().
     *
     * The "local" database is exempt and should not roll back any state except for
     * "local.replset.minvalid" which must roll back to the last stable timestamp.
     *
     * If successful, returns the timestamp that the storage engine recovered to.
     *
     * fasserts if StorageEngine::supportsRecoverToStableTimestamp() would return
     * false. Returns a bad status if there is no stable timestamp to recover to.
     *
     * It is illegal to call this concurrently with `setStableTimestamp` or
     * `setInitialDataTimestamp`.
     */
    virtual StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) = 0;

    /**
     * Returns the stable timestamp that the storage engine recovered to on startup. If the
     * recovery point was not stable, returns "none".
     * fasserts if StorageEngine::supportsRecoverToStableTimestamp() would return false.
     */
    virtual boost::optional<Timestamp> getRecoveryTimestamp() const = 0;

    /**
     * Returns a timestamp that is guaranteed to exist on storage engine recovery to a stable
     * timestamp. This indicates when the storage engine can safely rollback to stable; and for
     * durable engines, it is also the guaranteed minimum stable recovery point on server restart
     * after crash or shutdown.
     *
     * fasserts if StorageEngine::supportsRecoverToStableTimestamp() would return false. Returns
     * boost::none if the recovery time has not yet been established. Replication recoverable
     * rollback may not succeed before establishment, and restart will require resync.
     */
    virtual boost::optional<Timestamp> getLastStableRecoveryTimestamp() const = 0;

    /**
     * Sets the last materialized LSN, marking the highest phylog LSN
     * that has been successfully written to the page server and should have no holes.
     *
     * TODO: Revisit how to handle cases where mongod speaks with a log server
     * in a non-local zone due to failover.
     */
    virtual void setLastMaterializedLsn(uint64_t lsn) = 0;

    /**
     * Configures the specified checkpoint as the starting point for recovery.
     */
    virtual void setRecoveryCheckpointMetadata(StringData checkpointMetadata) = 0;

    /**
     * Configures the storage engine as the leader, allowing it to flush checkpoints to remote
     * storage.
     */
    virtual void promoteToLeader() = 0;

    /**
     * Sets the highest timestamp at which the storage engine is allowed to take a checkpoint. This
     * timestamp must not decrease unless force=true is set, in which case we force the stable
     * timestamp, the oldest timestamp, and the commit timestamp backward.
     */
    virtual void setStableTimestamp(Timestamp stableTimestamp, bool force = false) = 0;

    /**
     * Returns the stable timestamp.
     */
    virtual Timestamp getStableTimestamp() const = 0;

    /**
     * Tells the storage engine the timestamp of the data at startup. This is necessary because
     * timestamps are not persisted in the storage layer.
     */
    virtual void setInitialDataTimestamp(Timestamp timestamp) = 0;

    /**
     * Returns the initial data timestamp.
     */
    virtual Timestamp getInitialDataTimestamp() const = 0;

    /**
     * Uses the current stable timestamp to set the oldest timestamp for which the storage engine
     * must maintain snapshot history through.
     *
     * oldest_timestamp will be set to stable_timestamp adjusted by
     * 'minSnapshotHistoryWindowInSeconds' to create a window of available snapshots on the
     * storage engine from oldest to stable. Furthermore, oldest_timestamp will never be set ahead
     * of the oplog read timestamp, ensuring the oplog reader's 'read_timestamp' can always be
     * serviced.
     */
    virtual void setOldestTimestampFromStable() = 0;

    /**
     * Sets the oldest timestamp for which the storage engine must maintain snapshot history
     * through. Additionally, all future writes must be newer or equal to this value.
     */
    virtual void setOldestTimestamp(Timestamp timestamp, bool force) = 0;

    /**
     * Gets the oldest timestamp for which the storage engine must maintain snapshot history
     * through.
     */
    virtual Timestamp getOldestTimestamp() const = 0;

    /**
     * Sets a callback which returns the timestamp of the oldest oplog entry involved in an
     * active MongoDB transaction. The storage engine calls this function to determine how much
     * oplog it must preserve.
     */
    virtual void setOldestActiveTransactionTimestampCallback(
        OldestActiveTransactionTimestampCallback callback) = 0;

    struct IndexIdentifier {
        const RecordId catalogId;
        const NamespaceString nss;
        const std::string indexName;
    };

    /*
     * ReconcileResult is the result of reconciling abandoned storage engine idents and unfinished
     * index builds.
     */
    struct ReconcileResult {
        // A map of unfinished two-phase indexes that must be restarted in the background, but
        // not to completion; they will wait for replicated commit or abort operations. This is a
        // mapping from index build UUID to index build.
        IndexBuilds indexBuildsToRestart;

        // List of index builds to be resumed. Each ResumeIndexInfo may contain multiple indexes to
        // resume as part of the same build.
        std::vector<ResumeIndexInfo> indexBuildsToResume;
    };

    /**
     * Returns the all_durable timestamp. All transactions with timestamps earlier than the
     * all_durable timestamp are committed.
     *
     * The all_durable timestamp is the in-memory no holes point. That does not mean that there are
     * no holes behind it on disk. The all_durable timestamp also might not correspond with any
     * oplog entry, but instead have a timestamp value between that of two oplog entries.
     *
     * The all_durable timestamp only includes non-prepared transactions that have been given a
     * commit_timestamp and prepared transactions that have been given a durable_timestamp.
     * Previously, the deprecated all_committed timestamp would also include prepared transactions
     * that were prepared but not committed which could make the stable timestamp briefly jump back.
     *
     * Returns kMinimumTimestamp if there have been no new writes since the storage engine started.
     */
    virtual Timestamp getAllDurableTimestamp() const = 0;

    /**
     * Returns the minimum possible Timestamp value in the oplog that replication may need for
     * recovery in the event of a crash.
     *
     * Returns boost::none when called on an ephemeral database.
     */
    virtual boost::optional<Timestamp> getOplogNeededForCrashRecovery() const = 0;

    /**
     * Returns oplog that may not be truncated. This method is a function of oplog needed for
     * rollback and oplog needed for crash recovery. This method considers different states the
     * storage engine can be running in, such as running in in-memory mode.
     *
     * This method returning Timestamp::min() implies no oplog should be truncated and
     * Timestamp::max() means oplog can be truncated freely based on user oplog size configuration.
     */
    virtual Timestamp getPinnedOplog() const = 0;

    /**
     * Returns the path to the directory which has the data files of database with `dbName`.
     */
    virtual std::string getFilesystemPathForDb(const DatabaseName& dbName) const = 0;

    virtual std::string generateNewCollectionIdent(const DatabaseName& dbName) const = 0;
    virtual std::string generateNewIndexIdent(const DatabaseName& dbName) const = 0;

    /**
     * Generates a unique ident for an internal table that can be used to create a temporary
     * RecordStore instance using makeTemporaryRecordStore().
     */
    std::string generateNewInternalIdent() const {
        return ident::generateNewInternalIdent();
    }

    std::string generateNewInternalIndexBuildIdent(StringData identStem,
                                                   StringData indexIdent) const {
        return ident::generateNewInternalIndexBuildIdent(identStem, indexIdent);
    }

    virtual std::vector<std::string> generateNewIndexIdents(const DatabaseName& dbName,
                                                            size_t count) const {
        std::vector<std::string> idents;
        idents.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            idents.push_back(generateNewIndexIdent(dbName));
        }
        return idents;
    }

    /**
     * Returns true if this storage engine stores all data files directly in the dbPath, and not in
     * subdirectories of that path or in some other place.
     */
    virtual bool storesFilesInDbPath() const = 0;

    virtual int64_t getIdentSize(RecoveryUnit&, StringData ident) const = 0;

    virtual KVEngine* getEngine() = 0;
    virtual const KVEngine* getEngine() const = 0;
    virtual KVEngine* getSpillEngine() = 0;
    virtual const KVEngine* getSpillEngine() const = 0;
    virtual MDBCatalog* getMDBCatalog() = 0;
    virtual const MDBCatalog* getMDBCatalog() const = 0;
    virtual std::set<std::string> getDropPendingIdents() = 0;

    /**
     * A service that would like to pin the oldest timestamp registers its request here. If the
     * request can be satisfied, OK is returned with the oldest timestamp the caller can
     * use. Otherwise, SnapshotTooOld is returned. See the following enumerations:
     *
     * | Timestamp Relation  | roundUpIfTooOld | Result                    |
     * |---------------------+-----------------+---------------------------|
     * | requested >= oldest | false/true      | <OK, requested timestamp> |
     * | requested < oldest  | false           | <SnapshotTooOld>          |
     * | requested < oldest  | true            | <OK, oldest timestamp>    |
     *
     * If the input OperationContext is in a WriteUnitOfWork, an `onRollback` handler will be
     * registered to return the pin for the `requestingServiceName` to the previous value.
     */
    virtual StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                                     const std::string& requestingServiceName,
                                                     Timestamp requestedTimestamp,
                                                     bool roundUpIfTooOld) = 0;

    /**
     * Unpins the request registered under `requestingServiceName`.
     */
    virtual void unpinOldestTimestamp(const std::string& requestingServiceName) = 0;

    /**
     * Prevents oplog history at 'pinnedTimestamp' and later from being truncated. Setting
     * Timestamp::max() effectively nullifies the pin because no oplog truncation will be stopped by
     * it.
     */
    virtual void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) = 0;

    /**
     * When we write to an oplog, we call this so that that the storage engine can manage the
     * visibility of oplog entries to ensure they are ordered.
     *
     * Since this is called inside of a WriteUnitOfWork while holding a std::mutex, it is
     * illegal to acquire any LockManager locks inside of this function.
     *
     * If `orderedCommit` is true, the storage engine can assume the input `opTime` has become
     * visible in the oplog. Otherwise the storage engine must continue to maintain its own
     * visibility management. Calls with `orderedCommit` true will not be concurrent with calls of
     * `orderedCommit` false.
     */
    virtual Status oplogDiskLocRegister(OperationContext* opCtx,
                                        RecordStore* oplogRecordStore,
                                        const Timestamp& opTime,
                                        bool orderedCommit) = 0;

    /**
     * Waits for all writes that completed before this call to be visible to forward scans.
     * See the comment on RecordCursor for more details about the visibility rules.
     *
     * It is only legal to call this on an oplog. It is illegal to call this inside a
     * WriteUnitOfWork.
     */
    virtual void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                         RecordStore* oplogRecordStore) const = 0;

    /**
     * Waits until all commits that happened before this call are durable in the journal. Returns
     * true, unless the storage engine cannot guarantee durability, which should never happen when
     * the engine is non-ephemeral. This cannot be called from inside a unit of work, and should
     * fail if it is. This method invariants if the caller holds any locks, except for repair.
     *
     * Can throw write interruption errors from the JournalListener.
     */
    virtual bool waitUntilDurable(OperationContext* opCtx) = 0;

    /**
     * Unlike `waitUntilDurable`, this method takes a stable checkpoint, making durable any writes
     * on unjournaled tables that are behind the current stable timestamp. If the storage engine
     * is starting from an "unstable" checkpoint or 'stableCheckpoint'=false, this method call will
     * turn into an unstable checkpoint.
     *
     * This must not be called by a system taking user writes until after a stable timestamp is
     * passed to the storage engine.
     */
    virtual bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                                   bool stableCheckpoint) = 0;

    /**
     * Sets an optional boolean value (true / false / unset) associated to an arbitrary
     * `flagName` key on the storage engine options BSON object of a collection / index.
     * The way the flag is stored in the BSON object is engine-specific, and callers should only
     * assume that the persisted value can be later recovered using `getFlagFromStorageOptions`.
     *
     * This method only exists to support a critical fix (SERVER-91195), which required introducing
     * a backportable way to persist boolean flags; do not add new usages.
     * TODO SERVER-92265 evaluate getting rid of this method.
     */
    virtual BSONObj setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                            StringData flagName,
                                            boost::optional<bool> flagValue) const = 0;

    /**
     * Gets an optional boolean flag (true / false / unset) associated to an arbitrary
     * `flagName` key on the storage engine options BSON object of a collection / index,
     * as previously set by `setFlagToStorageOptions`.
     * The default value, if one has not been previously set, is the unset state (`boost::none`).
     *
     * TODO SERVER-92265 evaluate getting rid of this method.
     */
    virtual boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                            StringData flagName) const = 0;

    /**
     * Returns the input storage engine options, sanitized to remove options that may not apply to
     * this node, such as encryption. Might be called for both collection and index options. See
     * SERVER-68122.
     *
     * TODO SERVER-81069: Remove this since it's intrinsically tied to encryption options only.
     */
    virtual BSONObj getSanitizedStorageOptionsForSecondaryReplication(
        const BSONObj& options) const = 0;
    /**
     * Instructs the storage engine to dump its internal state.
     */
    virtual void dump() const = 0;

    /**
     * Toggles auto compact for a database. Auto compact periodically iterates through all of
     * the files available and runs compaction if they are eligible.
     */
    virtual Status autoCompact(RecoveryUnit&, const AutoCompactOptions& options) = 0;

    /**
     * Return true if the storage engine indicates that it is under cache pressure.
     */
    virtual bool underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) {
        return false;
    };

    /**
     * Return the size (in megabytes) allocated to the storage engine for cache.
     */
    virtual size_t getCacheSizeMB() {
        return 0;
    }

    /**
     * Returns whether the storage engine is currently trying to live-restore its database.
     */
    virtual bool hasOngoingLiveRestore() = 0;
};

}  // namespace mongo
