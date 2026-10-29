// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::repl {

/**
 * Owns the OTel metrics for the pipelined oplog applier and provides static functions to update
 * them. The metric names for these instruments are declared in `otel::metrics::MetricNames` with
 * a `kPipelinedApply*` prefix.
 *
 * The instruments are created only when the pipelined oplog applier feature flag is enabled, so
 * these metrics do not appear on nodes that do not run the pipelined applier. Every update helper
 * is a no-op if `initialize()` has not been called.
 */
class [[MONGO_MOD_PUBLIC]] PipelinedApplyOtelMetrics {
    PipelinedApplyOtelMetrics() = delete;

public:
    /**
     * Creates the pipelined-apply OTel instruments and registers their serverStatus mirrors.
     * Must be called before the global MetricTreeSet is frozen and on the main thread.
     * Subsequent calls have no effect.
     */
    static void initialize();

    /** Records that `n` ops were enqueued onto a worker's queue. */
    static void addOpsQueued(int64_t n);

    /** Records that `n` ops were dispatched (routed) onto worker queues. */
    static void addOpsDispatched(int64_t n);

    /** Records that `n` ops were successfully applied by workers. */
    static void addOpsAppliedByWorkers(int64_t n);

    // TODO(SERVER-135412): Implement this metric once SERVER-134699 is merged.
    /** Records that `n` ops were drained. */
    static void addOpsDrained(int64_t n);

    /** Sets the currently-shallowest worker queue depth. */
    static void setQueueDepthMin(int64_t v);

    /** Sets the currently-deepest worker queue depth. */
    static void setQueueDepthMax(int64_t v);

    // TODO(SERVER-135412): Implement this metric once SERVER-134697 is merged.
    /** Records that the advancer published a new lastApplied `n` more times. */
    static void addLastAppliedAdvancements(int64_t n);
};

}  // namespace mongo::repl
