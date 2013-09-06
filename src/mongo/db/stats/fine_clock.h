// fine_clock.h

/**
*    Copyright (C) 2008 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#ifndef DB_STATS_FINE_CLOCK_HEADER
#define DB_STATS_FINE_CLOCK_HEADER

#include <time.h>  // struct timespec

namespace mongo {

    /**
     * This is a nano-second precision clock. We're skipping the
     * harware TSC in favor of clock_gettime() which in some systems
     * does not involve a trip to the OS (VDSO).
     *
     * We're exporting a type WallTime that is and should remain
     * opaque. The business of getting accurate time is still ongoing
     * and we may change the internal representation of this class.
     * (http://lwn.net/Articles/388188/)
     *
     * Really, you shouldn't be using this class in hot code paths for
     * platforms you're not sure whether the overhead is low.
     */
    class FineClock {
    public:

        typedef timespec WallTime;

        static WallTime now() {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return ts;
        }

        static uint64_t diffInNanos( WallTime end, WallTime start ) {
            uint64_t diff;
            if ( end.tv_nsec < start.tv_nsec ) {
                diff = 1000000000 * ( end.tv_sec - start.tv_sec - 1);
                diff += 1000000000 + end.tv_nsec - start.tv_nsec;
            }
            else {
                diff = 1000000000 * ( end.tv_sec - start.tv_sec );
                diff += end.tv_nsec - start.tv_nsec;
            }
            return diff;
        }

    };
}

#endif  // DB_STATS_FINE_CLOCK_HEADER

