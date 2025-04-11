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

namespace mongo {

class ClockSource;

/**
 * WiredTigerKVEngineBase implementation for temporary tables. Temporary tables are tables that
 * don't need to be retained after a restart. This class uses its own WiredTiger instance called
 * "internal" WiredTiger instance. Journaling is disabled for this WiredTiger instance. The cache
 * size of this WiredTiger instance is also configured to be very small.
 */
class TemporaryWiredTigerKVEngine final : public WiredTigerKVEngineBase {
public:
    TemporaryWiredTigerKVEngine(const std::string& canonicalName,
                                const std::string& path,
                                ClockSource* clockSource,
                                WiredTigerConfig wtConfig);

    ~TemporaryWiredTigerKVEngine() override;

    std::unique_ptr<RecordStore> getTemporaryRecordStore(OperationContext* opCtx,
                                                         StringData ident,
                                                         KeyFormat keyFormat) override;

    std::unique_ptr<RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                          StringData ident,
                                                          KeyFormat keyFormat) override;

    bool isEphemeral() const override {
        return true;
    }

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override;

    bool supportsDirectoryPerDB() const override {
        return false;
    }

    int64_t getIdentSize(RecoveryUnit&, StringData ident) override {
        // TODO(SERVER-103258): Implement getIdentSize().
        MONGO_UNREACHABLE;
    }

    bool hasIdent(RecoveryUnit&, StringData ident) const override {
        // TODO(SERVER-103258): Implement hasIdent().
        MONGO_UNREACHABLE;
    }

    std::vector<std::string> getAllIdents(RecoveryUnit&) const override {
        // TODO(SERVER-103258): Implement getAllIdents().
        MONGO_UNREACHABLE;
    }

    Status dropIdent(RecoveryUnit* ru,
                     StringData ident,
                     bool identHasSizeInfo,
                     const StorageEngine::DropIdentCallback& onDrop = nullptr) override {
        // TODO(SERVER-103272): Implement dropIdent().
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const CollectionOptions& options) override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<SortedDataInterface> getSortedDataInterface(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& collOptions,
        StringData ident,
        const IndexConfig& config) override {
        MONGO_UNREACHABLE;
    }

    Status createRecordStore(const NamespaceString& nss,
                             StringData ident,
                             const CollectionOptions& options,
                             KeyFormat keyFormat = KeyFormat::Long) override {
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

    bool underCachePressure() override {
        MONGO_UNREACHABLE;
    }

    Status createSortedDataInterface(RecoveryUnit&,
                                     const NamespaceString& nss,
                                     const CollectionOptions& collOptions,
                                     StringData ident,
                                     const IndexConfig& config) override {
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

    void dump() const override {
        MONGO_UNREACHABLE;
    }

    void cleanShutdown() override;

private:
    void _openWiredTiger(const std::string& path, const std::string& wtOpenConfig);
};

}  // namespace mongo
