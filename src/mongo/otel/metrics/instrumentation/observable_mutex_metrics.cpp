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

#include "mongo/otel/metrics/instrumentation/observable_mutex_metrics.h"

#include "mongo/db/service_context.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/observable_mutex_registry.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/string_map.h"

#include <fmt/format.h>

namespace mongo {
namespace {

using otel::metrics::Counter;
using otel::metrics::DynamicMetricNameMaker;
using otel::metrics::MetricsService;
using otel::metrics::MetricUnit;

struct ObservableMutexOtelMetricsState {
    std::unique_ptr<ObservableMutexMetrics> metrics;
    PeriodicJobAnchor job;
};

const auto getObservableMutexOtelMetricsState =
    ServiceContext::declareDecoration<ObservableMutexOtelMetricsState>();

}  // namespace

class ObservableMutexMetrics::Impl {
public:
    Impl() = default;

    void update(const StringMap<MutexStats>& statsPerTag) {
        for (auto& [tag, stats] : statsPerTag) {
            auto it = _tagStates.find(tag);
            if (it == _tagStates.end()) {
                it = _tagStates.emplace(tag, TagState{.counters = _makeCounters(tag)}).first;
            }
            _addDeltas(it->second, stats);
        }
    }

private:
    struct TagCounters {
        Counter<int64_t>* exclusiveTotal{nullptr};
        Counter<int64_t>* exclusiveContentions{nullptr};
        Counter<int64_t>* exclusiveWaitCycles{nullptr};
        Counter<int64_t>* sharedTotal{nullptr};
        Counter<int64_t>* sharedContentions{nullptr};
        Counter<int64_t>* sharedWaitCycles{nullptr};
    };

    struct TagState {
        TagCounters counters;
        MutexStats prevCounters{};
    };

    StringMap<TagState> _tagStates;

    TagCounters _makeCounters(const std::string& tag) {
        auto passkey = ObservableMutexMetrics::dyn_metric_passkey();
        const auto makeCounter = [&](std::string_view field,
                                     std::string desc) -> Counter<int64_t>& {
            auto name = fmt::format("serverStatus.lockContentionMetrics.{}.{}", tag, field);
            return MetricsService::instance().createInt64Counter(
                DynamicMetricNameMaker::make(name, passkey), std::move(desc), MetricUnit::kCount);
        };

        return TagCounters{
            .exclusiveTotal = &makeCounter("exclusive.total", "Total exclusive acquisitions"),
            .exclusiveContentions =
                &makeCounter("exclusive.contentions", "Contended exclusive acquisitions"),
            .exclusiveWaitCycles = &makeCounter("exclusive.waitCycles",
                                                "Wait cycles for contended exclusive acquisitions"),
            .sharedTotal = &makeCounter("shared.total", "Total shared acquisitions"),
            .sharedContentions =
                &makeCounter("shared.contentions", "Contended shared acquisitions"),
            .sharedWaitCycles =
                &makeCounter("shared.waitCycles", "Wait cycles for contended shared acquisitions"),
        };
    }

    void _addDeltas(TagState& state, const MutexStats& currStats) {
        const auto delta = [](uint64_t prev, uint64_t curr) -> int64_t {
            return static_cast<int64_t>(curr >= prev ? curr - prev : 0);
        };

        const auto& curEx = currStats.exclusiveAcquisitions;
        const auto& prevEx = state.prevCounters.exclusiveAcquisitions;
        state.counters.exclusiveTotal->add(delta(prevEx.total, curEx.total));
        state.counters.exclusiveContentions->add(delta(prevEx.contentions, curEx.contentions));
        state.counters.exclusiveWaitCycles->add(delta(prevEx.waitCycles, curEx.waitCycles));

        const auto& curSh = currStats.sharedAcquisitions;
        const auto& prevSh = state.prevCounters.sharedAcquisitions;
        state.counters.sharedTotal->add(delta(prevSh.total, curSh.total));
        state.counters.sharedContentions->add(delta(prevSh.contentions, curSh.contentions));
        state.counters.sharedWaitCycles->add(delta(prevSh.waitCycles, curSh.waitCycles));

        state.prevCounters = currStats;
    }
};

ObservableMutexMetrics::ObservableMutexMetrics() : _impl(std::make_unique<Impl>()) {}

ObservableMutexMetrics::~ObservableMutexMetrics() = default;

void ObservableMutexMetrics::update(const StringMap<MutexStats>& statsPerTag) {
    _impl->update(statsPerTag);
}

void installObservableMutexMetrics(ServiceContext* svcCtx) {
    auto& state = getObservableMutexOtelMetricsState(svcCtx);
    state.metrics = std::make_unique<ObservableMutexMetrics>();
    state.job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "ObservableMutexOtelMetrics",
        [&state](Client*) { state.metrics->update(ObservableMutexRegistry::get().statsPerTag()); },
        Seconds(1),
        false /*isKillableByStepdown*/});
    state.job.start();
}

}  // namespace mongo
