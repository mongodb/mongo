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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/backup_block.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class JournalListener;

/**
 * The devnull storage engine is intended for unit and performance testing.
 */
class DevNullKVEngine : public KVEngine {
public:
    DevNullKVEngine();
    ~DevNullKVEngine() override;

    std::unique_ptr<RecoveryUnit> newRecoveryUnit() override;

    Status createRecordStore(const rss::PersistenceProvider&,
                             const NamespaceString& nss,
                             StringData ident,
                             const RecordStore::Options& options) override {
        return Status::OK();
    }

    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const RecordStore::Options& options,
                                                boost::optional<UUID> uuid) override;

    std::unique_ptr<RecordStore> getTemporaryRecordStore(RecoveryUnit& ru,
                                                         StringData ident,
                                                         KeyFormat keyFormat) override;

    std::unique_ptr<RecordStore> makeTemporaryRecordStore(RecoveryUnit& ru,
                                                          StringData ident,
                                                          KeyFormat keyFormat) override;

    Status createSortedDataInterface(
        const rss::PersistenceProvider&,
        RecoveryUnit&,
        const NamespaceString& nss,
        const UUID& uuid,
        StringData ident,
        const IndexConfig& indexConfig,
        const boost::optional<mongo::BSONObj>& storageEngineOptions) override {
        return Status::OK();
    }

    Status dropSortedDataInterface(RecoveryUnit&, StringData ident) override {
        return Status::OK();
    }

    std::unique_ptr<SortedDataInterface> getSortedDataInterface(OperationContext* opCtx,
                                                                RecoveryUnit& ru,
                                                                const NamespaceString& nss,
                                                                const UUID& uuid,
                                                                StringData ident,
                                                                const IndexConfig& config,
                                                                KeyFormat keyFormat) override;

    Status dropIdent(RecoveryUnit& ru,
                     StringData ident,
                     bool identHasSizeInfo,
                     const StorageEngine::DropIdentCallback& onDrop) override {
        return Status::OK();
    }

    void dropIdentForImport(Interruptible&, RecoveryUnit&, StringData ident) override {}

    bool isEphemeral() const override {
        return true;
    }

    int64_t getIdentSize(RecoveryUnit&, StringData ident) override {
        return 1;
    }

    Status repairIdent(RecoveryUnit&, StringData ident) override {
        return Status::OK();
    }

    bool hasIdent(RecoveryUnit&, StringData ident) const override {
        return true;
    }

    std::vector<std::string> getAllIdents(RecoveryUnit&) const override {
        return std::vector<std::string>();
    }

    void cleanShutdown(bool memLeakAllowed) override {}

    void setJournalListener(JournalListener* jl) override {}

    Timestamp getAllDurableTimestamp() const override {
        return Timestamp();
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const override {
        return boost::none;
    }

    Status beginBackup() override {
        return Status::OK();
    }

    void endBackup() override {}

    StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        const StorageEngine::BackupOptions& options) override;

    void endNonBlockingBackup() override {}

    Timestamp getBackupCheckpointTimestamp() override {
        return Timestamp(0, 0);
    }

    StatusWith<std::deque<std::string>> extendBackupCursor() override;

    boost::optional<Timestamp> getLastStableRecoveryTimestamp() const override {
        return boost::none;
    }

    Timestamp getOldestTimestamp() const override {
        return Timestamp();
    }

    boost::optional<Timestamp> getRecoveryTimestamp() const override {
        return boost::none;
    }

    void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) override {}

    void waitForAllEarlierOplogWritesToBeVisible(OperationContext* opCtx,
                                                 RecordStore* recordsStore) const override {}

    Status oplogDiskLocRegister(RecoveryUnit&,
                                RecordStore* oplogRecordStore,
                                const Timestamp& opTime,
                                bool orderedCommit) override {
        return Status::OK();
    }

    bool waitUntilDurable(OperationContext* opCtx) override {
        return true;
    }

    bool waitUntilUnjournaledWritesDurable(OperationContext* opCtx, bool) override {
        return true;
    }

    StatusWith<Timestamp> pinOldestTimestamp(RecoveryUnit&,
                                             const std::string& requestingServiceName,
                                             Timestamp requestedTimestamp,
                                             bool roundUpIfTooOld) override {
        return Timestamp(0, 0);
    }

    void unpinOldestTimestamp(const std::string& requestingServiceName) override {}

    bool underCachePressure(int concurrentWriteOuts, int concurrentReadOuts) override {
        return false;
    }

    BSONObj setFlagToStorageOptions(const BSONObj& storageEngineOptions,
                                    StringData flagName,
                                    boost::optional<bool> flagValue) const override {
        return storageEngineOptions;
    }

    boost::optional<bool> getFlagFromStorageOptions(const BSONObj& storageEngineOptions,
                                                    StringData flagName) const override {
        return boost::none;
    }

    void dump() const override {}

    Status insertIntoIdent(RecoveryUnit& ru,
                           StringData ident,
                           IdentKey key,
                           std::span<const char> value) override {
        return Status::OK();
    }

    StatusWith<UniqueBuffer> getFromIdent(RecoveryUnit& ru,
                                          StringData ident,
                                          IdentKey key) override {
        return Status::OK();
    }

    Status deleteFromIdent(RecoveryUnit& ru, StringData ident, IdentKey key) override {
        return Status::OK();
    }

    // This sets the results of the backup cursor for unit tests.
    void setBackupBlocks_forTest(std::deque<KVBackupBlock> newBackupBlocks) {
        _mockBackupBlocks = std::move(newBackupBlocks);
    }

private:
    std::shared_ptr<void> _catalogInfo;
    int _cachePressureForTest;
    std::deque<KVBackupBlock> _mockBackupBlocks;
    boost::filesystem::path _engineDbPath;
};
}  // namespace mongo
