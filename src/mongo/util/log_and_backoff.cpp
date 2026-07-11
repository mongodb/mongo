// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/util/log_and_backoff.h"

#include "mongo/platform/random.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo::log_backoff_detail {

namespace {
// Per-thread PRNG used to jitter backoff sleeps so competing threads wake at different times.
thread_local PseudoRandom tlsPrng{SecureRandom{}.nextInt64()};
}  // namespace

void logAndBackoffImpl(size_t numAttempts) {
    int64_t base = 0;
    if (numAttempts < 4) {
        return;
    } else if (numAttempts < 10) {
        base = 1;
    } else if (numAttempts < 100) {
        base = 5;
    } else if (numAttempts < 200) {
        base = 10;
    } else {
        base = 100;
    }
    // Full jitter: uniform in [base, 2*base] ms. Competing threads that hit this path
    // simultaneously sleep for different durations, spreading their wakeups over time
    // instead of flooding the ticket queue as a synchronized burst.
    sleepmillis(base + tlsPrng.nextInt64(base + 1));
}

}  // namespace mongo::log_backoff_detail
