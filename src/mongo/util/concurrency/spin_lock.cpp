// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/config.h"  // IWYU pragma: keep

#if !(defined(_WIN32) || MONGO_CONFIG_DEBUG_BUILD)

#include "mongo/platform/pause.h"
#include "mongo/util/concurrency/spin_lock.h"

#include <ctime>

#include <sched.h>

namespace mongo {


void SpinLock::_lockSlowPath() {
    /**
     * this is designed to perform close to the default spin lock
     * the reason for the mild insanity is to prevent horrible performance
     * when contention spikes
     * it allows spinlocks to be used in many more places
     * which is good because even with this change they are about 8x faster on linux
     */

    for (int i = 0; i < 1000; i++) {
        if (_tryLock())
            return;

        MONGO_YIELD_CORE_FOR_SMT();
    }

    for (int i = 0; i < 1000; i++) {
        if (_tryLock())
            return;
        sched_yield();
    }

    struct timespec t;
    t.tv_sec = 0;
    t.tv_nsec = 5000000;

    while (!_tryLock()) {
        nanosleep(&t, nullptr);
    }
}

}  // namespace mongo

#endif
