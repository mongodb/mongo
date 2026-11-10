// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/decorable.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <functional>

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

/**
 * True once the replication compression pipeline has a reply ready for this getMore (set only by
 * signalAwaitDataPipelineReady(), never cleared). While set, shouldWaitForInserts() returns false
 * and an in-progress awaitData wait ends. Lives here so both db/query and db/repl can use it.
 */
[[MONGO_MOD_PUBLIC]] bool awaitDataPipelineReadyToSend(OperationContext* opCtx);

/**
 * Marks 'opCtx' as one the pipeline may wake. Only marked operations install a waker in
 * waitForInserts(). Call on the operation's thread before it starts waiting.
 */
[[MONGO_MOD_PUBLIC]] void setAwaitDataPipelineWakeEligible(OperationContext* opCtx);
[[MONGO_MOD_PUBLIC]] bool awaitDataPipelineWakeEligible(OperationContext* opCtx);

/**
 * Installs, or clears with an empty function, the callable that ends this operation's awaitData
 * wait. It may run on another thread, so it must own whatever it uses. Registration and
 * signalAwaitDataPipelineReady() share a per-operation mutex: a signal sent before registration is
 * seen through the ready flag, one sent after calls the waker.
 */
[[MONGO_MOD_PUBLIC]] void setAwaitDataPipelineWaker(OperationContext* opCtx,
                                                    std::function<void()> waker);

/**
 * Sets the ready flag and calls the operation's waker, if any. Never advances a notifier version,
 * so other tailable readers keep waiting. 'opCtx' must stay alive for the call.
 */
[[MONGO_MOD_PUBLIC]] void signalAwaitDataPipelineReady(OperationContext* opCtx);

}  // namespace mongo
