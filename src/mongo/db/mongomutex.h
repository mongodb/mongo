// @file mongomutex.h

/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "../util/time_support.h"
#include "d_globals.h"

namespace mongo {

    class Client;

    /* mongomutex time stats */
    class MutexInfo {
        unsigned long long enter, timeLocked; // microseconds
        int locked;
        unsigned long long start; // last as we touch this least often
    public:
        MutexInfo() : timeLocked(0) , locked(0) {
            start = curTimeMicros64();
        }
        void entered() {
            if ( locked == 0 )
                enter = curTimeMicros64();
            locked++;
            verify( locked >= 1 );
        }
        void leaving() {
            locked--;
            verify( locked >= 0 );
            if ( locked == 0 )
                timeLocked += curTimeMicros64() - enter;
        }
        int isLocked() const { return locked; }
        void getTimingInfo(unsigned long long &s, unsigned long long &tl) const {
            s = start;
            tl = timeLocked;
        }
        unsigned long long getTimeLocked() const { return timeLocked; }
    };

    /** old. see Lock class in d_concurrency.h instead. */
    class MongoMutex : boost::noncopyable {
    public:
        MongoMutex();
        MutexInfo& info() { return _minfo; }
        MutexInfo _minfo;
    };

}
