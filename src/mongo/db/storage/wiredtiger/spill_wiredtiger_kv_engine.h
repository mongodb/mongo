/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

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

    std::unique_ptr<RecordStore> getTemporaryRecordStore(RecoveryUnit& ru,
                                                         StringData ident,
                                                         KeyFormat keyFormat) override;

    std::unique_ptr<RecordStore> makeTemporaryRecordStore(RecoveryUnit& ru,
                                                          StringData ident,
                                                          KeyFormat keyFormat) override;

    int64_t storageSize(RecoveryUnit& ru);

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override {
        return std::make_unique<WiredTigerRecoveryUnit>(_connection.get());
    }

    int64_t getIdentSize(RecoveryUnit&, StringData ident) override;

    bool hasIdent(RecoveryUnit&, StringData ident) const override;

    std::vector<std::string> getAllIdents(RecoveryUnit&) const override;

    Status dropIdent(RecoveryUnit& ru,
                     StringData ident,
                     bool identHasSizeInfo,
                     const StorageEngine::DropIdentCallback& onDrop = nullptr) override;

    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const RecordStore::Options& options,
                                                boost::optional<UUID> uuid) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SortedDataInterface> getSortedDataInterface(OperationContext* opCtx,
                                                                RecoveryUnit& ru,
                                                                const NamespaceString& nss,
                                                                const UUID& uuid,
                                                                StringData ident,
                                                                const IndexConfig& config,
                                                                KeyFormat keyFormat) override {
        MONGO_UNREACHABLE;
    }

    Status createRecordStore(const rss::PersistenceProvider&,
                             const NamespaceString& nss,
                             StringData ident,
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

    bool underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) override {
        MONGO_UNREACHABLE;
    }

    Status createSortedDataInterface(
        const rss::PersistenceProvider&,
        RecoveryUnit&,
        const NamespaceString& nss,
        const UUID& uuid,
        StringData ident,
        const IndexConfig& indexConfig,
        const boost::optional<mongo::BSONObj>& storageEngineOptions) override {
        MONGO_UNREACHABLE;
    }

    Status dropSortedDataInterface(RecoveryUnit&, StringData ident) override {
        MONGO_UNREACHABLE;
    }

    Status repairIdent(RecoveryUnit& ru, StringData ident) override {
        MONGO_UNREACHABLE;
    }

    void dropIdentForImport(Interruptible&, RecoveryUnit&, StringData ident) override {
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
                                    StringData flagName,
                                    boost::optional<bool> flagValue) const override {
        MONGO_UNREACHABLE;
    }

    boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                    StringData flagName) const override {
        MONGO_UNREACHABLE;
    }

    void dump() const override {
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
WiredTigerKVEngineBase::WiredTigerConfig getSpillWiredTigerConfigFromStartupOptions();

}  // namespace mongo
