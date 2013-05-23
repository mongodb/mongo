// timer_stats.cpp

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

#include "mongo/db/stats/timer_stats.h"

namespace mongo {

    TimerHolder::TimerHolder( TimerStats* stats )
        : _stats( stats ), _recorded( false ){
    }

    TimerHolder::~TimerHolder() {
        if ( ! _recorded ) {
            recordMillis();
        }
    }

    int TimerHolder::recordMillis() {
        _recorded = true;
        if ( _stats ) {
            return _stats->record( _t );
        }
        return _t.millis();
    }

    void TimerStats::recordMillis( int millis ) {
        scoped_spinlock lk( _lock );
        _num++;
        _totalMillis += millis;
    }

    int TimerStats::record( const Timer& timer ) {
        int millis = timer.millis();
        recordMillis( millis );
        return millis;
    }

    BSONObj TimerStats::getReport() const {
        long long n, t;
        {
            scoped_spinlock lk( _lock );
            n = _num;
            t = _totalMillis;
        }
        BSONObjBuilder b(64);
        b.appendNumber( "num", n );
        b.appendNumber( "totalMillis" , t );
        return b.obj();

    }
}
