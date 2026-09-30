// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/instrumentation/replication_session_metrics.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/periodic_runner.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {
using otel::metrics::Counter;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using transport::SessionId;
}  // namespace

struct ReplicationSessionOtelMetricsState {
    std::unique_ptr<SessionMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getReplicationSessionOtelMetricsState =
    ServiceContext::declareDecoration<ReplicationSessionOtelMetricsState>();

MONGO_FAIL_POINT_DEFINE(failCollectReplicationSessionStats);

class SessionMetrics::Impl {
public:
    Impl()
        : _receiveQueueBytes(MetricsService::instance().createInt64Histogram(
              MetricNames::kReplicationSecondaryReceiveQueueBytes,
              "Number of bytes currently in the network queue across replication connections",
              MetricUnit::kBytes)),
          _receiveQueueSizes(MetricsService::instance().createInt64Histogram(
              MetricNames::kReplicationSecondaryReceiveQueueSize,
              "Size of network queue across replication connections",
              MetricUnit::kBytes)),
          _congestionWindowSizes(MetricsService::instance().createInt64Histogram(
              MetricNames::kReplicationSecondaryTcpCongestionWindowSize,
              "TCP Congestion Window size across replication connections",
              MetricUnit::kBytes)),
          _collectionFailures(MetricsService::instance().createInt64Counter(
              MetricNames::kReplicationSecondaryCollectErrors,
              "Number of times collecting replication session info fails",
              MetricUnit::kCount)) {}


    void update(const std::vector<transport::SessionStats>& windows) {
        for (const auto& info : windows) {
            _receiveQueueBytes.record(info.receiveBufferBytes);
            _congestionWindowSizes.record(info.congestionWindowSizeBytes);
            _receiveQueueSizes.record(info.receiveBufferSizeBytes);
        }
    }

    void recordCollectError() {
        _collectionFailures.add(1);
    }

private:
    otel::metrics::Histogram<int64_t>& _receiveQueueBytes;
    otel::metrics::Histogram<int64_t>& _receiveQueueSizes;
    otel::metrics::Histogram<int64_t>& _congestionWindowSizes;
    Counter<int64_t>& _collectionFailures;
};

SessionMetrics::SessionMetrics() : _impl(std::make_unique<Impl>()) {}
SessionMetrics::~SessionMetrics() = default;

void SessionMetrics::update(const std::vector<transport::SessionStats>& info) {
    _impl->update(info);
}

void SessionMetrics::recordCollectError() {
    _impl->recordCollectError();
}

std::optional<std::vector<transport::SessionStats>> collectReplicationSessionStats() {
    if (MONGO_unlikely(failCollectReplicationSessionStats.shouldFail())) {
        return {};
    }
    return getGlobalServiceContext()
        ->getTransportLayerManager()
        ->getDefaultEgressLayer()
        ->collectReplicationSessionStats();
}

void runReplicationSessionCollectionCycle(SessionMetrics& metrics) {
    auto windows = collectReplicationSessionStats();
    if (windows.has_value()) {
        metrics.update(windows.value());
    } else {
        metrics.recordCollectError();
    }
}


void installReplicationSessionOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getReplicationSessionOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<SessionMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "CongestionWindowOtelMetrics",
        [&state](Client*) { runReplicationSessionCollectionCycle(*state.metrics); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}
}  // namespace mongo

