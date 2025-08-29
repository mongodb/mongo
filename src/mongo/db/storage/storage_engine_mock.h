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
    std::unique_ptr<RecoveryUnit> newRecoveryUnit() final {
        return nullptr;
    }
    bool supportsCappedCollections() const final {
        return true;
    }
    bool supportsCheckpoints() const final {
        return false;
    }
    bool isEphemeral() const override {
        return true;
    }
    void loadMDBCatalog(OperationContext* opCtx, LastShutdownState lastShutdownState) final {}
    void closeMDBCatalog(OperationContext* opCtx) final {}
    void flushAllFiles(OperationContext* opCtx, bool callerHoldsReadLock) final {}
    Status beginBackup() final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    void endBackup() final {}
    Status disableIncrementalBackup() override {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    Timestamp getBackupCheckpointTimestamp() override {
        return Timestamp(0, 0);
    }
    StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        const StorageEngine::BackupOptions& options) final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    void endNonBlockingBackup() final {}
    StatusWith<std::deque<std::string>> extendBackupCursor() final {
        return Status(ErrorCodes::CommandNotSupported,
                      "The current storage engine doesn't support backup mode");
    }
    Status repairRecordStore(OperationContext* opCtx,
                             RecordId catalogId,
                             const NamespaceString& ns) final {
        return Status::OK();
    }
    std::unique_ptr<SpillTable> makeSpillTable(OperationContext* opCtx,
                                               KeyFormat keyFormat,
                                               int64_t thresholdBytes) final {

        return {};
    }

    void dropSpillTable(RecoveryUnit& ru, StringData ident) final {};

    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                   StringData ident,
                                                                   KeyFormat keyFormat) final {
        return {};
    }
    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreForResumableIndexBuild(
        OperationContext* opCtx, KeyFormat keyFormat) final {
        return {};
    }
    std::unique_ptr<TemporaryRecordStore> makeTemporaryRecordStoreFromExistingIdent(
        OperationContext* opCtx, StringData ident, KeyFormat keyFormat) final {
        return {};
    }
    void cleanShutdown(ServiceContext* svcCtx, bool memLeakAllowed) final {}
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
    void clearDropPendingState(OperationContext* opCtx) final {}
    Status immediatelyCompletePendingDrop(OperationContext* opCtx, StringData ident) final {
        return Status::OK();
    }
    StatusWith<Timestamp> recoverToStableTimestamp(OperationContext* opCtx) final {
        fassertFailed(40547);
    }
    boost::optional<Timestamp> getRecoveryTimestamp() const final {
        MONGO_UNREACHABLE;
    }
    boost::optional<Timestamp> getLastStableRecoveryTimestamp() const final {
        MONGO_UNREACHABLE;
    }

    void setLastMaterializedLsn(uint64_t lsn) final {}

    void setRecoveryCheckpointMetadata(StringData checkpointMetadata) final {}

    void promoteToLeader() final {}

    void setStableTimestamp(Timestamp stableTimestamp, bool force = false) override {}
    Timestamp getStableTimestamp() const override {
        return Timestamp();
    }
    void setInitialDataTimestamp(Timestamp timestamp) final {}
    Timestamp getInitialDataTimestamp() const override {
        return Timestamp();
    }
    void setOldestTimestampFromStable() final {}
    void setOldestTimestamp(Timestamp timestamp, bool force) final {}
    Timestamp getOldestTimestamp() const final {
        return {};
    };
    void setOldestActiveTransactionTimestampCallback(
        OldestActiveTransactionTimestampCallback callback) final {}

    Timestamp getAllDurableTimestamp() const final {
        return {};
    }
    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const final {
        return boost::none;
    }
    Timestamp getPinnedOplog() const final {
        return Timestamp();
    }
    std::string getFilesystemPathForDb(const DatabaseName& dbName) const final {
        return "";
    }
    std::set<std::string> getDropPendingIdents() const final {
        return {};
    }
    size_t getNumDropPendingIdents() const final {
        return 0;
    }
    void addDropPendingIdent(
        const std::variant<Timestamp, StorageEngine::CheckpointIteration>& dropTime,
        std::shared_ptr<Ident> ident,
        DropIdentCallback&& onDrop) final {}
    std::shared_ptr<Ident> markIdentInUse(StringData ident) final {
        return nullptr;
    }
    void startTimestampMonitor(
        std::initializer_list<TimestampMonitor::TimestampListener*> listeners) final {}
    void stopTimestampMonitor() final {}
    void restartTimestampMonitor() final {}

    void checkpoint() final {}

    StorageEngine::CheckpointIteration getCheckpointIteration() const final {
        return StorageEngine::CheckpointIteration{0};
    }

    bool hasDataBeenCheckpointed(
        StorageEngine::CheckpointIteration checkpointIteration) const override {
        return false;
    }

    std::string generateNewCollectionIdent(const DatabaseName& dbName) const final {
        return "";
    }
    std::string generateNewIndexIdent(const DatabaseName& dbName) const final {
        return "";
    }
    bool storesFilesInDbPath() const final {
        return false;
    }
    int64_t getIdentSize(RecoveryUnit& ru, StringData ident) const final {
        return 0;
    }
    KVEngine* getEngine() final {
        return nullptr;
    }
    const KVEngine* getEngine() const final {
        return nullptr;
    }
    KVEngine* getSpillEngine() override {
        return nullptr;
    }
    const KVEngine* getSpillEngine() const override {
        return nullptr;
    }
    MDBCatalog* getMDBCatalog() final {
        return nullptr;
    }
    const MDBCatalog* getMDBCatalog() const final {
        return nullptr;
    }
    std::set<std::string> getDropPendingIdents() final {
        return {};
    }

    StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                             const std::string& requestingServiceName,
                                             Timestamp requestedTimestamp,
                                             bool roundUpIfTooOld) final {
        return Status::OK();
    }

    void unpinOldestTimestamp(const std::string& requestingServiceName) final {}

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) final {}

    Status oplogDiskLocRegister(OperationContext* opCtx,
                                RecordStore* oplogRecordStore,
                                const Timestamp& opTime,
                                bool orderedCommit) final {
        return Status::OK();
    }

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 RecordStore* oplogRecordStore) const override {}

    bool waitUntilDurable(OperationContext* opCtx) override {
        return true;
    }

    bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                           bool stableCheckpoint) override {
        return true;
    }

    BSONObj setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                    StringData flagName,
                                    boost::optional<bool> flagValue) const final {
        return storageEngineOptions;
    }

    boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                    StringData flagName) const final {
        return boost::none;
    }

    BSONObj getSanitizedStorageOptionsForSecondaryReplication(const BSONObj& options) const final {
        return options;
    }

    void dump() const final {}

    Status autoCompact(RecoveryUnit&, const AutoCompactOptions& options) final {
        return Status::OK();
    }

    bool hasOngoingLiveRestore() final {
        return false;
    }
};

}  // namespace mongo
