// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/transaction/transaction_operations.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <vector>

namespace mongo {

/**
 * OpObserver that forwards operations during migration to the chunk cloner.
 *
 * Contains logic that used to reside in OpObserverImpl that was extracted in SERVER-36084
 * and placed in OpObserverShardingImpl as privately overridden functions separate from the
 * OpObserver methods.
 *
 * This class replaces OpObserverShardingImpl without deriving directly from OpObserverImpl
 * while implementing the standard OpObserver methods. OpObserverShardingImpl was removed in
 * SERVER-76271.
 *
 * See ShardServerOpObserver.
 */
class [[MONGO_MOD_PUBLIC]] MigrationChunkClonerSourceOpObserver final : public OpObserverNoop {
public:
    /**
     * Write operations do shard version checking, but if an update operation runs as part of a
     * 'readConcern:snapshot' transaction, the router could have used the metadata at the snapshot
     * time and yet set the latest shard version on the request. This is why the write can get
     * routed to a shard which no longer owns the chunk being written to. In such cases, throw a
     * MigrationConflict exception to indicate that the transaction needs to be rolled-back and
     * restarted.
     */
    static void assertIntersectingChunkHasNotMoved(OperationContext* opCtx,
                                                   const CollectionMetadata& metadata,
                                                   const BSONObj& shardKey,
                                                   const LogicalTime& atClusterTime);

    /**
     * Ensures that there is no movePrimary operation in progress for the given namespace.
     */
    static void assertNoMovePrimaryInProgress(OperationContext* opCtx, const NamespaceString& nss);


    /**
     * Returns whether a committed batched write should be logged for session migration. A batched
     * write qualifies only if it produced oplog entries and is a retryable write.
     */
    static bool shouldLogBatchedWriteForSessionMigration(
        const OpStateAccumulator* opAccumulator,
        WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
        bool hasTxnNumber,
        bool hasLogicalSessionId);

    void onInserts(OperationContext* opCtx,
                   const CollectionPtr& coll,
                   std::vector<InsertStatement>::const_iterator first,
                   std::vector<InsertStatement>::const_iterator last,
                   const std::vector<RecordId>& recordIds,
                   std::vector<bool> fromMigrate,
                   bool defaultFromMigrate,
                   OpStateAccumulator* opAccumulator = nullptr) final;

    void onUpdate(OperationContext* opCtx,
                  const OplogUpdateEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void onDelete(OperationContext* opCtx,
                  const CollectionPtr& coll,
                  StmtId stmtId,
                  const BSONObj& doc,
                  const DocumentKey& documentKey,
                  const OplogDeleteEntryArgs& args,
                  OpStateAccumulator* opAccumulator = nullptr) final;

    void onUnpreparedTransactionCommit(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        OpStateAccumulator* opAccumulator = nullptr) final;

    void postTransactionPrepare(OperationContext* opCtx,
                                const std::vector<OplogSlot>& reservedSlots,
                                const TransactionOperations& transactionOperations) final;

    void onTransactionPrepareNonPrimaryForChunkMigration(
        OperationContext* opCtx,
        const LogicalSessionId& lsid,
        boost::optional<const std::vector<repl::OplogEntry>&> statements,
        boost::optional<const repl::OpTime&> prepareOpTime) final;

    void onBatchedWriteCommit(OperationContext* opCtx,
                              WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                              OpStateAccumulator* opAccumulator = nullptr) final;
};

}  // namespace mongo
