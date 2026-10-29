// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/pipelined_applier_otel_metrics.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/server_status_options.h"
#include "mongo/util/static_immortal.h"

#include <boost/optional.hpp>

namespace mongo::repl {
namespace {

using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ServerStatusOptions;

// Bundles the OTel instruments and their serverStatus mirrors for the pipelined oplog applier.
// The bundle is initialized when pipelined-apply metrics are registered, only if the pipelined
// oplog applier feature flag is enabled. This keeps them from appearing on nodes that do not
// run the pipelined applier.
struct PipelinedApplyOtelMetricHandles {
    Counter<int64_t>& opsQueued = MetricsService::instance().createInt64Counter(
        MetricNames::kPipelinedApplyOpsQueued,
        "Number of ops queued using POA",
        MetricUnit::kOperations,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "repl.pipelinedApply.opsQueued"}});

    Counter<int64_t>& opsDispatched = MetricsService::instance().createInt64Counter(
        MetricNames::kPipelinedApplyOpsDispatched,
        "Number of ops dispatched using POA",
        MetricUnit::kOperations,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "repl.pipelinedApply.opsDispatched"}});

    Counter<int64_t>& opsAppliedByWorkers = MetricsService::instance().createInt64Counter(
        MetricNames::kPipelinedApplyOpsAppliedByWorkers,
        "Number of ops applied by workers using POA",
        MetricUnit::kOperations,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "repl.pipelinedApply.opsAppliedByWorkers"}});

    // TODO(SERVER-135412): Implement this metric once SERVER-134699 is merged.
    Counter<int64_t>& opsDrained = MetricsService::instance().createInt64Counter(
        MetricNames::kPipelinedApplyOpsDrained,
        "Number of ops drained using POA",
        MetricUnit::kOperations,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "repl.pipelinedApply.opsDrained"}});

    Gauge<int64_t>& queueDepthMin = MetricsService::instance().createInt64Gauge(
        MetricNames::kPipelinedApplyQueueDepthMin,
        "Depth of the currently-shallowest worker queue",
        MetricUnit::kCount,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "repl.pipelinedApply.queueDepthMin"}});

    Gauge<int64_t>& queueDepthMax = MetricsService::instance().createInt64Gauge(
        MetricNames::kPipelinedApplyQueueDepthMax,
        "Depth of the currently-deepest worker queue",
        MetricUnit::kCount,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "repl.pipelinedApply.queueDepthMax"}});

    // TODO(SERVER-135412): Implement this metric once SERVER-134697 is merged.
    Counter<int64_t>& lastAppliedAdvancements = MetricsService::instance().createInt64Counter(
        MetricNames::kPipelinedApplyLastAppliedAdvancements,
        "Times the advancer published a new lastApplied",
        MetricUnit::kCount,
        {.serverStatusOptions =
             ServerStatusOptions{.dottedPath = "repl.pipelinedApply.lastAppliedAdvancements"}});
};

// Exists only when the pipelined oplog applier feature flag is enabled.
inline boost::optional<PipelinedApplyOtelMetricHandles>& MetricHandles() {
    static StaticImmortal<boost::optional<PipelinedApplyOtelMetricHandles>> handles;
    return *handles;
}

MONGO_INITIALIZER_GENERAL(RegisterPipelinedApplyOtelMetrics,
                          ("EndStartupOptionHandling"),
                          ("default"))
(InitializerContext*) {
    if (gFeatureFlagPipelinedOplogApplier.isEnabled())
        PipelinedApplyOtelMetrics::initialize();
}

}  // namespace

void PipelinedApplyOtelMetrics::initialize() {
    if (!MetricHandles())
        MetricHandles().emplace();
}

void PipelinedApplyOtelMetrics::addOpsQueued(int64_t n) {
    if (!MetricHandles())
        return;
    MetricHandles()->opsQueued.add(n);
}

void PipelinedApplyOtelMetrics::addOpsDispatched(int64_t n) {
    if (!MetricHandles())
        return;
    MetricHandles()->opsDispatched.add(n);
}

void PipelinedApplyOtelMetrics::addOpsAppliedByWorkers(int64_t n) {
    if (!MetricHandles())
        return;
    MetricHandles()->opsAppliedByWorkers.add(n);
}

void PipelinedApplyOtelMetrics::addOpsDrained(int64_t n) {
    if (!MetricHandles())
        return;
    MetricHandles()->opsDrained.add(n);
}

void PipelinedApplyOtelMetrics::setQueueDepthMin(int64_t v) {
    if (!MetricHandles())
        return;
    MetricHandles()->queueDepthMin.set(v);
}

void PipelinedApplyOtelMetrics::setQueueDepthMax(int64_t v) {
    if (!MetricHandles())
        return;
    MetricHandles()->queueDepthMax.set(v);
}

void PipelinedApplyOtelMetrics::addLastAppliedAdvancements(int64_t n) {
    if (!MetricHandles())
        return;
    MetricHandles()->lastAppliedAdvancements.add(n);
}

}  // namespace mongo::repl
