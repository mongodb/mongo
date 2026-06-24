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
