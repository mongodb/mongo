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

#pragma once

#include "mongo/platform/atomic_word.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstdint>

namespace mongo {

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
