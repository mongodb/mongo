// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/elapsed_tracker.h"

#include "mongo/util/clock_source.h"

namespace mongo {

ElapsedTracker::ElapsedTracker(ClockSource* cs,
                               int32_t hitsBetweenMarks,
                               Milliseconds msBetweenMarks)
    : _clock(cs),
      _hitsBetweenMarks(hitsBetweenMarks),
      _msBetweenMarks(msBetweenMarks),
      _pings(0),
      _last(cs->now()) {}

bool ElapsedTracker::intervalHasElapsed() {
    if (_hitsBetweenMarks >= 0 && ++_pings >= _hitsBetweenMarks) {
        _pings = 0;
        _last = _clock->now();
        return true;
    }

    const auto now = _clock->now();
    if (now - _last > _msBetweenMarks) {
        _pings = 0;
        _last = now;
        return true;
    }

    return false;
}

void ElapsedTracker::resetLastTime() {
    _pings = 0;
    _last = _clock->now();
}

}  // namespace mongo
