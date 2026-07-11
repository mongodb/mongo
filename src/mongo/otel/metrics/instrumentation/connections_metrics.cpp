// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/connections_metrics.h"

#include "mongo/db/service_context.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {
namespace {

using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

struct ConnectionsOtelMetricsState {
    std::unique_ptr<ConnectionsMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getConnectionsOtelMetricsState =
    ServiceContext::declareDecoration<ConnectionsOtelMetricsState>();

}  // namespace

class ConnectionsMetrics::Impl {
public:
    Impl()
        : _current(MetricsService::instance().createInt64Gauge(
              MetricNames::kConnectionsCurrent,
              "Current number of open incoming connections",
              MetricUnit::kConnections)),
          _available(MetricsService::instance().createInt64Gauge(
              MetricNames::kConnectionsAvailable,
              "Number of unused incoming connection slots available",
              MetricUnit::kConnections)),
          _totalCreated(MetricsService::instance().createInt64Counter(
              MetricNames::kConnectionsTotalCreated,
              "Cumulative count of all incoming connections created since server start",
              MetricUnit::kConnections)),
          _rejected(MetricsService::instance().createInt64Counter(
              MetricNames::kConnectionsRejected,
              "Cumulative count of incoming connections rejected since server start",
              MetricUnit::kConnections)),
          _active(MetricsService::instance().createInt64Gauge(
              MetricNames::kConnectionsActive,
              "Number of incoming connections currently executing an operation",
              MetricUnit::kConnections)) {}

    void update(const transport::ConnectionsStatsSnapshot& snap) {
        _current.set(snap.current);
        _available.set(snap.available);

        // Counters require deltas, not absolute values. Compute the increment since the
        // last poll and advance by that amount. The snapshot values are monotonically
        // increasing, so the delta is always non-negative under normal operation.
        _totalCreated.add(snap.totalCreated - _lastTotalCreated);
        _lastTotalCreated = snap.totalCreated;

        _rejected.add(snap.rejected - _lastRejected);
        _lastRejected = snap.rejected;

        _active.set(snap.active);
    }

private:
    Gauge<int64_t>& _current;
    Gauge<int64_t>& _available;
    Counter<int64_t>& _totalCreated;
    Counter<int64_t>& _rejected;
    Gauge<int64_t>& _active;

    int64_t _lastTotalCreated{0};
    int64_t _lastRejected{0};
};

ConnectionsMetrics::ConnectionsMetrics() : _impl(std::make_unique<Impl>()) {}

ConnectionsMetrics::~ConnectionsMetrics() = default;

void ConnectionsMetrics::update(const transport::ConnectionsStatsSnapshot& snap) {
    _impl->update(snap);
}

void installConnectionsOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getConnectionsOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<ConnectionsMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "ConnectionsOtelMetrics",
        [&state, svcCtx](Client*) {
            state.metrics->update(transport::collectConnectionsStatsSnapshot(svcCtx));
        },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
