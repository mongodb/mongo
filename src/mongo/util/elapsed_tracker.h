// @file elapsed_tracker.h

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
 */

#pragma once

#include "mongo/platform/cstdint.h"

namespace mongo {

    /** Keep track of elapsed time. After a set amount of time, tells you to do something. */
    class ElapsedTracker {
    public:
        ElapsedTracker( int32_t hitsBetweenMarks, int32_t msBetweenMarks );

        /**
         * Call this for every iteration.
         * @return true if one of the triggers has gone off.
         */
        bool intervalHasElapsed();

        void resetLastTime();
        
    private:
        const int32_t _hitsBetweenMarks;
        const int32_t _msBetweenMarks;

        uint64_t _pings;

        int64_t _last;
    };

} // namespace mongo
