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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/index_builds.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/util/functional.h"
#include "mongo/util/str.h"

namespace mongo {

class JournalListener;
class DurableCatalog;
class KVEngine;
class OperationContext;
class RecoveryUnit;
class SnapshotManager;
class StorageEngineLockFile;
class StorageEngineMetadata;

struct StorageGlobalParams;

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
     * timestamped data.
     */
    static const uint64_t kMinimumTimestamp = 1;

    /**
     * When the storage engine needs to know how much oplog to preserve for the sake of active
     * transactions, it executes a callback that returns either the oldest active transaction
     * timestamp, or boost::none if there is no active transaction, or an error if it fails.
     */
    using OldestActiveTransactionTimestampResult = StatusWith<boost::optional<Timestamp>>;
    using OldestActiveTransactionTimestampCallback =
        std::function<OldestActiveTransactionTimestampResult(Timestamp stableTimestamp)>;

    struct BackupOptions {
        bool disableIncrementalBackup = false;
        bool incrementalBackup = false;
        int blockSizeMB = 16;
        boost::optional<std::string> thisBackupName;
        boost::optional<std::string> srcBackupName;
    };

    struct BackupBlock {
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
    };

    /**
     * Contains the size of the file to be backed up. This allows the backup application to safely
     * truncate the file for incremental backups. Files that have had changes since the last
     * incremental backup will have their changed file blocks listed.
     */
    struct BackupFile {
        BackupFile() = delete;
        explicit BackupFile(std::uint64_t fileSize) : fileSize(fileSize){};

        std::uint64_t fileSize;
        std::vector<BackupBlock> blocksToCopy;
    };

    // Map of filenames to backup file information.
    using BackupInformation = stdx::unordered_map<std::string, BackupFile>;

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
         * Return a new instance of the StorageEngine. The lockFile parameter may be null if
         * params.readOnly is set. Caller owns the returned pointer.
         */
        virtual StorageEngine* create(const StorageGlobalParams& params,
                                      const StorageEngineLockFile* lockFile) const = 0;

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
         * Returns whether the engine supports read-only mode. If read-only mode is enabled, the
         * engine may be started on a read-only filesystem (either mounted read-only or with
         * read-only permissions). If readOnly mode is enabled, it is undefined behavior to call
         * methods that write data (e.g. insertRecord). This method is provided on the Factory
         * because it must be called before the storageEngine is instantiated.
         */
        virtual bool supportsReadOnly() const {
            return false;
        }
    };

    /**
     * The destructor should only be called if we are tearing down but not exiting the process.
     */
    virtual ~StorageEngine() {}

    /**
     * Called after the globalStorageEngine pointer has been set up, before any other methods
     * are called. Any initialization work that requires the ability to create OperationContexts
     * should be done here rather than in the constructor.
     */
    virtual void finishInit() = 0;

    /**
     * Returns a new interface to the storage engine's recovery unit.  The recovery
     * unit is the durability interface.  For details, see recovery_unit.h
     *
     * Caller owns the returned pointer.
     */
    virtual RecoveryUnit* newRecoveryUnit() = 0;

    /**
     * List the databases stored in this storage engine.
     */
    virtual std::vector<std::string> listDatabases() const = 0;

    /**
     * Returns whether the storage engine supports its own locking locking below the collection
     * level. If the engine returns true, MongoDB will acquire intent locks down to the
     * collection level and will assume that the engine will ensure consistency at the level of
     * documents. If false, MongoDB will lock the entire collection in Shared/Exclusive mode
     * for read/write operations respectively.
     */
    virtual bool supportsDocLocking() const = 0;

    /**
     * Returns whether the storage engine supports locking at a database level.
     */
    virtual bool supportsDBLocking() const = 0;

    /**
     * Returns whether the storage engine supports capped collections.
     */
    virtual bool supportsCappedCollections() const = 0;

    /**
     * Returns whether the storage engine supports checkpoints.
     */
    virtual bool supportsCheckpoints() const = 0;

    /**
     * Returns whether the engine supports a journalling concept or not.
     */
    virtual bool isDurable() const = 0;

    /**
     * Returns true if the engine does not persist data to disk; false otherwise.
     */
    virtual bool isEphemeral() const = 0;

    /**
     * Populates and tears down in-memory data structures, respectively. Only required for storage
     * engines that support recoverToStableTimestamp().
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    virtual void loadCatalog(OperationContext* opCtx) = 0;
    virtual void closeCatalog(OperationContext* opCtx) = 0;

    /**
     * Closes all file handles associated with a database.
     */
    virtual Status closeDatabase(OperationContext* opCtx, StringData db) = 0;

    /**
     * Deletes all data and metadata for a database.
     */
    virtual Status dropDatabase(OperationContext* opCtx, StringData db) = 0;

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
    virtual Status beginBackup(OperationContext* opCtx) = 0;

    /**
     * Transitions the storage engine out of backup mode.
     *
     * Storage engines that do not support this feature should use the default implementation.
     *
     * Storage engines implementing this feature should fassert when unable to leave backup mode.
     */
    virtual void endBackup(OperationContext* opCtx) = 0;

    /**
     * Disables the storage of incremental backup history until a subsequent incremental backup
     * cursor is requested.
     *
     * The storage engine must release all incremental backup information and resources.
     */
    virtual Status disableIncrementalBackup(OperationContext* opCtx) = 0;

    /**
     * When performing an incremental backup, we first need a basis for future incremental backups.
     * The basis will be a full backup called 'thisBackupName'. For future incremental backups, the
     * storage engine will take a backup called 'thisBackupName' which will contain the changes made
     * to data files since the backup named 'srcBackupName'.
     *
     * The storage engine must use an upper bound limit of 'blockSizeMB' when returning changed
     * file blocks.
     *
     * The first full backup meant for incremental and future incremental backups must pass
     * 'incrementalBackup' as true.
     * 'thisBackupName' must exist only if 'incrementalBackup' is true.
     * 'srcBackupName' must not exist when 'incrementalBackup' is false but may or may not exist
     * when 'incrementalBackup' is true.
     */
    virtual StatusWith<StorageEngine::BackupInformation> beginNonBlockingBackup(
        OperationContext* opCtx, const BackupOptions& options) = 0;

    virtual void endNonBlockingBackup(OperationContext* opCtx) = 0;

    virtual StatusWith<std::vector<std::string>> extendBackupCursor(OperationContext* opCtx) = 0;

    /**
     * Recover as much data as possible from a potentially corrupt RecordStore.
     * This only recovers the record data, not indexes or anything else.
     *
     * The Collection object for on this namespace will be destructed and invalidated. A new
     * Collection object will be created and it should be retrieved from the CollectionCatalog.
     */
    virtual Status repairRecordStore(OperationContext* opCtx,
                                     RecordId catalogId,
                                     const NamespaceString& nss) = 0;

    /**
     * Creates a temporary RecordStore on the storage engine. This record store will drop itself
     * automatically when it goes out of scope. This means the TemporaryRecordStore should not exist
     * any longer than the OperationContext used to create it. On startup, the storage engine will
     * drop any un-dropped temporary record stores.
     */
    virtual std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(
        OperationContext* opCtx) = 0;

    /**
     * This method will be called before there is a clean shutdown.  Storage engines should
     * override this method if they have clean-up to do that is different from unclean shutdown.
     * MongoDB will not call into the storage subsystem after calling this function.
     *
     * On error, the storage engine should assert and crash.
     * There is intentionally no uncleanShutdown().
     */
    virtual void cleanShutdown() = 0;

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

    virtual bool supportsReadConcernMajority() const = 0;

    /**
     * Returns true if the storage engine uses oplog stones to more finely control
     * deletion of oplog history, instead of the standard capped collection controls on
     * the oplog collection size.
     */
    virtual bool supportsOplogStones() const = 0;

    /**
     * Returns true if the storage engine supports deferring collection drops until the the storage
     * engine determines that the storage layer artifacts for the pending drops are no longer needed
     * based on the stable and oldest timestamps.
     */
    virtual bool supportsPendingDrops() const = 0;

    /**
     * Returns a set of drop pending idents inside the storage engine.
     */
    virtual std::set<std::string> getDropPendingIdents() const = 0;

    /**
     * Clears list of drop-pending idents in the storage engine.
     * Used primarily by rollback after recovering to a stable timestamp.
     */
    virtual void clearDropPendingState() = 0;

    /**
     * Returns true if the storage engine supports two phase index builds.
     */
    virtual bool supportsTwoPhaseIndexBuild() const = 0;

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
     * Sets the highest timestamp at which the storage engine is allowed to take a checkpoint. This
     * timestamp must not decrease unless force=true is set, in which case we force the stable
     * timestamp, the oldest timestamp, and the commit timestamp backward.
     */
    virtual void setStableTimestamp(Timestamp stableTimestamp, bool force = false) = 0;

    /**
     * Tells the storage engine the timestamp of the data at startup. This is necessary because
     * timestamps are not persisted in the storage layer.
     */
    virtual void setInitialDataTimestamp(Timestamp timestamp) = 0;

    /**
     * Returns the initial data timestamp.
     */
    virtual Timestamp getInitialDataTimestamp() = 0;

    /**
     * Uses the current stable timestamp to set the oldest timestamp for which the storage engine
     * must maintain snapshot history through.
     *
     * oldest_timestamp will be set to stable_timestamp adjusted by
     * 'targetSnapshotHistoryWindowInSeconds' to create a window of available snapshots on the
     * storage engine from oldest to stable. Furthermore, oldest_timestamp will never be set ahead
     * of the oplog read timestamp, ensuring the oplog reader's 'read_timestamp' can always be
     * serviced.
     */
    virtual void setOldestTimestampFromStable() = 0;

    /**
     * Sets the oldest timestamp for which the storage engine must maintain snapshot history
     * through. Additionally, all future writes must be newer or equal to this value.
     */
    virtual void setOldestTimestamp(Timestamp timestamp) = 0;

    /**
     * Sets a callback which returns the timestamp of the oldest oplog entry involved in an
     * active MongoDB transaction. The storage engine calls this function to determine how much
     * oplog it must preserve.
     */
    virtual void setOldestActiveTransactionTimestampCallback(
        OldestActiveTransactionTimestampCallback callback) = 0;

    /**
     * Indicates whether the storage engine cache is under pressure.
     *
     * Retrieves a cache pressure value in the range [0, 100] from the storage engine, and compares
     * it against storageGlobalParams.cachePressureThreshold, a dynamic server parameter, to
     * determine whether cache pressure is too high.
     */
    virtual bool isCacheUnderPressure(OperationContext* opCtx) const = 0;

    /**
     * For unit tests only. Sets the cache pressure value with which isCacheUnderPressure()
     * evalutates to 'pressure'.
     */
    virtual void setCachePressureForTest(int pressure) = 0;

    /**
     * Prompts an immediate journal flush.
     */
    virtual void triggerJournalFlush() const = 0;

    /**
     * Initiates if needed and waits for a complete round of journal flushing to execute.
     *
     * Can throw ShutdownInProgress if the storage engine is being closed.
     */
    virtual void waitForJournalFlush(OperationContext* opCtx) const = 0;

    /**
     * Ensures interruption of the JournalFlusher if it is or will be acquiring a lock.
     *
     * TODO: this function will be moved above the Storage Engine layer along with the
     * JournalFlusher in SERVER-45847.
     */
    virtual void interruptJournalFlusherForReplStateChange() const = 0;

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
        // A list of IndexIdentifiers that must be rebuilt to completion.
        std::vector<IndexIdentifier> indexesToRebuild;

        // A map of unfinished two-phase indexes that must be restarted in the background, but
        // not to completion; they will wait for replicated commit or abort operations. This is a
        // mapping from index build UUID to index build.
        IndexBuilds indexBuildsToRestart;
    };

    /**
     * Drop abandoned idents. If successful, returns a ReconcileResult with indexes that need to be
     * rebuilt or builds that need to be restarted.
     * */
    virtual StatusWith<ReconcileResult> reconcileCatalogAndIdents(OperationContext* opCtx) = 0;

    /**
     * Returns the all_durable timestamp. All transactions with timestamps earlier than the
     * all_durable timestamp are committed. Only storage engines that support document level locking
     * must provide an implementation. Other storage engines may provide a no-op implementation.
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
     * Returns the oldest read timestamp in use by an open transaction. Storage engines that support
     * the 'snapshot' ReadConcern must provide an implementation. Other storage engines may provide
     * a no-op implementation.
     */
    virtual Timestamp getOldestOpenReadTimestamp() const = 0;

    /**
     * Returns the minimum possible Timestamp value in the oplog that replication may need for
     * recovery in the event of a crash.
     *
     * Returns boost::none when called on an ephemeral database.
     */
    virtual boost::optional<Timestamp> getOplogNeededForCrashRecovery() const = 0;

    /**
     * Returns the path to the directory which has the data files of database with `dbName`.
     */
    virtual std::string getFilesystemPathForDb(const std::string& dbName) const = 0;

    /**
     * Returns whethers the data files are compatible with the current code:
     *
     *   - Status::OK() if the data files are compatible with the current code.
     *
     *   - ErrorCodes::CanRepairToDowngrade if the data files are incompatible with the current
     *     code, but a --repair would make them compatible. For example, when rebuilding all indexes
     *     in the data files would resolve the incompatibility.
     *
     *   - ErrorCodes::MustUpgrade if the data files are incompatible with the current code and a
     *     newer version is required to start up.
     */
    virtual Status currentFilesCompatible(OperationContext* opCtx) const = 0;

    virtual int64_t sizeOnDiskForDb(OperationContext* opCtx, StringData dbName) = 0;

    virtual KVEngine* getEngine() = 0;
    virtual const KVEngine* getEngine() const = 0;
    virtual DurableCatalog* getCatalog() = 0;
    virtual const DurableCatalog* getCatalog() const = 0;
};

}  // namespace mongo
