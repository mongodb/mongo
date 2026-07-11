// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class ClockSource;

/**
 * This class uses its own WiredTiger instance called "spill" WiredTiger instance. Journaling is
 * disabled for this WiredTiger instance. The cache size of this WiredTiger instance is also
 * configured to be very small.
 */
class SpillWiredTigerKVEngine final : public WiredTigerKVEngineBase {
public:
    SpillWiredTigerKVEngine(const std::string& canonicalName,
                            const std::string& path,
                            ClockSource* clockSource,
                            WiredTigerConfig wtConfig,
                            const SpillWiredTigerExtensions& wtExtensions);

    ~SpillWiredTigerKVEngine() override;

    std::unique_ptr<RecordStore> getInternalRecordStore(RecoveryUnit& ru,
                                                        std::string_view ident,
                                                        KeyFormat keyFormat) override;

    std::unique_ptr<RecordStore> makeInternalRecordStore(RecoveryUnit& ru,
                                                         std::string_view ident,
                                                         KeyFormat keyFormat) override;

    int64_t storageSize(RecoveryUnit& ru);

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override {
        return std::make_unique<WiredTigerRecoveryUnit>(_connection.get());
    }

    int64_t getIdentSize(RecoveryUnit&, std::string_view ident) override;

    bool hasIdent(RecoveryUnit&, std::string_view ident) const override;

    std::vector<std::string> getAllIdents(RecoveryUnit&) const override;

    Status dropIdent(RecoveryUnit& ru,
                     std::string_view ident,
                     bool identHasSizeInfo,
                     const StorageEngine::DropIdentCallback& onDrop,
                     boost::optional<uint64_t> schemaEpoch,
                     bool waitForLocks) override;

    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                std::string_view ident,
                                                const RecordStore::Options& options,
                                                boost::optional<UUID> uuid) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SortedDataInterface> getSortedDataInterface(OperationContext* opCtx,
                                                                RecoveryUnit& ru,
                                                                const NamespaceString& nss,
                                                                const UUID& uuid,
                                                                std::string_view ident,
                                                                const IndexConfig& config,
                                                                KeyFormat keyFormat) override {
        MONGO_UNREACHABLE;
    }

    Status createRecordStore(const rss::PersistenceProvider&,
                             RecoveryUnit& ru,
                             const NamespaceString& nss,
                             std::string_view ident,
                             const RecordStore::Options& options) override {
        MONGO_UNREACHABLE;
    }

    Status oplogDiskLocRegister(RecoveryUnit&,
                                RecordStore* oplogRecordStore,
                                const Timestamp& opTime,
                                bool orderedCommit) override {
        MONGO_UNREACHABLE;
    }

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 RecordStore* oplogRecordStore) const override {
        MONGO_UNREACHABLE;
    }

    bool waitUntilDurable(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

    bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx,
                                           bool stableCheckpoint) override {
        MONGO_UNREACHABLE;
    }

    bool underCachePressure(int concurrentOpOuts) override {
        MONGO_UNREACHABLE;
    }

    Status createSortedDataInterface(
        const rss::PersistenceProvider&,
        RecoveryUnit&,
        const NamespaceString& nss,
        const UUID& uuid,
        std::string_view ident,
        const IndexConfig& indexConfig,
        const boost::optional<mongo::BSONObj>& storageEngineOptions) override {
        MONGO_UNREACHABLE;
    }

    Status dropSortedDataInterface(RecoveryUnit&, std::string_view ident) override {
        MONGO_UNREACHABLE;
    }

    Status repairIdent(RecoveryUnit& ru, std::string_view ident) override {
        MONGO_UNREACHABLE;
    }

    void dropIdentForImport(Interruptible&, RecoveryUnit&, std::string_view ident) override {
        MONGO_UNREACHABLE;
    }

    Timestamp getBackupCheckpointTimestamp() override {
        MONGO_UNREACHABLE
    }

    void setJournalListener(JournalListener* jl) override {
        MONGO_UNREACHABLE;
    }

    Timestamp getAllDurableTimestamp() const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const override {
        MONGO_UNREACHABLE;
    }

    StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                             const std::string& requestingServiceName,
                                             Timestamp requestedTimestamp,
                                             bool roundUpIfTooOld) override {
        MONGO_UNREACHABLE
    }

    void unpinOldestTimestamp(const std::string& requestingServiceName) override {
        MONGO_UNREACHABLE
    }

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) override {
        MONGO_UNREACHABLE;
    }

    BSONObj setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                    std::string_view flagName,
                                    boost::optional<bool> flagValue) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                    std::string_view flagName) const override {
        MONGO_UNREACHABLE;
    }

    [[nodiscard]] BSONObj setStorageTierToStorageOptions(
        const BSONObj& storageEngineOptions, StorageTierLevelEnum value) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<StorageTierLevelEnum> getStorageTierFromStorageOptions(
        const BSONObj& storageEngineOptions) const override {
        MONGO_UNREACHABLE;
    }

    void dump() const override {
        MONGO_UNREACHABLE;
    }

    bool usesSchemaEpochs() const override {
        return false;
    }

    uint64_t getRawAllDurableTimestamp() const override {
        MONGO_UNREACHABLE;
    }

    void pinAllDurableTimestamp(uint64_t ts) override {
        MONGO_UNREACHABLE;
    }

    void unpinAllDurableTimestamp(uint64_t ts) override {
        MONGO_UNREACHABLE;
    }

    void publishIdent(WiredTigerRecoveryUnit& ru,
                      std::string_view ident,
                      uint64_t schemaEpoch) override {
        MONGO_UNREACHABLE;
    }

    void cleanShutdown(bool memLeakAllowed) override;

private:
    void _openWiredTiger(const std::string& path, const std::string& wtOpenConfig);
};

/**
 * Returns a WiredTigerKVEngineBase::WiredTigerConfig populated with config values provided at
 * startup for the Spill WiredTiger Engine.
 */
[[MONGO_MOD_USE_REPLACEMENT(jstest)]]
WiredTigerKVEngineBase::WiredTigerConfig getSpillWiredTigerConfigFromStartupOptions();

}  // namespace mongo
