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
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/rss/persistence_provider.h"
#include "mongo/db/storage/compact_options.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

class JournalListener;
class OperationContext;
class RecoveryUnit;
class SnapshotManager;

class KVEngine {
public:
    using IdentKey = std::variant<std::span<const char>, int64_t>;

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
     * recovery and considers itself stable.
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

    virtual std::unique_ptr<RecoveryUnit> newRecoveryUnit() {
        MONGO_UNREACHABLE;
    }

    /**
     * Creates a RecordStore instance for the given ident. The ident must exist, and the behavior if
     * it does not is undefined.
     *
     * At most one non-point-in-time RecordStore should exist for a given ident at a time. Multiple
     * instances do not synchronize with each other, and writing via multiple instances or writing
     * via one instance while reading from another (non-PIT) instance may break in surprising ways.
     *
     * Instantiating RecordStores is expensive, and the returned pointer should be cached by the
     * caller if it will be used multiple times. In normal usage, this is managed by Collection and
     * RecordSTore instances should be obtained via that.
     */
    virtual std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        StringData ident,
                                                        const RecordStore::Options& options,
                                                        boost::optional<UUID> uuid) = 0;
    /**
     * Opens an existing ident as a temporary record store. Must be used for record stores created
     * with `makeTemporaryRecordStore`. Using `getRecordStore` would cause the record store to use
     * the same settings as a regular collection, and would differ in behaviour as when it was
     * originally created with `makeTemporaryRecordStore`.
     */
    virtual std::unique_ptr<RecordStore> getTemporaryRecordStore(RecoveryUnit& ru,
                                                                 StringData ident,
                                                                 KeyFormat keyFormat) = 0;

    virtual std::unique_ptr<SortedDataInterface> getSortedDataInterface(OperationContext* opCtx,
                                                                        RecoveryUnit& ru,
                                                                        const NamespaceString& nss,
                                                                        const UUID& uuid,
                                                                        StringData ident,
                                                                        const IndexConfig& config,
                                                                        KeyFormat keyFormat) = 0;

    /**
     * The create and drop methods on KVEngine are not transactional. Transactional semantics
     * are provided by the StorageEngine code that calls these. For example, drop will be
     * called if a create is rolled back. A higher-level drop operation will only propagate to a
     * drop call on the KVEngine once the WUOW commits. Therefore drops will never be rolled
     * back and it is safe to immediately reclaim storage.
     *
     * Creates a 'RecordStore' and generated from the provided 'options'.
     */
    virtual Status createRecordStore(const rss::PersistenceProvider&,
                                     const NamespaceString& nss,
                                     StringData ident,
                                     const RecordStore::Options& options) = 0;

    /**
     * RecordStores initially created with `makeTemporaryRecordStore` must be opened with
     * `getTemporaryRecordStore`.
     */
    virtual std::unique_ptr<RecordStore> makeTemporaryRecordStore(RecoveryUnit& ru,
                                                                  StringData ident,
                                                                  KeyFormat keyFormat) = 0;

    /**
     * Similar to createRecordStore but this imports from an existing table with the provided ident
     * instead of creating a new one.
     */
    virtual Status importRecordStore(StringData ident,
                                     const BSONObj& storageMetadata,
                                     bool panicOnCorruptWtMetadata,
                                     bool repair) {
        MONGO_UNREACHABLE;
    }

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
    virtual Status oplogDiskLocRegister(RecoveryUnit&,
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

    virtual bool underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) = 0;

    virtual Status createSortedDataInterface(
        const rss::PersistenceProvider&,
        RecoveryUnit&,
        const NamespaceString& nss,
        const UUID& uuid,
        StringData ident,
        const IndexConfig& indexConfig,
        const boost::optional<mongo::BSONObj>& storageEngineIndexOptions) = 0;

    /**
     * Similar to createSortedDataInterface but this imports from an existing table with the
     * provided ident instead of creating a new one.
     */
    virtual Status importSortedDataInterface(RecoveryUnit&,
                                             StringData ident,
                                             const BSONObj& storageMetadata,
                                             bool panicOnCorruptWtMetadata,
                                             bool repair) {
        MONGO_UNREACHABLE;
    }

    virtual Status dropSortedDataInterface(RecoveryUnit&, StringData ident) = 0;

    virtual int64_t getIdentSize(RecoveryUnit&, StringData ident) = 0;

    /**
     * Repair an ident. Returns Status::OK if repair did not modify data. Returns a non-fatal status
     * of DataModifiedByRepair if a repair operation succeeded, but may have modified data.
     */
    virtual Status repairIdent(RecoveryUnit& ru, StringData ident) = 0;

    /**
     * Removes any knowledge of the ident from the storage engines metadata which includes removing
     * the underlying files belonging to the ident. If the storage engine is unable to process the
     * removal immediately, we enqueue it to be removed at a later time. If a callback is specified,
     * it will be run upon the drop if this function returns an OK status.
     */
    virtual Status dropIdent(RecoveryUnit& ru,
                             StringData ident,
                             bool identHasSizeInfo,
                             const StorageEngine::DropIdentCallback& onDrop = nullptr) = 0;

    /**
     * Removes any knowledge of the ident from the storage engines metadata without removing the
     * underlying files belonging to the ident.
     */
    virtual void dropIdentForImport(Interruptible&, RecoveryUnit&, StringData ident) = 0;

    /**
     * Attempts to locate and recover a file that is "orphaned" from the storage engine's metadata,
     * but may still exist on disk if this is a durable storage engine. Returns DataModifiedByRepair
     * if a new record store was successfully created and Status::OK() if no data was modified.
     *
     * This may return an error if the storage engine attempted to recover the file and failed.
     *
     * This recovery process makes no guarantees about the integrity of data recovered or even that
     * it still exists when recovered.
     */
    virtual Status recoverOrphanedIdent(const rss::PersistenceProvider& provider,
                                        const NamespaceString& nss,
                                        StringData ident,
                                        const RecordStore::Options& recordStoreOptions) {
        auto status = createRecordStore(provider, nss, ident, recordStoreOptions);
        if (status.isOK()) {
            return {ErrorCodes::DataModifiedByRepair, "Orphan recovery created a new record store"};
        }
        return status;
    }

    virtual void alterIdentMetadata(RecoveryUnit&,
                                    StringData ident,
                                    const IndexConfig& config,
                                    bool isForceUpdateMetadata) {}

    /**
     * See StorageEngine::flushAllFiles for details
     */
    virtual void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) {}

    /**
     * See StorageEngine::beginBackup for details
     */
    virtual Status beginBackup() {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }

    /**
     * See StorageEngine::endBackup for details
     */
    virtual void endBackup() {
        MONGO_UNREACHABLE;
    }

    virtual Timestamp getBackupCheckpointTimestamp() = 0;

    virtual Status disableIncrementalBackup() {
        MONGO_UNREACHABLE;
    }

    virtual StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        const StorageEngine::BackupOptions& options) {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }

    virtual void endNonBlockingBackup() {
        MONGO_UNREACHABLE;
    }

    virtual StatusWith<std::deque<std::string>> extendBackupCursor() {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }

    /**
     * Returns whether the KVEngine supports checkpoints.
     */
    virtual bool supportsCheckpoints() const {
        return false;
    }

    virtual void checkpoint() {}

    virtual StorageEngine::CheckpointIteration getCheckpointIteration() const {
        return StorageEngine::CheckpointIteration{0};
    }

    virtual bool hasDataBeenCheckpointed(
        StorageEngine::CheckpointIteration checkpointIteration) const {
        MONGO_UNREACHABLE;
    }

    /**
     * Returns true if the KVEngine is ephemeral -- that is, it is NOT persistent and all data is
     * lost after shutdown. Otherwise, returns false.
     */
    virtual bool isEphemeral() const = 0;

    /**
     * This must not change over the lifetime of the engine.
     */
    virtual bool supportsCappedCollections() const {
        return true;
    }

    virtual bool hasIdent(RecoveryUnit&, StringData ident) const = 0;

    virtual std::vector<std::string> getAllIdents(RecoveryUnit&) const = 0;

    /**
     * This method will be called before there is a clean shutdown.  Storage engines should
     * override this method if they have clean-up to do that is different from unclean shutdown.
     * MongoDB will not call into the storage subsystem after calling this function.
     *
     * The storage engine is allowed to leak memory for faster shutdown, except when the process is
     * not exiting or when running tools to look for memory leaks.
     *
     * There is intentionally no uncleanShutdown().
     */
    virtual void cleanShutdown(bool memLeakAllowed) = 0;

    /**
     * Return the SnapshotManager for this KVEngine or NULL if not supported.
     *
     * Pointer remains owned by the StorageEngine, not the caller.
     */
    virtual SnapshotManager* getSnapshotManager() const {
        return nullptr;
    }

    /**
     * Sets a new JournalListener, which is used to alert the rest of the
     * system about journaled write progress.
     */
    virtual void setJournalListener(JournalListener* jl) = 0;

    /**
     * See `StorageEngine::setLastMaterializedLsn`
     */
    virtual void setLastMaterializedLsn(uint64_t lsn) {}

    /**
     * Configures the specified checkpoint as the starting point for recovery.
     */
    virtual void setRecoveryCheckpointMetadata(StringData checkpointMetadata) {}

    /**
     * Configures the storage engine as the leader, allowing it to flush checkpoints to remote
     * storage.
     */
    virtual void promoteToLeader() {}

    /**
     * See `StorageEngine::setStableTimestamp`
     */
    virtual void setStableTimestamp(Timestamp stableTimestamp, bool force) {}

    /**
     * See `StorageEngine::setInitialDataTimestamp`
     */
    virtual void setInitialDataTimestamp(Timestamp initialDataTimestamp) {}

    /**
     * See `StorageEngine::getInitialDataTimestamp`
     */
    virtual Timestamp getInitialDataTimestamp() const {
        return Timestamp();
    }

    /**
     * See `StorageEngine::setOldestTimestampFromStable`
     */
    virtual void setOldestTimestampFromStable() {}

    /**
     * See `StorageEngine::setOldestActiveTransactionTimestampCallback`
     */
    virtual void setOldestActiveTransactionTimestampCallback(
        StorageEngine::OldestActiveTransactionTimestampCallback callback) {};

    /**
     * See `StorageEngine::setOldestTimestamp`
     */
    virtual void setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {}

    /**
     * See `StorageEngine::supportsRecoverToStableTimestamp`
     */
    virtual bool supportsRecoverToStableTimestamp() const {
        return false;
    }

    /**
     * See `StorageEngine::supportsRecoveryTimestamp`
     */
    virtual bool supportsRecoveryTimestamp() const {
        return false;
    }

    /**
     * See `StorageEngine::recoverToStableTimestamp`
     */
    virtual StatusWith<Timestamp> recoverToStableTimestamp(Interruptible&) {
        fassertFailed(50664);
    }

    /**
     * See `StorageEngine::getRecoveryTimestamp`
     */
    virtual boost::optional<Timestamp> getRecoveryTimestamp() const {
        MONGO_UNREACHABLE;
    }

    /**
     * See `StorageEngine::getLastStableRecoveryTimestamp`
     */
    virtual boost::optional<Timestamp> getLastStableRecoveryTimestamp() const {
        MONGO_UNREACHABLE;
    }

    /**
     * See `StorageEngine::getAllDurableTimestamp`
     */
    virtual Timestamp getAllDurableTimestamp() const = 0;

    /**
     * See `StorageEngine::getOplogNeededForCrashRecovery`
     */
    virtual boost::optional<Timestamp> getOplogNeededForCrashRecovery() const = 0;

    /**
     * See `StorageEngine::getPinnedOplog`
     */
    virtual Timestamp getPinnedOplog() const {
        return Timestamp::min();
    }

    /**
     * See `StorageEngine::supportsReadConcernSnapshot`
     */
    virtual bool supportsReadConcernSnapshot() const {
        return false;
    }

    /**
     * Methods to access the storage engine's timestamps.
     */
    virtual Timestamp getCheckpointTimestamp() const {
        return Timestamp();
    }

    virtual Timestamp getOldestTimestamp() const {
        return Timestamp();
    }

    virtual Timestamp getStableTimestamp() const {
        return Timestamp();
    }

    virtual StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                                     const std::string& requestingServiceName,
                                                     Timestamp requestedTimestamp,
                                                     bool roundUpIfTooOld) = 0;

    virtual void unpinOldestTimestamp(const std::string& requestingServiceName) = 0;

    /**
     * See `StorageEngine::setPinnedOplogTimestamp`
     */
    virtual void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) = 0;

    /**
     * Inserts a key-value pair into the specified 'ident'. Must be called from within a
     * storage transaction. Duplicate keys (and by extension, updates) are not allowed.
     *
     * Returns OK on success, 'DuplicateKey' if the key already exists, or the error returned by
     * the underlying storage engine on other failures.
     */
    virtual Status insertIntoIdent(RecoveryUnit& ru,
                                   StringData ident,
                                   IdentKey key,
                                   std::span<const char> value) = 0;

    /**
     * Retrieves the value associated with 'key' from the specified 'ident'.
     *
     * Returns a 'UniqueBuffer' containing the value on success, 'KeyNotFound' if the key does not
     * exist, or the error returned by the underlying storage engine on other failures.
     */
    virtual StatusWith<UniqueBuffer> getFromIdent(RecoveryUnit& ru,
                                                  StringData ident,
                                                  IdentKey key) = 0;

    /**
     * Deletes the key from the specified 'ident'.
     *
     * Returns OK on success, 'KeyNotFound' if the key does not exist, or the error returned by the
     * underlying storage engine on other failures. Must be called from within a storage
     * transaction.
     */
    virtual Status deleteFromIdent(RecoveryUnit& ru, StringData ident, IdentKey key) = 0;

    /**
     * See `StorageEngine::dump`
     */
    virtual void dump() const = 0;

    /**
     * Instructs the KVEngine to (re-)configure any internal logging
     * capabilities. Returns Status::OK() if the logging subsystem was successfully
     * configured (or if defaulting to the virtual implementation).
     */
    virtual Status reconfigureLogging() {
        return Status::OK();
    }

    virtual StatusWith<BSONObj> getStorageMetadata(StringData ident) const {
        return BSONObj{};
    };

    /**
     * Returns the 'KeyFormat' tied to 'ident'.
     */
    virtual KeyFormat getKeyFormat(RecoveryUnit&, StringData ident) const {
        MONGO_UNREACHABLE;
    }

    /**
     * Returns the cache size in MB.
     */
    virtual size_t getCacheSizeMB() const {
        return 0;
    }

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
        const BSONObj& options) const {
        return options;
    }

    /**
     * See StorageEngine::autoCompact for details
     */
    virtual Status autoCompact(RecoveryUnit&, const AutoCompactOptions& options) {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support auto compact");
    }

    /**
     * The destructor will never be called from mongod, but may be called from tests.
     * Engines may assume that this will only be called in the case of clean shutdown, even if
     * cleanShutdown() hasn't been called.
     */
    virtual ~KVEngine() {}

    /**
     * Returns whether the kv-engine is currently trying to live-restore its database.
     */
    virtual bool hasOngoingLiveRestore() {
        return false;
    }
};
}  // namespace mongo
