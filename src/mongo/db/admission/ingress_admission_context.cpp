// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/ingress_admission_context.h"

#include "mongo/db/operation_context.h"

namespace mongo {

namespace {
const auto contextDecoration = OperationContext::declareDecoration<IngressAdmissionContext>();
}  // namespace

IngressAdmissionContext& IngressAdmissionContext::get(OperationContext* opCtx) {
    auto& context = contextDecoration(opCtx);
    context._setOperationContext(opCtx);
    return context;
}

}  // namespace mongo
