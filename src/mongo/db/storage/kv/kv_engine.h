// kv_engine.h


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
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

class IndexDescriptor;
class JournalListener;
class OperationContext;
class RecoveryUnit;
class SortedDataInterface;
class SnapshotManager;

class KVEngine {
public:
    virtual RecoveryUnit* newRecoveryUnit() = 0;

    // ---------

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
                                                        StringData ns,
                                                        StringData ident,
                                                        const CollectionOptions& options) = 0;

    /**
     * Get a RecordStore that may share an underlying table with other RecordStores. 'prefix' is
     * guaranteed to be 'KVPrefix::kNotPrefixed' when 'groupCollections' is not enabled.
     *
     * @param prefix dictates the value keys for the RecordStore should be prefixed with to
     *        distinguish between RecordStores sharing an underlying table. A value of
     *        `KVPrefix::kNotPrefixed` guarantees the index is the sole resident of the table.
     */
    virtual std::unique_ptr<RecordStore> getGroupedRecordStore(OperationContext* opCtx,
                                                               StringData ns,
                                                               StringData ident,
                                                               const CollectionOptions& options,
                                                               KVPrefix prefix) {
        invariant(prefix == KVPrefix::kNotPrefixed);
        return getRecordStore(opCtx, ns, ident, options);
    }

    virtual SortedDataInterface* getSortedDataInterface(OperationContext* opCtx,
                                                        StringData ident,
                                                        const IndexDescriptor* desc) = 0;

    /**
     * Get a SortedDataInterface that may share an underlying table with other
     * SortedDataInterface. 'prefix' is guaranteed to be 'KVPrefix::kNotPrefixed' when
     * 'groupCollections' is not enabled.
     *
     * @param prefix dictates the value keys for the index should be prefixed with to distinguish
     *        between indexes sharing an underlying table. A value of `KVPrefix::kNotPrefixed`
     *        guarantees the index is the sole resident of the table.
     */
    virtual SortedDataInterface* getGroupedSortedDataInterface(OperationContext* opCtx,
                                                               StringData ident,
                                                               const IndexDescriptor* desc,
                                                               KVPrefix prefix) {
        invariant(prefix == KVPrefix::kNotPrefixed);
        return getSortedDataInterface(opCtx, ident, desc);
    }

    /**
     * The create and drop methods on KVEngine are not transactional. Transactional semantics
     * are provided by the KVStorageEngine code that calls these. For example, drop will be
     * called if a create is rolled back. A higher-level drop operation will only propagate to a
     * drop call on the KVEngine once the WUOW commits. Therefore drops will never be rolled
     * back and it is safe to immediately reclaim storage.
     */
    virtual Status createRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     StringData ident,
                                     const CollectionOptions& options) = 0;

    virtual std::unique_ptr<RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                  StringData ident) = 0;

    /**
     * Create a RecordStore that MongoDB considers eligible to share space in an underlying table
     * with other RecordStores. 'prefix' is guaranteed to be 'KVPrefix::kNotPrefixed' when
     * 'groupCollections' is not enabled.
     *
     * @param prefix signals whether the RecordStore may be shared by an underlying table. A
     *        prefix of `KVPrefix::kNotPrefixed` must remain isolated in its own table. Otherwise
     *        the storage engine implementation ultimately chooses which RecordStores share a
     *        table. Sharing RecordStores belonging to different databases within the same table
     *        is forbidden.
     */
    virtual Status createGroupedRecordStore(OperationContext* opCtx,
                                            StringData ns,
                                            StringData ident,
                                            const CollectionOptions& options,
                                            KVPrefix prefix) {
        invariant(prefix == KVPrefix::kNotPrefixed);
        return createRecordStore(opCtx, ns, ident, options);
    }

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             StringData ident,
                                             const IndexDescriptor* desc) = 0;

    /**
     * Create a SortedDataInterface that MongoDB considers eligible to share space in an
     * underlying table with other SortedDataInterfaces. 'prefix' is guaranteed to be
     * 'KVPrefix::kNotPrefixed' when 'groupCollections' is not enabled.
     *
     * @param prefix signals whether the SortedDataInterface (index) may be shared by an
     *        underlying table. A prefix of `KVPrefix::kNotPrefixed` must remain isolated in its own
     *        table. Otherwise the storage engine implementation ultimately chooses which indexes
     *        share a table. Sharing indexes belonging to different databases is forbidden.
     */
    virtual Status createGroupedSortedDataInterface(OperationContext* opCtx,
                                                    StringData ident,
                                                    const IndexDescriptor* desc,
                                                    KVPrefix prefix) {
        invariant(prefix == KVPrefix::kNotPrefixed);
        return createSortedDataInterface(opCtx, ident, desc);
    }

    virtual int64_t getIdentSize(OperationContext* opCtx, StringData ident) = 0;

    /**
     * Repair an ident. Returns Status::OK if repair did not modify data. Returns a non-fatal status
     * of DataModifiedByRepair if a repair operation succeeded, but may have modified data.
     */
    virtual Status repairIdent(OperationContext* opCtx, StringData ident) = 0;

    virtual Status dropIdent(OperationContext* opCtx, StringData ident) = 0;

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
                                        StringData ns,
                                        StringData ident,
                                        const CollectionOptions& options) {
        auto status = createRecordStore(opCtx, ns, ident, options);
        if (status.isOK()) {
            return {ErrorCodes::DataModifiedByRepair, "Orphan recovery created a new record store"};
        }
        return status;
    }


    virtual void alterIdentMetadata(OperationContext* opCtx,
                                    StringData ident,
                                    const IndexDescriptor* desc){};

    // optional
    virtual int flushAllFiles(OperationContext* opCtx, bool sync) {
        return 0;
    }

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

    virtual StatusWith<std::vector<std::string>> beginNonBlockingBackup(OperationContext* opCtx) {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }

    virtual void endNonBlockingBackup(OperationContext* opCtx) {
        MONGO_UNREACHABLE;
    }

    virtual bool isDurable() const = 0;

    /**
     * Returns true if the KVEngine is ephemeral -- that is, it is NOT persistent and all data is
     * lost after shutdown. Otherwise, returns false.
     */
    virtual bool isEphemeral() const = 0;

    /**
     * This must not change over the lifetime of the engine.
     */
    virtual bool supportsDocLocking() const = 0;

    /**
     * This must not change over the lifetime of the engine.
     */
    virtual bool supportsDBLocking() const {
        return true;
    }

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

    virtual Status okToRename(OperationContext* opCtx,
                              StringData fromNS,
                              StringData toNS,
                              StringData ident,
                              const RecordStore* originalRecordStore) const {
        return Status::OK();
    }

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
    virtual void setStableTimestamp(Timestamp stableTimestamp,
                                    boost::optional<Timestamp> maximumTruncationTimestamp) {}

    /**
     * See `StorageEngine::setInitialDataTimestamp`
     */
    virtual void setInitialDataTimestamp(Timestamp initialDataTimestamp) {}

    /**
     * See `StorageEngine::setOldestTimestampFromStable`
     */
    virtual void setOldestTimestampFromStable() {}

    /**
     * See `StorageEngine::setOldestTimestamp`
     */
    virtual void setOldestTimestamp(Timestamp newOldestTimestamp, bool force) {}

    /**
     * See `StorageEngine::isCacheUnderPressure()`
     */
    virtual bool isCacheUnderPressure(OperationContext* opCtx) const {
        return false;
    }

    /**
     * See 'StorageEngine::setCachePressureForTest()'
     */
    virtual void setCachePressureForTest(int pressure) {}

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
     * See `StorageEngine::getAllCommittedTimestamp`
     */
    virtual Timestamp getAllCommittedTimestamp() const = 0;

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
     * See `StorageEngine::replicationBatchIsComplete()`
     */
    virtual void replicationBatchIsComplete() const {};

    /**
     * The destructor will never be called from mongod, but may be called from tests.
     * Engines may assume that this will only be called in the case of clean shutdown, even if
     * cleanShutdown() hasn't been called.
     */
    virtual ~KVEngine() {}

protected:
    /**
     * The default capped size (in bytes) for capped collections, unless overridden.
     */
    const int64_t kDefaultCappedSizeBytes = 4096;
};
}
