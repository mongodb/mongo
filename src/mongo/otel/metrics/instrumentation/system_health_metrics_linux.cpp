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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/instrumentation/system_health_metrics.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/procparser.h"

#include <utility>
#include <vector>

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {

using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

constexpr StringData kProcStatPath = "/proc/stat"_sd;
constexpr StringData kProcFileNrPath = "/proc/sys/fs/file-nr"_sd;

const std::vector<StringData> kStatKeys{"cpu"_sd, "procs_running"_sd, "procs_blocked"_sd};

struct SystemHealthOtelMetricsState {
    std::unique_ptr<SystemHealthMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getSystemHealthOtelMetricsState =
    ServiceContext::declareDecoration<SystemHealthOtelMetricsState>();

}  // namespace

MONGO_FAIL_POINT_DEFINE(failCollectSystemHealthSnapshot);

class SystemHealthMetrics::Impl {
public:
    Impl()
        : _cpuUser(MetricsService::instance().createInt64Counter(
              MetricNames::kCpuUserMs,
              "Total CPU time spent in user mode since boot",
              MetricUnit::kMilliseconds)),
          _cpuSystem(MetricsService::instance().createInt64Counter(
              MetricNames::kCpuSystemMs,
              "Total CPU time spent in kernel mode since boot",
              MetricUnit::kMilliseconds)),
          _cpuIowait(MetricsService::instance().createInt64Counter(
              MetricNames::kCpuIowaitMs,
              "Total CPU time spent waiting for I/O since boot",
              MetricUnit::kMilliseconds)),
          _threadActive(MetricsService::instance().createInt64Gauge(
              MetricNames::kThreadActive,
              "Number of OS threads/processes currently in a runnable state",
              MetricUnit::kCount)),
          _threadQueued(MetricsService::instance().createInt64Gauge(
              MetricNames::kThreadQueued,
              "Number of OS threads/processes currently blocked waiting for I/O",
              MetricUnit::kCount)),
          _fdOpen(MetricsService::instance().createInt64Gauge(
              MetricNames::kFdOpen,
              "Number of file handles currently open system-wide",
              MetricUnit::kCount)),
          _collectErrors(MetricsService::instance().createInt64Counter(
              MetricNames::kSystemHealthCollectErrors,
              "Number of times /proc parsing failed during system health collection",
              MetricUnit::kCount)) {}

    void recordCollectError() {
        _collectErrors.add(1);
    }

    void update(const SystemHealthSnapshot& snap) {
        auto addDelta = [](Counter<int64_t>& counter, int64_t& prev, int64_t current) {
            int64_t delta = current - std::exchange(prev, current);
            if (delta > 0)
                counter.add(delta);
        };
        addDelta(_cpuUser, _prevCpuUserMs, snap.cpuUserMs);
        addDelta(_cpuSystem, _prevCpuSystemMs, snap.cpuSystemMs);
        addDelta(_cpuIowait, _prevCpuIowaitMs, snap.cpuIowaitMs);
        _threadActive.set(snap.procsRunning);
        _threadQueued.set(snap.procsBlocked);
        _fdOpen.set(snap.fdOpen);
    }

private:
    Counter<int64_t>& _cpuUser;
    Counter<int64_t>& _cpuSystem;
    Counter<int64_t>& _cpuIowait;
    Gauge<int64_t>& _threadActive;
    Gauge<int64_t>& _threadQueued;
    Gauge<int64_t>& _fdOpen;
    Counter<int64_t>& _collectErrors;

    int64_t _prevCpuUserMs = 0;
    int64_t _prevCpuSystemMs = 0;
    int64_t _prevCpuIowaitMs = 0;
};

SystemHealthMetrics::SystemHealthMetrics() : _impl(std::make_unique<Impl>()) {}

SystemHealthMetrics::~SystemHealthMetrics() = default;

void SystemHealthMetrics::update(const SystemHealthSnapshot& snap) {
    _impl->update(snap);
}

void SystemHealthMetrics::recordCollectError() {
    _impl->recordCollectError();
}

boost::optional<SystemHealthSnapshot> collectSystemHealthSnapshot() {
    if (MONGO_unlikely(failCollectSystemHealthSnapshot.shouldFail())) {
        return boost::none;
    }

    BSONObjBuilder statBuilder;
    if (Status s = procparser::parseProcStatFile(kProcStatPath, kStatKeys, &statBuilder);
        !s.isOK()) {
        static logv2::SeveritySuppressor suppressor(
            Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(3));
        if (auto sev = suppressor(); shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, sev)) {
            LOGV2_DEBUG(11908300, sev.toInt(), "Failed to collect /proc/stat", "error"_attr = s);
        }
        return boost::none;
    }
    BSONObj stat = statBuilder.obj();

    BSONObjBuilder fileNrBuilder;
    if (Status fs = procparser::parseProcSysFsFileNrFile(
            kProcFileNrPath, procparser::FileNrKey::kFileHandlesInUse, &fileNrBuilder);
        !fs.isOK()) {
        static logv2::SeveritySuppressor suppressor(
            Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(3));
        if (auto sev = suppressor(); shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, sev)) {
            LOGV2_DEBUG(
                11908301, sev.toInt(), "Failed to collect /proc/sys/fs/file-nr", "error"_attr = fs);
        }
        return boost::none;
    }
    BSONObj fileNr = fileNrBuilder.obj();

    SystemHealthSnapshot snap;
    snap.cpuUserMs = stat["user_ms"].safeNumberLong();
    snap.cpuSystemMs = stat["system_ms"].safeNumberLong();
    snap.cpuIowaitMs = stat["iowait_ms"].safeNumberLong();
    snap.procsRunning = stat["procs_running"].safeNumberLong();
    snap.procsBlocked = stat["procs_blocked"].safeNumberLong();
    snap.fdOpen = fileNr[procparser::kFileHandlesInUseKey].safeNumberLong();
    return snap;
}

void runSystemHealthCollectionCycle(SystemHealthMetrics& metrics) {
    if (auto snap = collectSystemHealthSnapshot()) {
        metrics.update(*snap);
    } else {
        metrics.recordCollectError();
    }
}

void installSystemHealthOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getSystemHealthOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<SystemHealthMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "SystemHealthOtelMetrics",
        [&state](Client*) { runSystemHealthCollectionCycle(*state.metrics); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
