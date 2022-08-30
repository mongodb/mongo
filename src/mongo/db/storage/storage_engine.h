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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/index_builds.h"
#include "mongo/db/database_name.h"
#include "mongo/db/resumable_index_builds_gen.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/util/functional.h"
#include "mongo/util/str.h"

namespace mongo {

class BackupBlock;
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
        virtual std::unique_ptr<StorageEngine> create(
            OperationContext* opCtx,
            const StorageGlobalParams& params,
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
    virtual void notifyStartupComplete() {}

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
    virtual std::vector<DatabaseName> listDatabases() const = 0;

    /**
     * Returns whether the storage engine supports capped collections.
     */
    virtual bool supportsCappedCollections() const = 0;

    /**
     * Returns whether the storage engine supports checkpoints.
     */
    virtual bool supportsCheckpoints() const = 0;

    /**
     * Returns true if the engine does not persist data to disk; false otherwise.
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
    virtual void loadCatalog(OperationContext* opCtx, LastShutdownState lastShutdownState) = 0;
    virtual void closeCatalog(OperationContext* opCtx) = 0;

    /**
     * Closes all file handles associated with a database.
     */
    virtual Status closeDatabase(OperationContext* opCtx, const DatabaseName& dbName) = 0;

    /**
     * Deletes all data and metadata for a database.
     */
    virtual Status dropDatabase(OperationContext* opCtx, const DatabaseName& dbName) = 0;

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
        explicit StreamingCursor(BackupOptions options) : options(options){};

        virtual ~StreamingCursor() = default;

        virtual StatusWith<std::deque<BackupBlock>> getNextBatch(OperationContext* opCtx,
                                                                 std::size_t batchSize) = 0;

    protected:
        BackupOptions options;
    };

    virtual StatusWith<std::unique_ptr<StreamingCursor>> beginNonBlockingBackup(
        OperationContext* opCtx,
        boost::optional<Timestamp> checkpointTimestamp,
        const BackupOptions& options) = 0;

    virtual void endNonBlockingBackup(OperationContext* opCtx) = 0;

    virtual StatusWith<std::deque<std::string>> extendBackupCursor(OperationContext* opCtx) = 0;

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
     * Creates a temporary RecordStore on the storage engine. On startup after an unclean shutdown,
     * the storage engine will drop any un-dropped temporary record stores.
     */
    virtual std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
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
        OperationContext* opCtx, StringData ident) = 0;

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

    virtual bool supportsResumableIndexBuilds() const = 0;

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
     * Adds 'ident' to a list of indexes/collections whose data will be dropped when:
     * - the dropTimestamp' is sufficiently old to ensure no future data accesses
     * - and no holders of 'ident' remain (the index/collection is no longer in active use)
     */
    virtual void addDropPendingIdent(const Timestamp& dropTimestamp,
                                     std::shared_ptr<Ident> ident,
                                     DropIdentCallback&& onDrop = nullptr) = 0;

    /**
     * Starts the timestamp monitor. This periodically drops idents queued by addDropPendingIdent,
     * and removes historical ident entries no longer necessary.
     */
    virtual void startTimestampMonitor() = 0;

    /**
     * Called when the checkpoint thread instructs the storage engine to take a checkpoint. The
     * underlying storage engine must take a checkpoint at this point.
     */
    virtual void checkpoint() = 0;

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
    virtual void setOldestTimestamp(Timestamp timestamp) = 0;

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
        // A list of IndexIdentifiers that must be rebuilt to completion.
        std::vector<IndexIdentifier> indexesToRebuild;

        // A map of unfinished two-phase indexes that must be restarted in the background, but
        // not to completion; they will wait for replicated commit or abort operations. This is a
        // mapping from index build UUID to index build.
        IndexBuilds indexBuildsToRestart;

        // List of index builds to be resumed. Each ResumeIndexInfo may contain multiple indexes to
        // resume as part of the same build.
        std::vector<ResumeIndexInfo> indexBuildsToResume;
    };

    /**
     * Drop abandoned idents. If successful, returns a ReconcileResult with indexes that need to be
     * rebuilt or builds that need to be restarted.
     *
     * Abandoned internal idents require special handling based on the context known only to the
     * caller. For example, on starting from a previous unclean shutdown, we would always drop all
     * unknown internal idents. If we started from a clean shutdown, the internal idents may contain
     * information for resuming index builds.
     */
    virtual StatusWith<ReconcileResult> reconcileCatalogAndIdents(
        OperationContext* opCtx, LastShutdownState lastShutdownState) = 0;

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
     * Returns the path to the directory which has the data files of database with `dbName`.
     */
    virtual std::string getFilesystemPathForDb(const DatabaseName& dbName) const = 0;

    virtual int64_t sizeOnDiskForDb(OperationContext* opCtx, const DatabaseName& dbName) = 0;

    virtual bool isUsingDirectoryPerDb() const = 0;

    virtual bool isUsingDirectoryForIndexes() const = 0;

    virtual KVEngine* getEngine() = 0;
    virtual const KVEngine* getEngine() const = 0;
    virtual DurableCatalog* getCatalog() = 0;
    virtual const DurableCatalog* getCatalog() const = 0;

    virtual void addIndividuallyCheckpointedIndex(const std::string& ident) = 0;

    virtual void clearIndividuallyCheckpointedIndexes() = 0;

    virtual bool isInIndividuallyCheckpointedIndexes(const std::string& ident) const = 0;

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
    virtual StatusWith<Timestamp> pinOldestTimestamp(OperationContext* opCtx,
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
     * Instructs the storage engine to dump its internal state.
     */
    virtual void dump() const = 0;
};

}  // namespace mongo
