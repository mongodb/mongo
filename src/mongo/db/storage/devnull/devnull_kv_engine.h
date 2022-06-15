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

#include "mongo/db/storage/backup_block.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit_noop.h"

namespace mongo {

class JournalListener;

/**
 * The devnull storage engine is intended for unit and performance testing.
 */
class DevNullKVEngine : public KVEngine {
public:
    DevNullKVEngine();

    virtual ~DevNullKVEngine() {}

    virtual RecoveryUnit* newRecoveryUnit() {
        return new RecoveryUnitNoop();
    }

    virtual Status createRecordStore(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     StringData ident,
                                     const CollectionOptions& options,
                                     KeyFormat keyFormat = KeyFormat::Long) {
        return Status::OK();
    }

    virtual std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        StringData ident,
                                                        const CollectionOptions& options);

    virtual std::unique_ptr<RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                                  StringData ident,
                                                                  KeyFormat keyFormat) override;

    virtual Status createSortedDataInterface(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& collOptions,
                                             StringData ident,
                                             const IndexDescriptor* desc) {
        return Status::OK();
    }

    virtual Status dropSortedDataInterface(OperationContext* opCtx, StringData ident) {
        return Status::OK();
    }

    virtual std::unique_ptr<SortedDataInterface> getSortedDataInterface(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& collOptions,
        StringData ident,
        const IndexDescriptor* desc);

    Status createColumnStore(OperationContext* opCtx,
                             const NamespaceString& ns,
                             const CollectionOptions& collOptions,
                             StringData ident,
                             const IndexDescriptor* desc) override {
        return Status(ErrorCodes::NotImplemented, "createColumnStore()");
    }

    std::unique_ptr<ColumnStore> getColumnStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const CollectionOptions& collOptions,
                                                StringData ident,
                                                const IndexDescriptor*) override {
        uasserted(ErrorCodes::NotImplemented, "getColumnStore()");
    }

    virtual Status dropIdent(RecoveryUnit* ru,
                             StringData ident,
                             StorageEngine::DropIdentCallback&& onDrop) {
        return Status::OK();
    }

    virtual void dropIdentForImport(OperationContext* opCtx, StringData ident) {}

    virtual bool supportsDirectoryPerDB() const {
        return false;
    }

    virtual bool isEphemeral() const {
        return true;
    }

    virtual int64_t getIdentSize(OperationContext* opCtx, StringData ident) {
        return 1;
    }

    virtual Status repairIdent(OperationContext* opCtx, StringData ident) {
        return Status::OK();
    }

    virtual bool hasIdent(OperationContext* opCtx, StringData ident) const {
        return true;
    }

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const {
        return std::vector<std::string>();
    }

    virtual void cleanShutdown(){};

    void setJournalListener(JournalListener* jl) final {}

    virtual Timestamp getAllDurableTimestamp() const override {
        return Timestamp();
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const final {
        return boost::none;
    }

    virtual Status beginBackup(OperationContext* opCtx) override {
        return Status::OK();
    }

    virtual void endBackup(OperationContext* opCtx) {}

    virtual StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        OperationContext* opCtx,
        boost::optional<Timestamp> checkpointTimestamp,
        const StorageEngine::BackupOptions& options) override;

    virtual void endNonBlockingBackup(OperationContext* opCtx) override {}

    virtual StatusWith<std::deque<std::string>> extendBackupCursor(
        OperationContext* opCtx) override;

    virtual boost::optional<Timestamp> getLastStableRecoveryTimestamp() const override {
        return boost::none;
    }

    virtual Timestamp getOldestTimestamp() const override {
        return Timestamp();
    }

    virtual boost::optional<Timestamp> getRecoveryTimestamp() const {
        return boost::none;
    }

    virtual void setPinnedOplogTimestamp(const Timestamp& pinnedTimestamp) {}

    void dump() const override {}

    // This sets the results of the backup cursor for unit tests.
    void setBackupBlocks_forTest(std::deque<BackupBlock> newBackupBlocks) {
        _mockBackupBlocks = std::move(newBackupBlocks);
    }

private:
    std::shared_ptr<void> _catalogInfo;
    int _cachePressureForTest;
    std::deque<BackupBlock> _mockBackupBlocks;
};
}  // namespace mongo
