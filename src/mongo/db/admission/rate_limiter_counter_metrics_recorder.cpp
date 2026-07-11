// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/rate_limiter_counter_metrics_recorder.h"

#include <type_traits>
#include <variant>

namespace mongo::admission {

void RateLimiterCounterMetricsRecorder::record(
    const RateLimiterMetricsRecorderEvent& event) noexcept {
    std::visit(
        [this](const auto& typedEvent) noexcept {
            using TypedEvent = std::decay_t<decltype(typedEvent)>;
            if constexpr (std::is_same_v<TypedEvent, AttemptedAdmission>) {
                _attemptedAdmissions.incrementRelaxed();
            } else if constexpr (std::is_same_v<TypedEvent, SuccessfulAdmission>) {
                _successfulAdmissions.incrementRelaxed();
                _tokensAcquired.fetchAndAddRelaxed(typedEvent.tokens);
            } else if constexpr (std::is_same_v<TypedEvent, RejectedAdmission>) {
                _rejectedAdmissions.incrementRelaxed();
            } else if constexpr (std::is_same_v<TypedEvent, AddedToQueue>) {
                _addedToQueue.incrementRelaxed();
            } else if constexpr (std::is_same_v<TypedEvent, RemovedFromQueue>) {
                _removedFromQueue.incrementRelaxed();
            } else if constexpr (std::is_same_v<TypedEvent, InterruptedInQueue>) {
                _interruptedInQueue.incrementRelaxed();
            } else if constexpr (std::is_same_v<TypedEvent, ExemptedAdmission>) {
                _exemptedAdmissions.incrementRelaxed();
            } else if constexpr (std::is_same_v<TypedEvent, AverageTimeQueuedMicros>) {
                _averageTimeQueuedMicros.addSample(typedEvent.sample);
            } else if constexpr (std::is_same_v<TypedEvent, TokensAcquired>) {
                _tokensAcquired.fetchAndAddRelaxed(typedEvent.tokens);
            } else if constexpr (std::is_same_v<TypedEvent, TokensAvailable>) {
                _tokensAvailable.storeRelaxed(typedEvent.available);
            }
        },
        event);
}

int64_t RateLimiterCounterMetricsRecorder::addedToQueue() const {
    return _addedToQueue.get();
}

int64_t RateLimiterCounterMetricsRecorder::removedFromQueue() const {
    return _removedFromQueue.get();
}

int64_t RateLimiterCounterMetricsRecorder::interruptedInQueue() const {
    return _interruptedInQueue.get();
}

int64_t RateLimiterCounterMetricsRecorder::rejectedAdmissions() const {
    return _rejectedAdmissions.get();
}

int64_t RateLimiterCounterMetricsRecorder::successfulAdmissions() const {
    return _successfulAdmissions.get();
}

int64_t RateLimiterCounterMetricsRecorder::exemptedAdmissions() const {
    return _exemptedAdmissions.get();
}

int64_t RateLimiterCounterMetricsRecorder::attemptedAdmissions() const {
    return _attemptedAdmissions.get();
}

boost::optional<double> RateLimiterCounterMetricsRecorder::averageTimeQueuedMicros() const {
    return _averageTimeQueuedMicros.get();
}

double RateLimiterCounterMetricsRecorder::tokensAcquired() const {
    return _tokensAcquired.loadRelaxed();
}

double RateLimiterCounterMetricsRecorder::tokensAvailable() const {
    return _tokensAvailable.loadRelaxed();
}

int64_t RateLimiterCounterMetricsRecorder::currentQueueDepth() const {
    return _addedToQueue.get() - _removedFromQueue.get();
}

}  // namespace mongo::admission
