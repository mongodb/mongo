// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ClockSource;

/**
 * Keeps track of elapsed time. After a set amount of time, or a set number of iterations, tells you
 * to do something.
 */
class ElapsedTracker {
public:
    /**
     * Either 'hitsBetweenMarks' calls to intervalHasElapsed() occur before intervalHasElapsed()
     * returns true, or 'msBetweenMarks' time must elapse before intervalHasElapsed() returns true.
     */
    ElapsedTracker(ClockSource* cs, int32_t hitsBetweenMarks, Milliseconds msBetweenMarks);

    /**
     * Call this for every iteration.
     *
     * Returns true after either _hitsBetweenMarks calls occur or _msBetweenMarks time has elapsed
     * since that last true response. Both triggers are reset whenever true is returned.
     */
    bool intervalHasElapsed();

    void resetLastTime();

private:
    ClockSource* const _clock;
    const int32_t _hitsBetweenMarks;
    const Milliseconds _msBetweenMarks;

    int32_t _pings;

    Date_t _last;
};

}  // namespace mongo
