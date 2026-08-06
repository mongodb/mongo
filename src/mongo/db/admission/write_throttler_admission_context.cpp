// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/write_throttler_admission_context.h"

#include "mongo/db/operation_context.h"

namespace mongo {

namespace {
const auto contextDecoration =
    OperationContext::declareDecoration<WriteThrottlerAdmissionContext>();
}  // namespace

WriteThrottlerAdmissionContext& WriteThrottlerAdmissionContext::get(OperationContext* opCtx) {
    return contextDecoration(opCtx);
}

OperationContext* WriteThrottlerAdmissionContext::getOperationContext() {
    return contextDecoration.owner(this);
}

}  // namespace mongo
