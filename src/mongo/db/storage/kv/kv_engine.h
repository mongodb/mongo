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
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/import_options.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {

class IndexDescriptor;
class JournalListener;
class OperationContext;
class RecoveryUnit;
class SnapshotManager;

class KVEngine {
public:
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

    virtual RecoveryUnit* newRecoveryUnit() = 0;

    /**
     * Requesting multiple copies for the same ns/ident is a rules violation; Calling on a
     * non-created ident is invalid and may crash.
     *
     * Trying to access this record store in the future will retreive the pointer from the
     * collection object, and therefore this function can only be called once per namespace.
     *
     * @param ident Will be created if it does not already exist.
     */
    virtual std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        StringData ident,
                                                        const CollectionOptions& options) = 0;

    virtual std::unique_ptr<SortedDataInterface> getSortedDataInterface(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& collOptions,
        StringData ident,
        const IndexDescriptor* desc) = 0;
    virtual std::unique_ptr<ColumnStore> getColumnStore(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        const CollectionOptions& collOptions,
                                                        StringData ident,
                                                        const IndexDescriptor*) = 0;

    /**
     * The create and drop methods on KVEngine are not transactional. Transactional semantics
     * are provided by the StorageEngine code that calls these. For example, drop will be
     * called if a create is rolled back. A higher-level drop operation will only propagate to a
     * drop call on the KVEngine once the WUOW commits. Therefore drops will never be rolled
     * back and it is safe to immediately reclaim storage.
     */
    virtual Status createRecordStore(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     StringData ident,
                                     const CollectionOptions& options,
                                     KeyFormat keyFormat = KeyFormat::Long) = 0;

    virtual std::unique_ptr<RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                  StringData ident,
                                                                  KeyFormat keyFormat) = 0;

    /**
     * Similar to createRecordStore but this imports from an existing table with the provided ident
     * instead of creating a new one.
     */
    virtual Status importRecordStore(OperationContext* opCtx,
                                     StringData ident,
                                     const BSONObj& storageMetadata,
                                     const ImportOptions& importOptions) {
        MONGO_UNREACHABLE;
    }

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& collOptions,
                                             StringData ident,
                                             const IndexDescriptor* desc) = 0;
    virtual Status createColumnStore(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     const CollectionOptions& collOptions,
                                     StringData ident,
                                     const IndexDescriptor* desc) = 0;

    /**
     * Similar to createSortedDataInterface but this imports from an existing table with the
     * provided ident instead of creating a new one.
     */
    virtual Status importSortedDataInterface(OperationContext* opCtx,
                                             StringData ident,
                                             const BSONObj& storageMetadata,
                                             const ImportOptions& importOptions) {
        MONGO_UNREACHABLE;
    }

    virtual Status dropSortedDataInterface(OperationContext* opCtx, StringData ident) = 0;

    virtual int64_t getIdentSize(OperationContext* opCtx, StringData ident) = 0;

    /**
     * Repair an ident. Returns Status::OK if repair did not modify data. Returns a non-fatal status
     * of DataModifiedByRepair if a repair operation succeeded, but may have modified data.
     */
    virtual Status repairIdent(OperationContext* opCtx, StringData ident) = 0;

    /**
     * Removes any knowledge of the ident from the storage engines metadata which includes removing
     * the underlying files belonging to the ident. If the storage engine is unable to process the
     * removal immediately, we enqueue it to be removed at a later time. If a callback is specified,
     * it will be run upon the drop if this function returns an OK status.
     */
    virtual Status dropIdent(RecoveryUnit* ru,
                             StringData ident,
                             StorageEngine::DropIdentCallback&& onDrop = nullptr) = 0;

    /**
     * Removes any knowledge of the ident from the storage engines metadata without removing the
     * underlying files belonging to the ident.
     */
    virtual void dropIdentForImport(OperationContext* opCtx, StringData ident) = 0;

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
    virtual Status recoverOrphanedIdent(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        StringData ident,
                                        const CollectionOptions& options) {
        auto status = createRecordStore(opCtx, nss, ident, options);
        if (status.isOK()) {
            return {ErrorCodes::DataModifiedByRepair, "Orphan recovery created a new record store"};
        }
        return status;
    }

    virtual void alterIdentMetadata(OperationContext* opCtx,
                                    StringData ident,
                                    const IndexDescriptor* desc,
                                    bool isForceUpdateMetadata) {}

    /**
     * See StorageEngine::flushAllFiles for details
     */
    virtual void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) {}

    /**
     * See StorageEngine::beginBackup for details
     */
    virtual Status beginBackup(OperationContext* opCtx) {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }

    /**
     * See StorageEngine::endBackup for details
     */
    virtual void endBackup(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    virtual Status disableIncrementalBackup(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    virtual StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        OperationContext* opCtx,
        boost::optional<Timestamp> checkpointTimestamp,
        const StorageEngine::BackupOptions& options) {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }

    virtual void endNonBlockingBackup(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    virtual StatusWith<std::deque<std::string>> extendBackupCursor(OperationContext* opCtx) {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }

    virtual void addIndividuallyCheckpointedIndex(const std::string& ident) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "The current storage engine does not support checkpoints");
    }

    virtual void clearIndividuallyCheckpointedIndexes() {
        uasserted(ErrorCodes::CommandNotSupported,
                  "The current storage engine does not support checkpoints");
    }

    virtual bool isInIndividuallyCheckpointedIndexes(const std::string& ident) const {
        uasserted(ErrorCodes::CommandNotSupported,
                  "The current storage engine does not support checkpoints");
    }

    /**
     * Returns whether the KVEngine supports checkpoints.
     */
    virtual bool supportsCheckpoints() const {
        return false;
    }

    virtual void checkpoint() {}

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

    /**
     * Returns true if storage engine supports --directoryperdb.
     * See:
     *     http://docs.mongodb.org/manual/reference/program/mongod/#cmdoption--directoryperdb
     */
    virtual bool supportsDirectoryPerDB() const = 0;

    virtual bool hasIdent(OperationContext* opCtx, StringData ident) const = 0;

    virtual std::vector<std::string> getAllIdents(OperationContext* opCtx) const = 0;

    /**
     * This method will be called before there is a clean shutdown.  Storage engines should
     * override this method if they have clean-up to do that is different from unclean shutdown.
     * MongoDB will not call into the storage subsystem after calling this function.
     *
     * There is intentionally no uncleanShutdown().
     */
    virtual void cleanShutdown() = 0;

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
        StorageEngine::OldestActiveTransactionTimestampCallback callback){};

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
    virtual StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) {
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
     * See `StorageEngine::supportsReadConcernSnapshot`
     */
    virtual bool supportsReadConcernSnapshot() const {
        return false;
    }

    virtual bool supportsReadConcernMajority() const {
        return false;
    }

    /**
     * See `StorageEngine::supportsOplogStones`
     */
    virtual bool supportsOplogStones() const {
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

    virtual StatusWith<Timestamp> pinOldestTimestamp(OperationContext* opCtx,
                                                     const std::string& requestingServiceName,
                                                     Timestamp requestedTimestamp,
                                                     bool roundUpIfTooOld) {
        MONGO_UNREACHABLE;
    }

    virtual void unpinOldestTimestamp(const std::string& requestingServiceName) {
        MONGO_UNREACHABLE
    }

    /**
     * See `StorageEngine::setPinnedOplogTimestamp`
     */
    virtual void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) = 0;

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
     * The destructor will never be called from mongod, but may be called from tests.
     * Engines may assume that this will only be called in the case of clean shutdown, even if
     * cleanShutdown() hasn't been called.
     */
    virtual ~KVEngine() {}
};
}  // namespace mongo
