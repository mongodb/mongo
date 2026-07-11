// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/execution_context.h"

#include "mongo/util/decorable.h"

namespace mongo {

namespace {

const OperationContext::Decoration<StorageExecutionContext> storageExecutionContextDecoration =
    OperationContext::declareDecoration<StorageExecutionContext>();

}  // namespace

StorageExecutionContext* StorageExecutionContext::get(OperationContext* opCtx) {
    return &storageExecutionContextDecoration(opCtx);
}

}  // namespace mongo
