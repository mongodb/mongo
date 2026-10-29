// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/multi_plan_admission_context.h"

#include "mongo/db/operation_context.h"

namespace mongo {

namespace {
const auto contextDecoration = OperationContext::declareDecoration<MultiPlanAdmissionContext>();
}  // namespace

MultiPlanAdmissionContext& MultiPlanAdmissionContext::get(OperationContext* opCtx) {
    return contextDecoration(opCtx);
}
}  // namespace mongo
