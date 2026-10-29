// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/otel/metrics/instrumentation/process_health_metrics.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/processinfo.h"

#include <array>
#include <string_view>

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

namespace {

using namespace std::literals::string_view_literals;

using otel::metrics::AttributeDefinition;
using otel::metrics::Counter;
using otel::metrics::Gauge;
using otel::metrics::MetricNames;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;
using otel::metrics::ReportingPolicy;
using otel::metrics::UpDownCounter;

struct ProcessHealthOtelMetricsState {
    std::unique_ptr<ProcessHealthMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getProcessHealthOtelMetricsState =
    ServiceContext::declareDecoration<ProcessHealthOtelMetricsState>();

const AttributeDefinition<std::string_view> kCpuModeAttr{
    .name = "mode",
    .values =
        {
            "user"sv, "system"sv,
            // TODO (SERVER-129801): OpenTelemetry also supports a "wait" attribute
        },
};

const AttributeDefinition<std::string_view> kSwitchTypeAttr{
    .name = "type",
    .values =
        {
            "voluntary"sv,
            "involuntary"sv,
        },
};

const AttributeDefinition<std::string_view> kPagingFaultTypeAttr{
    .name = "type",
    .values =
        {
            "major"sv,
            "minor"sv,
        },
};

}  // namespace

MONGO_FAIL_POINT_DEFINE(failCollectProcessHealthSnapshot);

class ProcessHealthMetrics::Impl {
public:
    Impl()
        : _cpuTime(MetricsService::instance().createInt64Counter<std::string_view>(
              MetricNames::kProcessCpuTime,
              "Total process CPU time since boot, by mode",
              MetricUnit::kMilliseconds,
              kCpuModeAttr)),
          _cpuUtilization(MetricsService::instance().createDoubleGauge<std::string_view>(
              MetricNames::kProcessCpuUtilization,
              "Per-mode process CPU utilization across all modes, summing to 1.0",
              MetricUnit::kRatio,
              kCpuModeAttr,
              otel::metrics::GaugeOptions{.reportingPolicy = ReportingPolicy::kUnconditionally})),
          _contextSwitches(MetricsService::instance().createInt64Counter<std::string_view>(
              MetricNames::kProcessContextSwitches,
              "Number of times the process has been context switched",
              MetricUnit::kCount,
              kSwitchTypeAttr)),
          _threadCount(MetricsService::instance().createInt64UpDownCounter(
              MetricNames::kProcessThreadCount,
              "Number of threads currently used by the process",
              MetricUnit::kCount)),
          _pagingFaults(MetricsService::instance().createInt64Counter<std::string_view>(
              MetricNames::kProcessPagingFaults,
              "Number of page faults the process has made, by major and minor",
              MetricUnit::kCount,
              kPagingFaultTypeAttr)),
          _collectErrors(MetricsService::instance().createInt64Counter(
              MetricNames::kProcessHealthCollectErrors,
              "Number of times process info reading failed during process health collection",
              MetricUnit::kCount)) {}

    void update(const ProcessHealthSnapshot& snap);
    void updateCpuTime(const ProcessHealthSnapshot& snap);
    void updateContextSwitches(const ProcessHealthSnapshot& snap);
    void updateThreadCount(const ProcessHealthSnapshot& snap);
    void updatePagingFaults(const ProcessHealthSnapshot& snap);

    void recordCollectError() {
        _collectErrors.add(1);
    }

private:
    Counter<int64_t, std::string_view>& _cpuTime;
    Gauge<double, std::string_view>& _cpuUtilization;
    Counter<int64_t, std::string_view>& _contextSwitches;
    UpDownCounter<int64_t>& _threadCount;
    Counter<int64_t, std::string_view>& _pagingFaults;
    Counter<int64_t>& _collectErrors;

    ProcessHealthSnapshot _prev;
};

ProcessHealthMetrics::ProcessHealthMetrics() : _impl(std::make_unique<Impl>()) {}
ProcessHealthMetrics::~ProcessHealthMetrics() = default;

void ProcessHealthMetrics::update(const ProcessHealthSnapshot& snap) {
    _impl->update(snap);
}

void ProcessHealthMetrics::recordCollectError() {
    _impl->recordCollectError();
}

void ProcessHealthMetrics::Impl::update(const ProcessHealthSnapshot& snap) {
    updateCpuTime(snap);
    updateContextSwitches(snap);
    updateThreadCount(snap);
    updatePagingFaults(snap);
}

void ProcessHealthMetrics::Impl::updateCpuTime(const ProcessHealthSnapshot& snap) {
    struct ModeDelta {
        std::string_view mode;
        int64_t delta;
    };
    std::array<ModeDelta, 2> modes;
    size_t n = 0;
    int64_t totalDelta = 0;

    auto record = [&](std::string_view mode, int64_t& prev, int64_t current) {
        invariant(n < modes.size());
        int64_t delta = std::max<int64_t>(current - std::exchange(prev, current), 0);

        modes[n++] = {mode, delta};
        totalDelta += delta;
        if (delta > 0)
            _cpuTime.add(delta, mode);
    };

    record("user"sv, _prev.userMs, snap.userMs);
    record("system"sv, _prev.systemMs, snap.systemMs);

    // If nothing has changed, just skip updating the utilization.
    // This prevents a potential divide by zero.
    if (totalDelta == 0)
        return;
    for (const auto& [mode, delta] : modes)
        _cpuUtilization.set(static_cast<double>(delta) / totalDelta, mode);
}

void ProcessHealthMetrics::Impl::updateContextSwitches(const ProcessHealthSnapshot& snap) {
    auto record = [&](std::string_view type, int64_t& prev, int64_t current) {
        int64_t delta = std::max<int64_t>(current - std::exchange(prev, current), 0);
        if (delta > 0)
            _contextSwitches.add(delta, type);
    };

    record("voluntary"sv, _prev.voluntaryContextSwitches, snap.voluntaryContextSwitches);
    record("involuntary"sv, _prev.involuntaryContextSwitches, snap.involuntaryContextSwitches);
}

void ProcessHealthMetrics::Impl::updateThreadCount(const ProcessHealthSnapshot& snap) {
    const auto current = snap.threadCount;
    // We add the delta unconditionally because thread count could go down!
    // So adding a negative delta in that case is fine.
    int64_t delta = current - std::exchange(_prev.threadCount, current);
    _threadCount.add(delta);
}

void ProcessHealthMetrics::Impl::updatePagingFaults(const ProcessHealthSnapshot& snap) {
    auto record = [&](std::string_view type, int64_t& prev, int64_t current) {
        int64_t delta = std::max<int64_t>(current - std::exchange(prev, current), 0);
        if (delta > 0)
            _pagingFaults.add(delta, type);
    };

    record("major"sv, _prev.majorPagingFaults, snap.majorPagingFaults);
    record("minor"sv, _prev.minorPagingFaults, snap.minorPagingFaults);
}

boost::optional<ProcessHealthSnapshot> collectProcessHealthSnapshot() {
    if (MONGO_unlikely(failCollectProcessHealthSnapshot.shouldFail()))
        return boost::none;

    ProcessInfo pi;
    BSONObjBuilder infoBuilder;

    // getExtraInfo can throw if we fail to open an fd
    try {
        pi.getExtraInfo(infoBuilder);
    } catch (const DBException& e) {
        static logv2::SeveritySuppressor suppressor(
            Minutes{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(3));
        if (auto sev = suppressor(); shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, sev)) {
            LOGV2_DEBUG(13043600,
                        sev.toInt(),
                        "Failed to collect process health metrics",
                        "error"_attr = e.toStatus());
        }
        return boost::none;
    }

    BSONObj info = infoBuilder.obj();

    ProcessHealthSnapshot snap;
    snap.userMs = info["user_time_us"].safeNumberLong() / 1000;
    snap.systemMs = info["system_time_us"].safeNumberLong() / 1000;
    snap.voluntaryContextSwitches = info["voluntary_context_switches"].safeNumberLong();
    snap.involuntaryContextSwitches = info["involuntary_context_switches"].safeNumberLong();
    snap.threadCount = info["threads"].safeNumberLong();
    snap.majorPagingFaults = info["page_faults"].safeNumberLong();
    snap.minorPagingFaults = info["page_reclaims"].safeNumberLong();
    return snap;
}

void runProcessHealthCollectionCycle(ProcessHealthMetrics& metrics) {
    if (auto snap = collectProcessHealthSnapshot())
        metrics.update(*snap);
    else
        metrics.recordCollectError();
}

void installProcessHealthOtelMetrics(ServiceContext* svcCtx) {
    auto& state = getProcessHealthOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<ProcessHealthMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "ProcessHealthOtelMetrics",
        [&state](Client*) { runProcessHealthCollectionCycle(*state.metrics); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
