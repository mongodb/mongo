// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/util/modules.h"

namespace mongo::exec::agg {

/**
 * References to the OTEL instruments for the v2 topology-handler stage's state machine: one
 * entry-counter per state plus the currently-degraded gauge. Held by the stage as a member; the
 * state-to-counter mapping lives with the stage itself (it owns the 'State' enum), so this struct
 * only exposes the raw counters plus the generic degraded-gauge helpers.
 */
struct ChangeStreamTopologyChangeV2MetricsRecorder {
    otel::metrics::Counter<int64_t>& waiting;
    otel::metrics::Counter<int64_t>& fetchingInitialization;
    otel::metrics::Counter<int64_t>& fetchingGettingChangeEvent;
    otel::metrics::Counter<int64_t>& fetchingStartingChangeStreamSegment;
    otel::metrics::Counter<int64_t>& fetchingNormalGettingChangeEvent;
    otel::metrics::Counter<int64_t>& fetchingDegradedGettingChangeEvent;
    otel::metrics::Counter<int64_t>& downgrading;
    otel::metrics::Counter<int64_t>& final;
    otel::metrics::UpDownCounter<int64_t>& degraded;

    /**
     * Increments 'changeStreams.shardTargeting.degraded' when a stream enters degraded mode.
     */
    void incrementDegraded() {
        degraded.add(1);
    }

    /**
     * Decrements 'changeStreams.shardTargeting.degraded' when a stream leaves degraded mode, or
     * when its stage is torn down while degraded.
     */
    void decrementDegraded() {
        degraded.add(-1);
    }
};

/**
 * Factory returning a recorder bound to the process-global OTEL instruments. Called once, at
 * stage-construction time.
 */
[[MONGO_MOD_PUBLIC]] ChangeStreamTopologyChangeV2MetricsRecorder
getChangeStreamTopologyChangeV2MetricsRecorder();

}  // namespace mongo::exec::agg
