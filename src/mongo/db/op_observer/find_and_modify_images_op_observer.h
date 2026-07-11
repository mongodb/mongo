// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * This OpObserver ensures that images for retryable findAndModify are written to
 * config.image_collection.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] FindAndModifyImagesOpObserver final : public OpObserverNoop {
    FindAndModifyImagesOpObserver(const FindAndModifyImagesOpObserver&) = delete;
    FindAndModifyImagesOpObserver& operator=(const FindAndModifyImagesOpObserver&) = delete;

public:
    FindAndModifyImagesOpObserver() = default;
    ~FindAndModifyImagesOpObserver() override = default;

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

    void onTransactionPrepare(
        OperationContext* opCtx,
        const std::vector<OplogSlot>& reservedSlots,
        const TransactionOperations& transactionOperations,
        const ApplyOpsOplogSlotAndOperationAssignment& applyOpsOperationAssignment,
        size_t numberOfPrePostImagesToWrite,
        Date_t wallClockTime,
        OpStateAccumulator* opAccumulator = nullptr) final;
};

}  // namespace mongo
