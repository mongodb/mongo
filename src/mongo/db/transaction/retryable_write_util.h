// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo::retryable_write_util {

/**
 * Returns true if we are running retryable write or retryable internal multi-document transaction.
 */
[[MONGO_MOD_PUBLIC]] bool isRetryableWrite(OperationContext* opCtx);

}  // namespace mongo::retryable_write_util
