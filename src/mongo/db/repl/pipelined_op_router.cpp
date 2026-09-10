// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_op_router.h"

#include "mongo/util/assert_util.h"

namespace mongo::repl {
namespace {

using OpClass = PipelinedOpRouter::OpClass;

OpClass classifyOp(const OplogEntry& op) {
    if (!op.isCommand() && op.getNss().mustBeAppliedInOwnOplogBatch()) {
        return OpClass::kRequiresInline;
    }
    switch (op.getOpType()) {
        case OpTypeEnum::kInsert:
        case OpTypeEnum::kUpdate:
        case OpTypeEnum::kDelete:
        case OpTypeEnum::kContainerInsert:
        case OpTypeEnum::kContainerUpdate:
        case OpTypeEnum::kContainerDelete:
        case OpTypeEnum::kKeyMaterial:
        case OpTypeEnum::kCMKRotation:
            return OpClass::kPipelined;
        case OpTypeEnum::kNoop:
            // Record ids may be reused after the new-primary noop, so it must not be applied
            // concurrently with neighboring ops.
            return op.isNewPrimaryNoop() ? OpClass::kRequiresInline : OpClass::kPipelined;
        case OpTypeEnum::kCommand:
            // Only a non-transactional applyOps is pipelined; DDL and transaction entries are
            // applied inline.
            if (op.isTerminalApplyOps() && !op.applyOpsIsLinkedTransactionally()) {
                return OpClass::kPipelinedApplyOps;
            }
            return OpClass::kRequiresInline;
        // Op types outside of the categories above are conservatively applied inline.
        default:
            return OpClass::kRequiresInline;
    }
}

}  // namespace

PipelinedOpRouter::PipelinedOpRouter(size_t numWorkers) : _numWorkers(numWorkers) {
    invariant(numWorkers > 0);
}

PipelinedOpRouter::OpClass PipelinedOpRouter::classify(const OplogEntry& op) {
    auto opClass = classifyOp(op);
    if (opClass == OpClass::kRequiresInline) {
        _collPropertiesCache = CachedCollectionProperties();
    }
    return opClass;
}

size_t PipelinedOpRouter::selectWorker(OperationContext* opCtx, OplogEntry* op) {
    return OplogApplierUtils::getOplogEntryHash(opCtx, op, &_collPropertiesCache) % _numWorkers;
}

}  // namespace mongo::repl
