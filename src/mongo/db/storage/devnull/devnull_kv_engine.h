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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/backup_block.h"
#include "mongo/db/storage/column_store.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class JournalListener;

/**
 * The devnull storage engine is intended for unit and performance testing.
 */
class DevNullKVEngine : public KVEngine {
public:
    DevNullKVEngine();
    ~DevNullKVEngine() override;

    RecoveryUnit* newRecoveryUnit() override;

    Status createRecordStore(OperationContext* opCtx,
                             const NamespaceString& nss,
                             StringData ident,
                             const CollectionOptions& options,
                             KeyFormat keyFormat = KeyFormat::Long) override {
        return Status::OK();
    }

    std::unique_ptr<RecordStore> getRecordStore(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                StringData ident,
                                                const CollectionOptions& options) override;

    std::unique_ptr<RecordStore> getTemporaryRecordStore(OperationContext* opCtx,
                                                         StringData ident,
                                                         KeyFormat keyFormat) override;

    std::unique_ptr<RecordStore> makeTemporaryRecordStore(OperationContext* opCtx,
                                                          StringData ident,
                                                          KeyFormat keyFormat) override;

    Status createSortedDataInterface(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const CollectionOptions& collOptions,
                                     StringData ident,
                                     const IndexDescriptor* desc) override {
        return Status::OK();
    }

    Status dropSortedDataInterface(OperationContext* opCtx, StringData ident) override {
        return Status::OK();
    }

    std::unique_ptr<SortedDataInterface> getSortedDataInterface(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& collOptions,
        StringData ident,
        const IndexDescriptor* desc) override;

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

    Status dropIdent(RecoveryUnit* ru,
                     StringData ident,
                     const StorageEngine::DropIdentCallback& onDrop) override {
        return Status::OK();
    }

    void dropIdentForImport(OperationContext* opCtx, StringData ident) override {}

    bool supportsDirectoryPerDB() const override {
        return false;
    }

    bool isEphemeral() const override {
        return true;
    }

    int64_t getIdentSize(OperationContext* opCtx, StringData ident) override {
        return 1;
    }

    Status repairIdent(OperationContext* opCtx, StringData ident) override {
        return Status::OK();
    }

    bool hasIdent(OperationContext* opCtx, StringData ident) const override {
        return true;
    }

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const override {
        return std::vector<std::string>();
    }

    void cleanShutdown() override {}

    void setJournalListener(JournalListener* jl) override {}

    Timestamp getAllDurableTimestamp() const override {
        return Timestamp();
    }

    boost::optional<Timestamp> getOplogNeededForCrashRecovery() const override {
        return boost::none;
    }

    Status beginBackup(OperationContext* opCtx) override {
        return Status::OK();
    }

    void endBackup(OperationContext* opCtx) override {}

    StatusWith<std::unique_ptr<StorageEngine::StreamingCursor>> beginNonBlockingBackup(
        OperationContext* opCtx, const StorageEngine::BackupOptions& options) override;

    void endNonBlockingBackup(OperationContext* opCtx) override {}

    StatusWith<std::deque<std::string>> extendBackupCursor(OperationContext* opCtx) override;

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

    void dump() const override {}

    // This sets the results of the backup cursor for unit tests.
    void setBackupBlocks_forTest(std::deque<BackupBlock> newBackupBlocks) {
        _mockBackupBlocks = std::move(newBackupBlocks);
    }

private:
    std::shared_ptr<void> _catalogInfo;
    int _cachePressureForTest;
    std::deque<BackupBlock> _mockBackupBlocks;
    boost::filesystem::path _engineDbPath;
};
}  // namespace mongo
