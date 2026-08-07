// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/op_observer/op_observer.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

/**
 * No-op implementation of OpObserver interface.
 *
 * Suitable base class of OpObserver implementations that do not need to implement most of the
 * OpObserver interface.
 */
class [[MONGO_MOD_OPEN]] OpObserverNoop : public OpObserver {
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
                            const std::vector<IndexBuildInfo>& indexes,
                            const std::vector<boost::optional<BSONObj>>& multikey,
                            bool fromMigrate,
                            bool isTimeseries) override {}

    void onAbortIndexBuild(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const UUID& collUUID,
                           const UUID& indexBuildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           const Status& cause,
                           bool fromMigrate,
                           bool isTimeseries) override {}

    void onSetMultikeyMetadata(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const std::string& idxName,
                               const BSONObj& multikeyPaths) override {}

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
                           std::string_view ident,
                           int64_t key,
                           std::span<const char> value) override {}

    void onContainerInsert(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const char> key,
                           std::span<const char> value) override {}

    void onContainerInsert(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const std::span<const char>> keys,
                           std::span<const char> value) override {
        // Defer to the single-op in a loop, which may be overridden
        for (auto key : keys) {
            onContainerInsert(opCtx, ident, key, value);
        }
    }

    void onContainerInsert(OperationContext* opCtx,
                           std::string_view ident,
                           int64_t base,
                           std::span<const std::span<const char>> vals) override {
        // Early exit empty values
        if (vals.empty()) {
            return;
        }
        // Check for overflow, overflow::add returns true if overflow occurred
        int64_t maxKey;
        massert(13064500,
                "record id overflowed in batched insert",
                !overflow::add(base, static_cast<int64_t>(vals.size() - 1), &maxKey));

        // Defer to the single-op in a loop, which may be overridden
        for (size_t i = 0; i < vals.size(); ++i) {
            onContainerInsert(opCtx, ident, base + i, vals[i]);
        }
    }


    void onContainerUpdate(OperationContext* opCtx,
                           std::string_view ident,
                           int64_t key,
                           std::span<const char> value) override {}

    void onContainerUpdate(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const char> key,
                           std::span<const char> value) override {}

    void onContainerDelete(OperationContext* opCtx, std::string_view ident, int64_t key) override {}

    void onContainerDelete(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const char> key) override {}

    void onContainerDelete(OperationContext* opCtx,
                           std::string_view ident,
                           std::span<const std::span<const char>> keys) override {
        // Defer to the single-op in a loop, which may be overridden
        for (auto key : keys) {
            onContainerDelete(opCtx, ident, key);
        }
    }

    void onInternalOpMessage(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const boost::optional<UUID>& uuid,
                             const BSONObj& msgObj,
                             const boost::optional<BSONObj> o2MsgObj,
                             const boost::optional<repl::OpTime> preImageOpTime,
                             const boost::optional<repl::OpTime> postImageOpTime,
                             const boost::optional<repl::OpTime> prevWriteOpTimeInTransaction,
                             const boost::optional<OplogSlot> slot,
                             boost::optional<Date_t> wallClockTime = boost::none) override {}

    void onCreateCollection(
        OperationContext* opCtx,
        const NamespaceString& collectionName,
        const CollectionOptions& options,
        const BSONObj& idIndex,
        const OplogSlot& createOpTime,
        const boost::optional<CreateCollCatalogIdentifier>& createCollCatalogIdentifier,
        bool fromMigrate,
        bool isTimeseries,
        bool recordIdsReplicated) override {}

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

    void onPreparedTransactionCommit(OperationContext* opCtx,
                                     OplogSlot commitOplogEntryOpTime,
                                     Timestamp commitTimestamp) noexcept override {}

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

    void onTransactionPrepareNonPrimaryForChunkMigration(
        OperationContext* opCtx,
        const LogicalSessionId& lsid,
        boost::optional<const std::vector<repl::OplogEntry>&> statements,
        boost::optional<const repl::OpTime&> prepareOpTime) override {}

    void onTransactionAbort(OperationContext* opCtx,
                            boost::optional<OplogSlot> abortOplogEntryOpTime) override {}

    void onReplicationRollback(OperationContext* opCtx,
                               const RollbackObserverInfo& rbInfo) override {}

    void onMajorityCommitPointUpdate(ServiceContext* service,
                                     const repl::OpTime& newCommitPoint) override {}

    void onCreateDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override {}

    void onDropDatabaseMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override {}

    void onInvalidateAllCollectionMetadata(OperationContext* opCtx,
                                           const repl::OplogEntry& op) override {}

    void onInvalidateAllDatabaseMetadata(OperationContext* opCtx,
                                         const repl::OplogEntry& op) override {}

    void onInvalidateCollectionMetadata(OperationContext* opCtx,
                                        const repl::OplogEntry& op) override {}

    void onSetAllowChunkOperations(OperationContext* opCtx, const repl::OplogEntry& op) override {}

    void onUpdateCollectionMetadata(OperationContext* opCtx, const repl::OplogEntry& op) override {}

    void onTruncateRange(OperationContext* opCtx,
                         const CollectionPtr& coll,
                         const RecordId& minRecordId,
                         const RecordId& maxRecordId,
                         int64_t bytesDeleted,
                         int64_t docsDeleted,
                         repl::OpTime& opTime) override {}

    void onUpgradeDowngradeViewlessTimeseries(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const UUID& uuid,
                                              bool isUpgrade,
                                              bool skipViewCreation = false) override {}

    void onReplicatedIdentDrop(OperationContext* opCtx,
                               const std::string& ident,
                               repl::OpTime& opTime) override {}

    void onInitReplicatedFastCount(OperationContext* opCtx,
                                   const InitReplicatedFastCountO2& o2,
                                   repl::OpTime& opTime) override {}
};

}  // namespace mongo
