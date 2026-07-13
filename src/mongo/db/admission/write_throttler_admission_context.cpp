// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/write_throttler_admission_context.h"

#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"

namespace mongo {

namespace {
const auto contextDecoration =
    OperationContext::declareDecoration<WriteThrottlerAdmissionContext>();
}  // namespace

WriteThrottlerAdmissionContext& WriteThrottlerAdmissionContext::get(OperationContext* opCtx) {
    return contextDecoration(opCtx);
}

void recordWriteThrottlerCostForReconciliation(OperationContext* opCtx, CurOp* curOp) {
    const auto& metrics = curOp->debug().getAdditiveMetrics();
    const int64_t docs = metrics.ninserted.value_or(0) + metrics.nModified.value_or(0) +
        metrics.ndeleted.value_or(0) + metrics.nUpserted.value_or(0);
    if (docs > 0) {
        WriteThrottlerAdmissionContext::get(opCtx).recordWriteCostForReconciliation(docs);
    }
}

}  // namespace mongo
