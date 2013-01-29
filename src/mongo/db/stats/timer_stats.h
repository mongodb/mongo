// timer_stats.h

/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/timer.h"

namespace mongo {


    /**
     * Holds timing information in milliseconds
     * keeps track of number of times and total milliseconds
     * so a diff can be computed
     */
    class TimerStats {
    public:
        void recordMillis( int millis );

        /**
         * @return number of millis
         */
        int record( const Timer& timer );

        BSONObj getReport() const;
        operator BSONObj() const { return getReport(); }

    private:
        mutable SpinLock _lock;
        long long _num;
        long long _totalMillis;
    };

    /**
     * Holds an instance of a Timer such that we the time is recorded
     * when the TimerHolder goes out of scope
     */
    class TimerHolder {
    public:
        /** Destructor will record to TimerStats */
        TimerHolder( TimerStats* stats );
        /** Will record stats if recordMillis hasn't (based on _recorded)  */
        ~TimerHolder();

        /**
         * returns elapsed millis from internal timer
         */
        int millis() const { return _t.millis(); }

        /**
         * records the time in the TimerStats and marks that we've
         * already recorded so the destructor doesn't
         */
        int recordMillis();

    private:
        TimerStats* _stats;
        bool _recorded;
        Timer _t;
    };
}
