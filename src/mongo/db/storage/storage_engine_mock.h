/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/storage/storage_engine.h"

namespace mongo {

/**
 * Mock storage engine.
 */
class StorageEngineMock : public StorageEngine {
public:
    RecoveryUnit* newRecoveryUnit() final {
        return nullptr;
    }
    std::vector<DatabaseName> listDatabases() const final {
        return {};
    }
    bool supportsCappedCollections() const final {
        return true;
    }
    bool supportsCheckpoints() const final {
        return false;
    }
    bool isEphemeral() const final {
        return true;
    }
    void loadCatalog(OperationContext* opCtx, LastShutdownState lastShutdownState) final {}
    void closeCatalog(OperationContext* opCtx) final {}
    Status closeDatabase(OperationContext* opCtx, const DatabaseName& dbName) final {
        return Status::OK();
    }
    Status dropDatabase(OperationContext* opCtx, const DatabaseName& dbName) final {
        return Status::OK();
    }
    void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) final {}
    Status beginBackup(OperationContext* opCtx) final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    void endBackup(OperationContext* opCtx) final {}
    Status disableIncrementalBackup(OperationContext* opCtx) {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        OperationContext* opCtx,
        boost::optional<Timestamp> checkpointTimestamp,
        const StorageEngine::BackupOptions& options) final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    void endNonBlockingBackup(OperationContext* opCtx) final {}
    StatusWith<std::deque<std::string>> extendBackupCursor(OperationContext* opCtx) final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    Status repairRecordStore(OperationContext* opCtx,
                             RecordId catalogId,
                             const NamespaceString& ns) final {
        return Status::OK();
    }
    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                   KeyFormat keyFormat) final {
        return {};
    }
    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreForResumableIndexBuild(
        OperationContext* opCtx, KeyFormat keyFormat) final {
        return {};
    }
    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreFromExistingIdent(
        OperationContext* opCtx, StringData ident) final {
        return {};
    }
    void cleanShutdown() final {}
    SnapshotManager* getSnapshotManager() const final {
        return nullptr;
    }
    void setJournalListener(JournalListener* jl) final {}
    bool supportsRecoverToStableTimestamp() const final {
        return false;
    }
    bool supportsRecoveryTimestamp() const final {
        return false;
    }
    bool supportsReadConcernSnapshot() const final {
        return false;
    }
    bool supportsReadConcernMajority() const final {
        return false;
    }
    bool supportsOplogStones() const final {
        return false;
    }
    bool supportsResumableIndexBuilds() const final {
        return false;
    }
    bool supportsPendingDrops() const final {
        return false;
    }
    void clearDropPendingState() final {}
    StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) final {
        fassertFailed(40547);
    }
    boost::optional<Timestamp> getRecoveryTimestamp() const final {
        MONGO_UNREACHABLE;
    }
    boost::optional<Timestamp> getLastStableRecoveryTimestamp() const final {
        MONGO_UNREACHABLE;
    }
    void setStableTimestamp(Timestamp stableTimestamp, bool force = false) final {}
    Timestamp getStableTimestamp() const override {
        return Timestamp();
    }
    void setInitialDataTimestamp(Timestamp timestamp) final {}
    Timestamp getInitialDataTimestamp() const override {
        return Timestamp();
    }
    void setOldestTimestampFromStable() final {}
    void setOldestTimestamp(Timestamp timestamp) final {}
    Timestamp getOldestTimestamp() const final {
        return {};
    };
    void setOldestActiveTransactionTimestampCallback(
        OldestActiveTransactionTimestampCallback callback) final {}

    StatusWith<StorageEngine::ReconcileResult> reconcileCatalogAndIdents(
        OperationContext* opCtx, LastShutdownState lastShutdownState) final {
        return ReconcileResult{};
    }
    Timestamp getAllDurableTimestamp() const final {
        return {};
    }
    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const final {
        return boost::none;
    }
    std::string getFilesystemPathForDb(const DatabaseName& dbName) const final {
        return "";
    }
    std::set<std::string> getDropPendingIdents() const final {
        return {};
    }
    void addDropPendingIdent(const Timestamp& dropTimestamp,
                             std::shared_ptr<Ident> ident,
                             DropIdentCallback&& onDrop) final {}
    void startTimestampMonitor() final {}

    void checkpoint() final {}

    int64_t sizeOnDiskForDb(OperationContext* opCtx, const DatabaseName& dbName) final {
        return 0;
    }
    bool isUsingDirectoryPerDb() const final {
        return false;
    }
    bool isUsingDirectoryForIndexes() const final {
        return false;
    }
    KVEngine* getEngine() final {
        return nullptr;
    }
    const KVEngine* getEngine() const final {
        return nullptr;
    }
    DurableCatalog* getCatalog() final {
        return nullptr;
    }
    const DurableCatalog* getCatalog() const final {
        return nullptr;
    }
    void addIndividuallyCheckpointedIndex(const std::string& ident) final {}
    void clearIndividuallyCheckpointedIndexes() final {}
    bool isInIndividuallyCheckpointedIndexes(const std::string& ident) const final {
        return false;
    }

    StatusWith<Timestamp> pinOldestTimestamp(OperationContext* opCtx,
                                             const std::string& requestingServiceName,
                                             Timestamp requestedTimestamp,
                                             bool roundUpIfTooOld) final {
        return Status::OK();
    }

    void unpinOldestTimestamp(const std::string& requestingServiceName) final {}

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) final {}

    void dump() const final {}
};

}  // namespace mongo
