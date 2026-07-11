// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * This OpObserver ensures that pre-images are written to the config.system.preimages collection
 * if images are enabled on the collection at the time of a document update or delete operation.
 *
 * Delegates config.system.preimages inserts to ChangeStreamPreImagesCollectionManager.
 */
class [[MONGO_MOD_PUBLIC]] ChangeStreamPreImagesOpObserver final : public OpObserverNoop {
    ChangeStreamPreImagesOpObserver(const ChangeStreamPreImagesOpObserver&) = delete;
    ChangeStreamPreImagesOpObserver& operator=(const ChangeStreamPreImagesOpObserver&) = delete;

public:
    ChangeStreamPreImagesOpObserver() = default;
    ~ChangeStreamPreImagesOpObserver() override = default;

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

    void preTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        Date_t wallClockTime) final;

    void onBatchedWriteCommit(OperationContext* opCtx,
                              WriteUnitOfWork::OplogEntryGroupType oplogGroupingFormat,
                              OpStateAccumulator* opStateAccumulator) final;
};

}  // namespace mongo
