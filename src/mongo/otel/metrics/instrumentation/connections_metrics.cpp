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

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace {

using otel::metrics::AttributeDefinition;
using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

std::vector<std::string> makeBackpressureVersionLabels() {
    std::vector<std::string> labels;
    labels.reserve(static_cast<std::size_t>(kMaxExplicitBackpressureVersion) + 2);
    for (int32_t version = 0; version <= kMaxExplicitBackpressureVersion; ++version) {
        labels.push_back(backpressureVersionLabel(version));
    }
    labels.push_back(backpressureVersionLabel(kOtherBackpressureVersion));
    return labels;
}

AttributeDefinition<std::string_view> makeBackpressureVersionAttr(
    const std::vector<std::string>& labels) {
    std::vector<std::string_view> views;
    views.reserve(labels.size());
    for (const auto& label : labels) {
        views.emplace_back(label);
    }
    return {.name = "version", .values = std::move(views)};
}

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
        : _backpressureVersionLabels(makeBackpressureVersionLabels()),
          _backpressureVersionAttr(makeBackpressureVersionAttr(_backpressureVersionLabels)),
          _current(MetricsService::instance().createInt64Gauge(
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
              MetricUnit::kConnections)),
          _backpressureCurrent(MetricsService::instance().createInt64Gauge<std::string_view>(
              MetricNames::kConnectionsBackpressureVersionsCurrent,
              "Current number of open ingress connections by client backpressure protocol version",
              MetricUnit::kConnections,
              _backpressureVersionAttr)),
          _backpressureTotalCreated(MetricsService::instance().createInt64Counter<std::string_view>(
              MetricNames::kConnectionsBackpressureVersionsTotal,
              "Cumulative count of ingress connections by client backpressure protocol version "
              "since server start",
              MetricUnit::kConnections,
              _backpressureVersionAttr)) {}

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

    void updateBackpressureVersionMetrics(const BackpressureConnectionMetrics& metrics) {
        for (int32_t version = 0; version <= kMaxExplicitBackpressureVersion; ++version) {
            _updateBackpressureVersion(_backpressureVersionLabels[version],
                                       metrics.count(version),
                                       metrics.totalCreated(version),
                                       _lastBackpressureTotalCreated[version]);
        }
        _updateBackpressureVersion(_backpressureVersionLabels.back(),
                                   metrics.count(kOtherBackpressureVersion),
                                   metrics.totalCreated(kOtherBackpressureVersion),
                                   _lastBackpressureOtherTotalCreated);
    }

private:
    void _updateBackpressureVersion(std::string_view versionLabel,
                                    int64_t current,
                                    int64_t totalCreated,
                                    int64_t& lastTotalCreated) {
        _backpressureCurrent.set(current, {versionLabel});
        const auto delta = totalCreated - lastTotalCreated;
        if (delta > 0) {
            _backpressureTotalCreated.add(delta, {versionLabel});
            // Only advance the baseline on a positive delta so a transient dip does not
            // cause already-emitted creations to be counted again on recovery.
            lastTotalCreated = totalCreated;
        }
    }

    // Owned labels must outlive AttributeDefinition string_views and gauge/counter updates.
    std::vector<std::string> _backpressureVersionLabels;
    AttributeDefinition<std::string_view> _backpressureVersionAttr;

    Gauge<int64_t>& _current;
    Gauge<int64_t>& _available;
    Counter<int64_t>& _totalCreated;
    Counter<int64_t>& _rejected;
    Gauge<int64_t>& _active;
    Gauge<int64_t, std::string_view>& _backpressureCurrent;
    Counter<int64_t, std::string_view>& _backpressureTotalCreated;

    int64_t _lastTotalCreated{0};
    int64_t _lastRejected{0};
    std::array<int64_t, kMaxExplicitBackpressureVersion + 1> _lastBackpressureTotalCreated{};
    int64_t _lastBackpressureOtherTotalCreated{0};
};

ConnectionsMetrics::ConnectionsMetrics() : _impl(std::make_unique<Impl>()) {}

ConnectionsMetrics::~ConnectionsMetrics() = default;

void ConnectionsMetrics::update(const transport::ConnectionsStatsSnapshot& snap) {
    _impl->update(snap);
}

void ConnectionsMetrics::updateBackpressureVersionMetrics(
    const BackpressureConnectionMetrics& metrics) {
    _impl->updateBackpressureVersionMetrics(metrics);
}

void installConnectionsOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getConnectionsOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<ConnectionsMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "ConnectionsOtelMetrics",
        [&state, svcCtx](Client*) {
            state.metrics->update(transport::collectConnectionsStatsSnapshot(svcCtx));
            state.metrics->updateBackpressureVersionMetrics(
                BackpressureConnectionMetrics::collect(svcCtx));
        },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
