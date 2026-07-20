// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * The state associated with tailable cursors.
 */
struct AwaitDataState {
    /**
     * The deadline for how long we wait on the tail of capped collection before returning IS_EOF.
     */
    Date_t waitForInsertsDeadline;

    /**
     * If true, when no results are available from a plan, then instead of returning immediately,
     * the system should wait up to the length of the operation deadline for data to be inserted
     * which causes results to become available.
     */
    bool shouldWaitForInserts;
};

extern const OperationContext::Decoration<AwaitDataState> awaitDataState;

/**
 * Sets the deadline by which the server-side awaitData wait on 'opCtx' must complete.
 */
[[MONGO_MOD_PUBLIC]] void setAwaitDataDeadline(OperationContext* opCtx, Date_t deadline);

}  // namespace mongo
