// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
