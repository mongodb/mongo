/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
