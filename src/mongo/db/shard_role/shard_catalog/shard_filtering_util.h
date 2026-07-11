// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

namespace mongo {

namespace refresh_util {

/**
 * This method implements a best-effort attempt to wait for a refresh to complete, throwing an
 * exception if it fails or times out.
 *
 * All waits on refreshes in a shard should go through this code path, because it also accounts for
 * transactions and locking.
 */
[[MONGO_MOD_PRIVATE]] void waitForRefreshToComplete(OperationContext* opCtx,
                                                    const SharedSemiFuture<void>& refresh);

/**
 * This method implements a best-effort attempt to wait for the critical section to complete
 * before returning to the router at the previous step in order to prevent it from busy spinning
 * while the critical section is in progress.
 *
 * All waits for migration critical section should go through this code path, because it also
 * accounts for transactions and locking.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] Status waitForCriticalSectionToComplete(
    OperationContext* opCtx, const CriticalSectionSignal& critSecSignal) noexcept;

}  // namespace refresh_util

}  // namespace mongo
