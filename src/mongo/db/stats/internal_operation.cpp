// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/internal_operation.h"

namespace mongo {
namespace {
const auto internalOperation = OperationContext::declareDecoration<bool>();
}

bool isInternalOperation(const OperationContext* opCtx) {
    return internalOperation(opCtx);
}

void markInternalOperation(OperationContext* opCtx) {
    internalOperation(opCtx) = true;
}

}  // namespace mongo
