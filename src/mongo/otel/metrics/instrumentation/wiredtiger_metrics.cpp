// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/otel/metrics/instrumentation/wiredtiger_metrics.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/ticketing_system.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {

using admission::execution_control::TicketingSystem;
using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

struct WiredTigerOtelMetricsState {
    std::unique_ptr<WiredTigerMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getWiredTigerOtelMetricsState =
    ServiceContext::declareDecoration<WiredTigerOtelMetricsState>();

}  // namespace

class WiredTigerMetrics::Impl {
public:
    Impl()
        : _concurrentTransactionsReadAvailable(MetricsService::instance().createInt64Gauge(
              MetricNames::kConcurrentTransactionsReadAvailable,
              "Amount of concurrent read transactions available",
              MetricUnit::kCount)),
          _concurrentTransactionsWriteAvailable(MetricsService::instance().createInt64Gauge(
              MetricNames::kConcurrentTransactionsWriteAvailable,
              "Amount of concurrent write transactions available",
              MetricUnit::kCount)),
          _ticketingSystemCollectErrors(MetricsService::instance().createInt64Counter(
              MetricNames::kTicketingSystemCollectErrors,
              "Number of times reading the ticketing system's stats failed during collection",
              MetricUnit::kCount)) {}

    void updateTicketingSystem(const TicketingSystemStatsSnapshot& snap) {
        _concurrentTransactionsReadAvailable.set(snap.readAvailable);
        _concurrentTransactionsWriteAvailable.set(snap.writeAvailable);
    }

    void recordTSCollectError() {
        _ticketingSystemCollectErrors.add(1);
    }

private:
    Gauge<int64_t>& _concurrentTransactionsReadAvailable;
    Gauge<int64_t>& _concurrentTransactionsWriteAvailable;

    // Error-handling metrics
    Counter<int64_t>& _ticketingSystemCollectErrors;
};

WiredTigerMetrics::WiredTigerMetrics() : _impl(std::make_unique<Impl>()) {}
WiredTigerMetrics::~WiredTigerMetrics() = default;

void WiredTigerMetrics::updateTicketingSystem(const TicketingSystemStatsSnapshot& snap) {
    _impl->updateTicketingSystem(snap);
}

void WiredTigerMetrics::recordTSCollectError() {
    _impl->recordTSCollectError();
}

TicketingSystemStatsSnapshot parseTicketingSystemStats(const BSONObj& stats) {
    TicketingSystemStatsSnapshot snap;

    auto readStats = stats.getObjectField("read");
    snap.readAvailable = readStats["available"].safeNumberInt();
    auto writeStats = stats.getObjectField("write");
    snap.writeAvailable = writeStats["available"].safeNumberInt();

    return snap;
}

void runTicketingSystemCollectionCycle(WiredTigerMetrics& metrics, ServiceContext* svc) {
    auto* ts = TicketingSystem::get(svc);
    if (!ts) {
        metrics.recordTSCollectError();
        return;
    }

    BSONObjBuilder statBuilder;
    ts->appendStats(statBuilder);

    auto stats = statBuilder.obj();
    auto snap = parseTicketingSystemStats(stats);
    metrics.updateTicketingSystem(snap);
}

void installWiredTigerOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getWiredTigerOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<WiredTigerMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "WiredTigerOtelMetrics",
        [&state, svcCtx](Client*) { runTicketingSystemCollectionCycle(*state.metrics, svcCtx); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
