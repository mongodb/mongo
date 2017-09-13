// @file elapsed_tracker.cpp

/**
 *    Copyright (C) 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

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
    if (++_pings >= _hitsBetweenMarks) {
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
