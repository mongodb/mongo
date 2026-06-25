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

#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

using otel::metrics::AttributeDefinition;
using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ReportingPolicy;

constexpr std::string_view kProcStatPath = "/proc/stat"sv;
constexpr std::string_view kProcFileNrPath = "/proc/sys/fs/file-nr"sv;

const std::vector<std::string_view> kStatKeys{"cpu"sv, "procs_running"sv, "procs_blocked"sv};

struct SystemHealthOtelMetricsState {
    std::unique_ptr<SystemHealthMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getSystemHealthOtelMetricsState =
    ServiceContext::declareDecoration<SystemHealthOtelMetricsState>();

const AttributeDefinition<std::string_view> kCpuModeAttr{
    .name = "mode",
    .values = {"user"sv,
               "nice"sv,
               "system"sv,
               "idle"sv,
               "iowait"sv,
               "irq"sv,
               "softirq"sv,
               "steal"sv,
               "guest"sv,
               "guest_nice"sv},
};

}  // namespace

MONGO_FAIL_POINT_DEFINE(failCollectSystemHealthSnapshot);

class SystemHealthMetrics::Impl {
public:
    Impl()
        : _cpuTime(MetricsService::instance().createInt64Counter<std::string_view>(
              MetricNames::kCpuTime,
              "Total CPU time since boot, by mode",
              MetricUnit::kMilliseconds,
              kCpuModeAttr)),
          _cpuUtilization(MetricsService::instance().createDoubleGauge<std::string_view>(
              MetricNames::kCpuUtilization,
              "Per-mode CPU utilization across all modes, summing to 1.0 when utilization is "
              "positive",
              MetricUnit::kRatio,
              kCpuModeAttr,
              otel::metrics::GaugeOptions{.reportingPolicy = ReportingPolicy::kUnconditionally})),
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

    void updateCpuTime(const SystemHealthSnapshot& snap) {
        struct ModeDelta {
            std::string_view mode;
            int64_t delta;
        };
        std::array<ModeDelta, 10> modes;
        size_t n = 0;
        int64_t totalDelta = 0;

        auto record = [&](std::string_view mode, int64_t& prev, int64_t current) {
            int64_t delta = std::max<int64_t>(current - std::exchange(prev, current), 0);
            modes[n++] = {mode, delta};
            totalDelta += delta;
            if (delta > 0)
                _cpuTime.add(delta, mode);
        };

        record("user"sv, _prev.cpuUserMs, snap.cpuUserMs);
        record("nice"sv, _prev.cpuNiceMs, snap.cpuNiceMs);
        record("system"sv, _prev.cpuSystemMs, snap.cpuSystemMs);
        record("idle"sv, _prev.cpuIdleMs, snap.cpuIdleMs);
        record("iowait"sv, _prev.cpuIowaitMs, snap.cpuIowaitMs);
        record("irq"sv, _prev.cpuIrqMs, snap.cpuIrqMs);
        record("softirq"sv, _prev.cpuSoftirqMs, snap.cpuSoftirqMs);
        record("steal"sv, _prev.cpuStealMs, snap.cpuStealMs);
        record("guest"sv, _prev.cpuGuestMs, snap.cpuGuestMs);
        record("guest_nice"sv, _prev.cpuGuestNiceMs, snap.cpuGuestNiceMs);

        // If nothing has changed, just skip updating the utilization.
        // This prevents a potential divide by zero.
        if (totalDelta == 0)
            return;
        for (const auto& [mode, delta] : modes)
            _cpuUtilization.set(static_cast<double>(delta) / totalDelta, mode);
    }

    void update(const SystemHealthSnapshot& snap) {
        updateCpuTime(snap);

        _threadActive.set(snap.procsRunning);
        _threadQueued.set(snap.procsBlocked);
        _fdOpen.set(snap.fdOpen);
    }

private:
    Counter<int64_t, std::string_view>& _cpuTime;
    Gauge<double, std::string_view>& _cpuUtilization;
    Gauge<int64_t>& _threadActive;
    Gauge<int64_t>& _threadQueued;
    Gauge<int64_t>& _fdOpen;
    Counter<int64_t>& _collectErrors;

    SystemHealthSnapshot _prev;
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
    snap.cpuNiceMs = stat["nice_ms"].safeNumberLong();
    snap.cpuSystemMs = stat["system_ms"].safeNumberLong();
    snap.cpuIdleMs = stat["idle_ms"].safeNumberLong();
    snap.cpuIowaitMs = stat["iowait_ms"].safeNumberLong();
    snap.cpuIrqMs = stat["irq_ms"].safeNumberLong();
    snap.cpuSoftirqMs = stat["softirq_ms"].safeNumberLong();
    snap.cpuStealMs = stat["steal_ms"].safeNumberLong();
    // We support kernels old enough to not have guest/guest_nice,
    // but safeNumberLong will just return 0 if they're not available.
    snap.cpuGuestMs = stat["guest_ms"].safeNumberLong();
    snap.cpuGuestNiceMs = stat["guest_nice_ms"].safeNumberLong();
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
