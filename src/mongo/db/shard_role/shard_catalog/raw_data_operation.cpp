// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"

namespace mongo {
namespace {
const auto rawDataOperation = OperationContext::declareDecoration<bool>();
}

bool& isRawDataOperation(OperationContext* opCtx) {
    return rawDataOperation(opCtx);
}

}  // namespace mongo
