// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include <variant>

namespace mongo::admission {

/**
 * Events emitted by the rate limiter metrics recorder.
 */

/* Event when a token acquisition is attempted */
struct AttemptedAdmission {};

/* Event when a token acquisition is successful */
struct SuccessfulAdmission {
    double tokens;
};

/* Event when a token acquisition is rejected */
struct RejectedAdmission {};

/* Event when a request is added to the queue */
struct AddedToQueue {};

/* Event when a request is removed from the queue (either to be admitted or rejected) */
struct RemovedFromQueue {};

/* Event when a request in the queue is interrupted */
struct InterruptedInQueue {};

/* Event when a request is exempted from admission */
struct ExemptedAdmission {};

/* Event when the average time queued is recorded */
struct AverageTimeQueuedMicros {
    double sample;
};

/* Event when tokens are acquired */
struct TokensAcquired {
    double tokens;
};

/* Event when the number of available tokens is recorded */
struct TokensAvailable {
    double available;
};

/* Represents any event emitted by the rate limiter metrics recorder */
using RateLimiterMetricsRecorderEvent = std::variant<AttemptedAdmission,
                                                     SuccessfulAdmission,
                                                     RejectedAdmission,
                                                     AddedToQueue,
                                                     RemovedFromQueue,
                                                     InterruptedInQueue,
                                                     ExemptedAdmission,
                                                     AverageTimeQueuedMicros,
                                                     TokensAcquired,
                                                     TokensAvailable>;

}  // namespace mongo::admission
