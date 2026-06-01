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

#include "mongo/otel/metrics/instrumentation/global_lock_metrics.h"

#include "mongo/db/service_context.h"
#include "mongo/db/stats/global_lock_stats.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

struct GlobalLockOtelMetricsState {
    std::unique_ptr<GlobalLockMetrics> metrics;
    Date_t startedAt;
    PeriodicJobAnchor job;
};

const auto getGlobalLockOtelMetricsState =
    ServiceContext::declareDecoration<GlobalLockOtelMetricsState>();

}  // namespace

class GlobalLockMetrics::Impl {
public:
    Impl()
        : _totalTime(MetricsService::instance().createInt64Gauge(
              MetricNames::kGlobalLockTotalTime,
              "Total time in microseconds since the global lock OpenTelemetry updater was started",
              MetricUnit::kMicroseconds)),
          _currentQueueTotal(MetricsService::instance().createInt64Gauge(
              MetricNames::kGlobalLockCurrentQueueTotal,
              "Current number of clients queued for the global lock",
              MetricUnit::kCount)),
          _currentQueueReaders(MetricsService::instance().createInt64Gauge(
              MetricNames::kGlobalLockCurrentQueueReaders,
              "Current number of readers queued for the global lock",
              MetricUnit::kCount)),
          _currentQueueWriters(MetricsService::instance().createInt64Gauge(
              MetricNames::kGlobalLockCurrentQueueWriters,
              "Current number of writers queued for the global lock",
              MetricUnit::kCount)),
          _activeClientsTotal(MetricsService::instance().createInt64Gauge(
              MetricNames::kGlobalLockActiveClientsTotal,
              "Current number of active clients holding global lock tickets",
              MetricUnit::kCount)),
          _activeClientsReaders(MetricsService::instance().createInt64Gauge(
              MetricNames::kGlobalLockActiveClientsReaders,
              "Current number of active readers holding global lock tickets",
              MetricUnit::kCount)),
          _activeClientsWriters(MetricsService::instance().createInt64Gauge(
              MetricNames::kGlobalLockActiveClientsWriters,
              "Current number of active writers holding global lock tickets",
              MetricUnit::kCount)) {}

    void update(const GlobalLockStatsSnapshot& snap) {
        _totalTime.set(snap.totalTimeMicros);
        _currentQueueTotal.set(snap.queuedReaders + snap.queuedWriters);
        _currentQueueReaders.set(snap.queuedReaders);
        _currentQueueWriters.set(snap.queuedWriters);
        _activeClientsTotal.set(snap.activeReaders + snap.activeWriters);
        _activeClientsReaders.set(snap.activeReaders);
        _activeClientsWriters.set(snap.activeWriters);
    }

private:
    Gauge<int64_t>& _totalTime;
    Gauge<int64_t>& _currentQueueTotal;
    Gauge<int64_t>& _currentQueueReaders;
    Gauge<int64_t>& _currentQueueWriters;
    Gauge<int64_t>& _activeClientsTotal;
    Gauge<int64_t>& _activeClientsReaders;
    Gauge<int64_t>& _activeClientsWriters;
};

GlobalLockMetrics::GlobalLockMetrics() : _impl(std::make_unique<Impl>()) {}

GlobalLockMetrics::~GlobalLockMetrics() = default;

void GlobalLockMetrics::update(const GlobalLockStatsSnapshot& snap) {
    _impl->update(snap);
}

void installGlobalLockOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getGlobalLockOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<GlobalLockMetrics>();
    state.startedAt = Date_t::now();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "GlobalLockOtelMetrics",
        [&state, svcCtx](Client*) {
            state.metrics->update(collectGlobalLockStatsSnapshot(svcCtx, state.startedAt));
        },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
