// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/instrumentation/replication_session_metrics.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_histogram.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/net/connection_purpose.h"
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
        : _sendQueueBytes(MetricsService::instance().createInt64Histogram<int64_t>(
              MetricNames::kReplicationPrimarySendQueueBytes,
              "Number of bytes currently in the send network queue for replication connections "
              "sending oplog entries",
              MetricUnit::kBytes,
              replicationIdAttribute())),
          _sendQueueSizes(MetricsService::instance().createInt64Histogram<int64_t>(
              MetricNames::kReplicationPrimarySendQueueSize,
              "Size of the send network queue across replication connections sending oplog entries",
              MetricUnit::kBytes,
              replicationIdAttribute())),
          _receiveQueueBytes(MetricsService::instance().createInt64Histogram(
              MetricNames::kReplicationSecondaryReceiveQueueBytes,
              "Number of bytes currently in the receive network queue across for replication "
              "connections receiving oplog entries ",
              MetricUnit::kBytes)),
          _receiveQueueSizes(MetricsService::instance().createInt64Histogram(
              MetricNames::kReplicationSecondaryReceiveQueueSize,
              "Size of the receive network queue across replication connections receiving oplog "
              "entrie",
              MetricUnit::kBytes)),
          _ingressCongestionWindowSizes(MetricsService::instance().createInt64Histogram<int64_t>(
              MetricNames::kReplicationPrimaryTcpCongestionWindowSize,
              "TCP Congestion Window sizes for replication connections sending oplog entries",
              MetricUnit::kBytes,
              replicationIdAttribute())),
          _egressCongestionWindowSizes(MetricsService::instance().createInt64Histogram(
              MetricNames::kReplicationSecondaryTcpCongestionWindowSize,
              "TCP Congestion Window size for replication connections receiving oplog entries",
              MetricUnit::kBytes)),
          _collectionFailures(MetricsService::instance().createInt64Counter(
              MetricNames::kReplicationCollectErrors,
              "Number of times collecting replication session info fails",
              MetricUnit::kCount)) {}


    void update(const std::vector<transport::SessionStats>& windows) {
        for (const auto& info : windows) {
            if (info.isIngress) {
                invariant(info.peerId.has_value());
                _sendQueueBytes.record(info.sendBufferBytes, {info.peerId.value()});
                _sendQueueSizes.record(info.sendBufferSizeBytes, {info.peerId.value()});
                _ingressCongestionWindowSizes.record(info.congestionWindowSizeBytes,
                                                     {info.peerId.value()});
            } else {
                _receiveQueueBytes.record(info.receiveBufferBytes);
                _receiveQueueSizes.record(info.receiveBufferSizeBytes);
                _egressCongestionWindowSizes.record(info.congestionWindowSizeBytes);
            }
        }
    }

    void recordCollectErrors(int64_t numErrors) {
        _collectionFailures.add(numErrors);
    }

private:
    otel::metrics::Histogram<int64_t, int64_t>& _sendQueueBytes;
    otel::metrics::Histogram<int64_t, int64_t>& _sendQueueSizes;
    otel::metrics::Histogram<int64_t>& _receiveQueueBytes;
    otel::metrics::Histogram<int64_t>& _receiveQueueSizes;
    otel::metrics::Histogram<int64_t, int64_t>& _ingressCongestionWindowSizes;
    otel::metrics::Histogram<int64_t>& _egressCongestionWindowSizes;
    Counter<int64_t>& _collectionFailures;
};

SessionMetrics::SessionMetrics() : _impl(std::make_unique<Impl>()) {}
SessionMetrics::~SessionMetrics() = default;

void SessionMetrics::update(const std::vector<transport::SessionStats>& info) {
    _impl->update(info);
}

void SessionMetrics::recordCollectErrors(int64_t numErrors) {
    _impl->recordCollectErrors(numErrors);
}

transport::SessionStatsResult collectReplicationSessionStats() {
    if (MONGO_unlikely(failCollectReplicationSessionStats.shouldFail())) {
        int64_t errorCount = 0;
        failCollectReplicationSessionStats.execute(
            [&](const BSONObj& data) { errorCount = data["errorCount"].numberInt(); });
        return {.errorCount = errorCount};
    }
    return getGlobalServiceContext()
        ->getTransportLayerManager()
        ->getDefaultEgressLayer()
        ->collectReplicationSessionStats();
}

void runReplicationSessionCollectionCycle(SessionMetrics& metrics) {
    auto [stats, errorCount] = collectReplicationSessionStats();
    metrics.update(stats);
    metrics.recordCollectErrors(errorCount);
}


void installReplicationSessionOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getReplicationSessionOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<SessionMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "ReplicationSessionOtelMetrics",
        [&state](Client*) { runReplicationSessionCollectionCycle(*state.metrics); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}
}  // namespace mongo

