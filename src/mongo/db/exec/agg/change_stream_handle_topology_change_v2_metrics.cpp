// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_handle_topology_change_v2_metrics.h"

#include "mongo/db/change_stream_metrics_util.h"

namespace mongo::exec::agg {
namespace {

using namespace change_stream;

/**
 * References to the OTEL instruments for the v2 topology-handler stage's state machine: one
 * entry-counter per state plus the currently-degraded gauge. The instruments are process-global
 * singletons owned by the OTEL MetricsService; this struct only borrows references to them.
 */
struct ChangeStreamTopologyChangeV2Metrics {
    otel::metrics::Counter<int64_t>& waiting;
    otel::metrics::Counter<int64_t>& fetchingInitialization;
    otel::metrics::Counter<int64_t>& fetchingGettingChangeEvent;
    otel::metrics::Counter<int64_t>& fetchingStartingChangeStreamSegment;
    otel::metrics::Counter<int64_t>& fetchingNormalGettingChangeEvent;
    otel::metrics::Counter<int64_t>& fetchingDegradedGettingChangeEvent;
    otel::metrics::Counter<int64_t>& downgrading;
    otel::metrics::Counter<int64_t>& final;
    otel::metrics::UpDownCounter<int64_t>& degraded;
};

const ChangeStreamTopologyChangeV2Metrics kChangeStreamTopologyChangeV2Metrics{
    .waiting = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTopologyStateWaiting,
        "changeStreams.shardTargeting.topologyState.waiting",
        "Number of entries into the v2 change stream topology-handler 'waiting' state."),
    .fetchingInitialization = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTopologyStateFetchingInitialization,
        "changeStreams.shardTargeting.topologyState.fetchingInitialization",
        "Number of entries into the v2 change stream topology-handler 'fetchingInitialization' "
        "state."),
    .fetchingGettingChangeEvent = createShardTargetingCounter(
        otel::metrics::MetricNames::
            kChangeStreamShardTargetingTopologyStateFetchingGettingChangeEvent,
        "changeStreams.shardTargeting.topologyState.fetchingGettingChangeEvent",
        "Number of entries into the v2 change stream topology-handler 'fetchingGettingChangeEvent' "
        "state."),
    .fetchingStartingChangeStreamSegment = createShardTargetingCounter(
        otel::metrics::MetricNames::
            kChangeStreamShardTargetingTopologyStateFetchingStartingChangeStreamSegment,
        "changeStreams.shardTargeting.topologyState.fetchingStartingChangeStreamSegment",
        "Number of entries into the v2 change stream topology-handler "
        "'fetchingStartingChangeStreamSegment' state."),
    .fetchingNormalGettingChangeEvent = createShardTargetingCounter(
        otel::metrics::MetricNames::
            kChangeStreamShardTargetingTopologyStateFetchingNormalGettingChangeEvent,
        "changeStreams.shardTargeting.topologyState.fetchingNormalGettingChangeEvent",
        "Number of entries into the v2 change stream topology-handler "
        "'fetchingNormalGettingChangeEvent' state."),
    .fetchingDegradedGettingChangeEvent = createShardTargetingCounter(
        otel::metrics::MetricNames::
            kChangeStreamShardTargetingTopologyStateFetchingDegradedGettingChangeEvent,
        "changeStreams.shardTargeting.topologyState.fetchingDegradedGettingChangeEvent",
        "Number of entries into the v2 change stream topology-handler "
        "'fetchingDegradedGettingChangeEvent' state."),
    .downgrading = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTopologyStateDowngrading,
        "changeStreams.shardTargeting.topologyState.downgrading",
        "Number of entries into the v2 change stream topology-handler 'downgrading' state."),
    .final = createShardTargetingCounter(
        otel::metrics::MetricNames::kChangeStreamShardTargetingTopologyStateFinal,
        "changeStreams.shardTargeting.topologyState.final",
        "Number of entries into the v2 change stream topology-handler 'final' state."),
    .degraded = createShardTargetingDegradedGauge(),
};

}  // namespace

ChangeStreamTopologyChangeV2MetricsRecorder getChangeStreamTopologyChangeV2MetricsRecorder() {
    return ChangeStreamTopologyChangeV2MetricsRecorder{
        .waiting = kChangeStreamTopologyChangeV2Metrics.waiting,
        .fetchingInitialization = kChangeStreamTopologyChangeV2Metrics.fetchingInitialization,
        .fetchingGettingChangeEvent =
            kChangeStreamTopologyChangeV2Metrics.fetchingGettingChangeEvent,
        .fetchingStartingChangeStreamSegment =
            kChangeStreamTopologyChangeV2Metrics.fetchingStartingChangeStreamSegment,
        .fetchingNormalGettingChangeEvent =
            kChangeStreamTopologyChangeV2Metrics.fetchingNormalGettingChangeEvent,
        .fetchingDegradedGettingChangeEvent =
            kChangeStreamTopologyChangeV2Metrics.fetchingDegradedGettingChangeEvent,
        .downgrading = kChangeStreamTopologyChangeV2Metrics.downgrading,
        .final = kChangeStreamTopologyChangeV2Metrics.final,
        .degraded = kChangeStreamTopologyChangeV2Metrics.degraded,
    };
}

}  // namespace mongo::exec::agg
