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

#include "mongo/db/admission/rate_limiter_otel_metrics_recorder.h"

#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metric_unit.h"
#include "mongo/otel/metrics/metrics_service.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/moving_average.h"

#include <algorithm>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace mongo::admission {

RateLimiterOtelMetricsRecorder::RateLimiterOtelMetricsRecorder(MetricsSpec spec) {
    auto& svc = otel::metrics::MetricsService::instance();

    const auto makeInt64Counter = [&](otel::metrics::MetricName name, const char* description) {
        return &svc.createInt64Counter(name, description, otel::metrics::MetricUnit::kCount);
    };

    _attemptedCounter =
        makeInt64Counter(spec.attemptedAdmissions, "Number of attempted admissions");
    _successfulCounter =
        makeInt64Counter(spec.successfulAdmissions, "Number of successful admissions");
    _rejectedCounter = makeInt64Counter(spec.rejectedAdmissions, "Number of rejected admissions");
    _addedToQueueCounter = makeInt64Counter(spec.addedToQueue, "Number of requests added to queue");
    _removedFromQueueCounter =
        makeInt64Counter(spec.removedFromQueue, "Number of requests removed from queue");
    _interruptedInQueueCounter =
        makeInt64Counter(spec.interruptedInQueue, "Number of queue waits interrupted");
    _exemptedAdmissionsCounter =
        makeInt64Counter(spec.exemptedAdmissions, "Number of exempted admissions");

    _tokensAcquiredCounter = &svc.createDoubleCounter(
        spec.tokensAcquired, "Cumulative tokens acquired", otel::metrics::MetricUnit::kCount);

    _queueDepthGauge = &svc.createInt64Gauge(
        spec.currentQueueDepth, "Current queue depth", otel::metrics::MetricUnit::kCount);

    _tokensAvailableGauge = &svc.createDoubleGauge(
        spec.totalAvailableTokens, "Total available tokens", otel::metrics::MetricUnit::kCount);

    _averageTimeQueuedMicrosGauge =
        &svc.createDoubleGauge(spec.averageTimeQueuedMicros,
                               "Exponential moving average of queue time (micros)",
                               otel::metrics::MetricUnit::kMicroseconds);

    _timeQueuedMicrosHistogram = &svc.createInt64Histogram(
        spec.timeQueuedMicros,
        "Distribution of per-request queue time (micros)",
        otel::metrics::MetricUnit::kMicroseconds,
        otel::metrics::HistogramOptions{.explicitBucketBoundaries = std::vector<double>{50,
                                                                                        100,
                                                                                        250,
                                                                                        500,
                                                                                        1000,
                                                                                        2500,
                                                                                        5000,
                                                                                        10000,
                                                                                        25000,
                                                                                        50000,
                                                                                        100000,
                                                                                        250000,
                                                                                        1000000}});
}

void RateLimiterOtelMetricsRecorder::record(const RateLimiterMetricsRecorderEvent& event) noexcept {
    std::visit(
        [this](const auto& typedEvent) noexcept {
            using TypedEvent = std::decay_t<decltype(typedEvent)>;
            if constexpr (std::is_same_v<TypedEvent, AttemptedAdmission>) {
                _attemptedCounter->add(1);
            } else if constexpr (std::is_same_v<TypedEvent, SuccessfulAdmission>) {
                _successfulCounter->add(1);
                _tokensAcquiredCounter->add(typedEvent.tokens);
            } else if constexpr (std::is_same_v<TypedEvent, RejectedAdmission>) {
                _rejectedCounter->add(1);
            } else if constexpr (std::is_same_v<TypedEvent, AddedToQueue>) {
                _addedToQueueCounter->add(1);
                const auto value = _queueDepthCounter.addAndFetch(1);
                _queueDepthGauge->set(value);
            } else if constexpr (std::is_same_v<TypedEvent, RemovedFromQueue>) {
                _removedFromQueueCounter->add(1);
                const auto value = _queueDepthCounter.subtractAndFetch(1);
                _queueDepthGauge->set(value);
            } else if constexpr (std::is_same_v<TypedEvent, InterruptedInQueue>) {
                _interruptedInQueueCounter->add(1);
            } else if constexpr (std::is_same_v<TypedEvent, ExemptedAdmission>) {
                _exemptedAdmissionsCounter->add(1);
            } else if constexpr (std::is_same_v<TypedEvent, AverageTimeQueuedMicros>) {
                const auto avg = _averageTimeQueuedMicros.addSample(typedEvent.sample);
                _averageTimeQueuedMicrosGauge->set(avg);
                _timeQueuedMicrosHistogram->record(static_cast<int64_t>(typedEvent.sample));
            } else if constexpr (std::is_same_v<TypedEvent, TokensAcquired>) {
                _tokensAcquiredCounter->add(typedEvent.tokens);
            } else if constexpr (std::is_same_v<TypedEvent, TokensAvailable>) {
                const auto value = std::max(0.0, typedEvent.available);
                _tokensAvailable.storeRelaxed(value);
                _tokensAvailableGauge->set(value);
            }
        },
        event);
}

int64_t RateLimiterOtelMetricsRecorder::addedToQueue() const {
    return _addedToQueueCounter->valueForLegacyUse();
}

int64_t RateLimiterOtelMetricsRecorder::removedFromQueue() const {
    return _removedFromQueueCounter->valueForLegacyUse();
}

int64_t RateLimiterOtelMetricsRecorder::interruptedInQueue() const {
    return _interruptedInQueueCounter->valueForLegacyUse();
}

int64_t RateLimiterOtelMetricsRecorder::rejectedAdmissions() const {
    return _rejectedCounter->valueForLegacyUse();
}

int64_t RateLimiterOtelMetricsRecorder::successfulAdmissions() const {
    return _successfulCounter->valueForLegacyUse();
}

int64_t RateLimiterOtelMetricsRecorder::exemptedAdmissions() const {
    return _exemptedAdmissionsCounter->valueForLegacyUse();
}

int64_t RateLimiterOtelMetricsRecorder::attemptedAdmissions() const {
    return _attemptedCounter->valueForLegacyUse();
}

boost::optional<double> RateLimiterOtelMetricsRecorder::averageTimeQueuedMicros() const {
    return _averageTimeQueuedMicros.get();
}

double RateLimiterOtelMetricsRecorder::tokensAcquired() const {
    return _tokensAcquiredCounter->valueForLegacyUse();
}

double RateLimiterOtelMetricsRecorder::tokensAvailable() const {
    return _tokensAvailable.loadRelaxed();
}

int64_t RateLimiterOtelMetricsRecorder::currentQueueDepth() const {
    return _queueDepthCounter.loadRelaxed();
}

PeriodicRunner::JobAnchor RateLimiterOtelMetricsRecorder::installOtelMetrics(
    PeriodicRunner* runner,
    Milliseconds period,
    std::string_view name,
    std::function<double()> sampleAvailableTokens) {
    invariant(runner);
    auto anchor = runner->makeJob(PeriodicRunner::PeriodicJob(
        fmt::format("RateLimiterMetricsSampling-{}", name),
        [this, sampleAvailableTokens = std::move(sampleAvailableTokens)](Client*) {
            record(TokensAvailable{sampleAvailableTokens()});
        },
        period,
        false /* isKillableByStepdown */));
    anchor.start();
    return anchor;
}

}  // namespace mongo::admission
