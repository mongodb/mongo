/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
