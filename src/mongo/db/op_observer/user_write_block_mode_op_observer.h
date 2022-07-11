/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/op_observer/op_observer.h"

namespace mongo {

/**
 * OpObserver for user write blocking. On write operations, checks whether the current global user
 * write blocking state allows the write, and uasserts if not.
 */
class UserWriteBlockModeOpObserver final : public OpObserver {
    UserWriteBlockModeOpObserver(const UserWriteBlockModeOpObserver&) = delete;
    UserWriteBlockModeOpObserver& operator=(const UserWriteBlockModeOpObserver&) = delete;

public:
    UserWriteBlockModeOpObserver() = default;
    ~UserWriteBlockModeOpObserver() = default;

    // Operations to check for allowed writes.

    // CUD operations
    void onInserts(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   bool fromMigrate) final;

    void onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) final;

    void onDelete(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const UUID& uuid,
                  StmtId stmtId,
                  const OplogDeleteEntryArgs& args) final;

    // DDL operations
    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       BSONObj indexDoc,
                       bool fromMigrate) final;

    // We need to check the startIndexBuild ops because onCreateIndex is only called for empty
    // collections.
    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           bool fromMigrate) final;

    void onStartIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) final;

    void onCreateCollection(OperationContext* opCtx,
                            const CollectionPtr& coll,
                            const NamespaceString& collectionName,
                            const CollectionOptions& options,
                            const BSONObj& idIndex,
                            const OplogSlot& createOpTime,
                            bool fromMigrate) final;

    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo) final;

    void onDropDatabase(OperationContext* opCtx, const DatabaseName& dbName) final;

    using OpObserver::onDropCollection;
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  CollectionDropType dropType) final;

    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const std::string& indexName,
                     const BSONObj& indexInfo) final;

    // onRenameCollection is only for renaming to a nonexistent target NS, so we need
    // preRenameCollection too.
    using OpObserver::preRenameCollection;
    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     const UUID& uuid,
                                     const boost::optional<UUID>& dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp) final;

    using OpObserver::onRenameCollection;
    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp) final;

    void onImportCollection(OperationContext* opCtx,
                            const UUID& importUUID,
                            const NamespaceString& nss,
                            long long numRecords,
                            long long dataSize,
                            const BSONObj& catalogEntry,
                            const BSONObj& storageMetadata,
                            bool isDryRun) final;

    // Note aboutToDelete is unchecked, but defined.
    void aboutToDelete(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const BSONObj& doc) final;

    // Noop operations (don't perform any check).

    // Index builds committing can be left unchecked since we kill any active index builds before
    // enabling write blocking. This means any index build which gets to the commit phase while
    // write blocking is active was started and hit the onStartIndexBuild hook with write blocking
    // active, and thus must be allowed under user write blocking.
    void onCommitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const UUID& collUUID,
                            const UUID& indexBuildUUID,
                            const std::vector<BSONObj>& indexes,
                            bool fromMigrate) final {}

    // At the moment we are leaving the onAbortIndexBuilds as unchecked. This is because they can be
    // called from both user and internal codepaths, and we don't want to risk throwing an assert
    // for the internal paths.
    void onAbortIndexBuildSinglePhase(OperationContext* opCtx, const NamespaceString& nss) final {}

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           const Status& cause,
                           bool fromMigrate) final {}

    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj,
                             const boost::optional<repl::OpTime> preImageOpTime,
                             const boost::optional<repl::OpTime> postImageOpTime,
                             const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                             const boost::optional<OplogSlot> slot) final {}

    // We don't need to check this and preRenameCollection (they are in the same WUOW).
    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              const UUID& uuid,
                              const boost::optional<UUID>& dropTargetUUID,
                              bool stayTemp) final {}

    // The transaction commit related hooks don't need to be checked, because all of the operations
    // inside the transaction are checked and they all execute in one WUOW.
    void onApplyOps(OperationContext* opCtx,
                    const DatabaseName& dbName,
                    const BSONObj& applyOpCmd) final {}

    void onEmptyCapped(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       const UUID& uuid) final {}

    void onUnpreparedTransactionCommit(OperationContext* opCtx,
                                       std::vector<repl::ReplOperation>* statements,
                                       size_t numberOfPrePostImagesToWrite) final {}

    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept final {}

    std::unique_ptr<ApplyOpsOplogSlotAndOperationAssignment> preTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        std::vector<repl::ReplOperation>* statements) final {
        return nullptr;
    }

    void onTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        std::vector<repl::ReplOperation>* statements,
        const ApplyOpsOplogSlotAndOperationAssignment* applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime) final {}

    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) final {}

    void onBatchedWriteStart(OperationContext* opCtx) final {}

    void onBatchedWriteCommit(OperationContext* opCtx) final {}

    void onBatchedWriteAbort(OperationContext* opCtx) final {}

    void onMajorityCommitPointUpdate(ServiceContext* service,
                                     const repl::OpTime& newCommitPoint) final {}

private:
    void _onReplicationRollback(OperationContext* opCtx, const RollbackObserverInfo& rbInfo) final;

    // uasserts that a write to the given namespace is allowed under the current user write blocking
    // setting.
    void _checkWriteAllowed(OperationContext* opCtx, const NamespaceString& nss);
};

}  // namespace mongo
