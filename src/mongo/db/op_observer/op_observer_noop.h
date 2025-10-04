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

#include "mongo/db/op_observer/op_observer.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUB mongo {

/**
 * No-op implementation of OpObserver interface.
 *
 * Suitable base class of OpObserver implementations that do not need to implement most of the
 * OpObserver interface.
 */
class MONGO_MOD_OPEN OpObserverNoop : public OpObserver {
public:
    NamespaceFilters getNamespaceFilters() const override {
        return {NamespaceFilter::kAll, NamespaceFilter::kAll};
    }

    void onCreateIndex(OperationContext* opCtx,
                       const NamespaceString& nss,
                       const UUID& uuid,
                       const IndexBuildInfo& indexBuildInfo,
                       bool fromMigrate,
                       bool isTimeseries) override {}

    void onStartIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           bool fromMigrate,
                           bool isTimeseries) override {}

    void onStartIndexBuildSinglePhase(OperationContext* opCtx,
                                      const NamespaceString& nss) override {}

    void onCommitIndexBuild(OperationContext* opCtx,
                            const NamespaceString& nss,
                            const UUID& collUUID,
                            const UUID& indexBuildUUID,
                            const std::vector<BSONObj>& indexes,
                            bool fromMigrate,
                            bool isTimeseries) override {}

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<BSONObj>& indexes,
                           const Status& cause,
                           bool fromMigrate,
                           bool isTimeseries) override {}

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator begin,
                   std::vector<InsertStatement>::const_iterator end,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) override {}

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override {}

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) override {}

    void onContainerInsert(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const UUID& collUUID,
                           StringData ident,
                           int64_t key,
                           std::span<const char> value) override {}

    void onContainerInsert(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const UUID& collUUID,
                           StringData ident,
                           std::span<const char> key,
                           std::span<const char> value) override {}

    void onContainerDelete(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const UUID& collUUID,
                           StringData ident,
                           int64_t key) override {}

    void onContainerDelete(OperationContext* opCtx,
                           const NamespaceString& ns,
                           const UUID& collUUID,
                           StringData ident,
                           std::span<const char> key) override {}

    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj,
                             const boost::optional<repl::OpTime> preImageOpTime,
                             const boost::optional<repl::OpTime> postImageOpTime,
                             const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                             const boost::optional<OplogSlot> slot) override {}

    void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
        bool fromMigrate,
        bool isTimeseries) override {}

    void onCollMod(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const UUID& uuid,
                   const BSONObj& collModCmd,
                   const CollectionOptions& oldCollOptions,
                   boost::optional<IndexCollModInfo> indexInfo,
                   bool isTimeseries) override {}

    void onDropDatabase(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        bool markFromMigrate) override {}

    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isTimeseries) override {
        return {};
    }

    void onDropIndex(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const UUID& uuid,
                     const std::string& indexName,
                     const BSONObj& idxDescriptor,
                     bool isTimeseries) override {}

    void onRenameCollection(OperationContext* opCtx,
                            const NamespaceString& fromCollection,
                            const NamespaceString& toCollection,
                            const UUID& uuid,
                            const boost::optional<UUID>& dropTargetUUID,
                            std::uint64_t numRecords,
                            bool stayTemp,
                            bool markFromMigrate,
                            bool isTimeseries) override {}

    void onImportCollection(OperationContext* opCtx,
                            const UUID& importUUID,
                            const NamespaceString& nss,
                            long long numRecords,
                            long long dataSize,
                            const BSONObj& catalogEntry,
                            const BSONObj& storageMetadata,
                            bool isDryRun,
                            bool isTimeseries) override {}

    repl::OpTime preRenameCollection(OperationContext* opCtx,
                                     const NamespaceString& fromCollection,
                                     const NamespaceString& toCollection,
                                     const UUID& uuid,
                                     const boost::optional<UUID>& dropTargetUUID,
                                     std::uint64_t numRecords,
                                     bool stayTemp,
                                     bool markFromMigrate,
                                     bool isTimeseries) override {
        return {};
    }

    void postRenameCollection(OperationContext* opCtx,
                              const NamespaceString& fromCollection,
                              const NamespaceString& toCollection,
                              const UUID& uuid,
                              const boost::optional<UUID>& dropTargetUUID,
                              bool stayTemp) override {}

    void onTransactionStart(OperationContext* opCtx) override {}

    void onUnpreparedTransactionCommit(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        OpStateAccumulator* opAccumulator = nullptr) override {}

    void onBatchedWriteStart(OperationContext* opCtx) override {}

    void onBatchedWriteCommit(OperationContext* opCtx,
                              WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                              OpStateAccumulator* opStateAccumulator = nullptr) override {}

    void onBatchedWriteAbort(OperationContext* opCtx) override {}

    void onPreparedTransactionCommit(
        OperationContext* opCtx,
        OplogSlot commitOplogEntryOpTime,
        Timestamp commitTimestamp,
        const std::vector<repl::ReplOperation>& statements) noexcept override {}

    void preTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        Date_t wallClockTime) override {}

    void onTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        OpStateAccumulator* opAccumulator = nullptr) override {}

    void postTransactionPrepare(OperationContext* opCtx,
                                const std::vector<OplogSlot>& reservedSlots,
                                const TransactionOperations& transactionOperations) override {}

    void onTransactionPrepareNonPrimary(OperationContext* opCtx,
                                        const LogicalSessionId& lsid,
                                        const std::vector<repl::OplogEntry>& statements,
                                        const repl::OpTime& prepareOpTime) override {}

    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) override {}

    void onReplicationRollback(OperationContext* opCtx,
                               const RollbackObserverInfo& rbInfo) override {}

    void onMajorityCommitPointUpdate(ServiceContext* service,
                                     const repl::OpTime& newCommitPoint) override {}

    void onCreateDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override {}

    void onDropDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override {}

    void onTruncateRange(OperationContext* opCtx,
                         const CollectionPtr& coll,
                         const RecordId& minRecordId,
                         const RecordId& maxRecordId,
                         int64_t bytesDeleted,
                         int64_t docsDeleted,
                         repl::OpTime& opTime) override {}
};

}  // namespace MONGO_MOD_PUB mongo
