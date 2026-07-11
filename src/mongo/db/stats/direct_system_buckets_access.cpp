// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/stats/direct_system_buckets_access.h"

namespace mongo {
namespace {
const auto directSystemBucketsAccess = OperationContext::declareDecoration<bool>();
}

bool& isDirectSystemBucketsAccess(OperationContext* opCtx) {
    return directSystemBucketsAccess(opCtx);
}

}  // namespace mongo
