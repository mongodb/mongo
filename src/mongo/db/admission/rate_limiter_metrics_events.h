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
