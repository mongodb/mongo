// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/query_memory_metrics.h"

#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {
namespace {

using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

struct QueryMemoryOtelMetricsState {
    std::unique_ptr<QueryMemoryMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getQueryMemoryOtelMetricsState =
    ServiceContext::declareDecoration<QueryMemoryOtelMetricsState>();

// Current configured value of the internalQueryMaxMemoryUsageBytesPerOperation server parameter,
// the operation-wide cap on memory-tracked query stages. This is a single metric exposed on two
// surfaces: the `serverStatusOptions` below publishes it in serverStatus as
// `metrics.query.configuredMaxMemoryUsageBytesPerOperation`, and the same value is exported over
// OpenTelemetry.
//
// Registered at static-initialization time, matching the other memory-tracking metrics in
// memory_usage_tracker.cpp. This registration MUST happen before the MetricTreeSet is frozen during
// startup (see globalMetricTreeSet().freeze() in mongod_main.cpp): specifying `serverStatusOptions`
// adds an entry to the MetricTreeSet, and adding one after the freeze crashes the server. Doing
// this here rather than in the runtime install path (installQueryMemoryOtelMetrics, which runs
// post-freeze) guarantees the entry is added pre-freeze on the main thread.
Gauge<int64_t>& gConfiguredMaxMemoryUsageBytesPerOperation =
    MetricsService::instance().createInt64Gauge(
        MetricNames::kQueryConfiguredMaxMemoryUsageBytesPerOperation,
        "Current configured value of the internalQueryMaxMemoryUsageBytesPerOperation server "
        "parameter, the operation-wide cap on memory-tracked query stages",
        MetricUnit::kBytes,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "query.configuredMaxMemoryUsageBytesPerOperation",
                                 .role = ClusterRole::None}});

}  // namespace

class QueryMemoryMetrics::Impl {
public:
    void update(int64_t configuredMaxMemoryUsageBytesPerOperation) {
        gConfiguredMaxMemoryUsageBytesPerOperation.set(configuredMaxMemoryUsageBytesPerOperation);
    }
};

QueryMemoryMetrics::QueryMemoryMetrics() : _impl(std::make_unique<Impl>()) {}

QueryMemoryMetrics::~QueryMemoryMetrics() = default;

void QueryMemoryMetrics::update(int64_t configuredMaxMemoryUsageBytesPerOperation) {
    _impl->update(configuredMaxMemoryUsageBytesPerOperation);
}

void installQueryMemoryOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getQueryMemoryOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<QueryMemoryMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "QueryMemoryOtelMetrics",
        [&state](Client*) {
            state.metrics->update(internalQueryMaxMemoryUsageBytesPerOperation.loadRelaxed());
        },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
