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
