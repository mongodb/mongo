// timer_stats.h

/*    Copyright 2012 10gen Inc.
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

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/util/timer.h"

namespace mongo {


/**
 * Holds timing information in milliseconds
 * keeps track of number of times and total milliseconds
 * so a diff can be computed
 */
class TimerStats {
public:
    void recordMillis(int millis);

    /**
     * @return number of millis
     */
    int record(const Timer& timer);

    BSONObj getReport() const;
    operator BSONObj() const {
        return getReport();
    }

private:
    AtomicInt64 _num;
    AtomicInt64 _totalMillis;
};

/**
 * Holds an instance of a Timer such that we the time is recorded
 * when the TimerHolder goes out of scope
 */
class TimerHolder {
public:
    /** Destructor will record to TimerStats */
    TimerHolder(TimerStats* stats);
    /** Will record stats if recordMillis hasn't (based on _recorded)  */
    ~TimerHolder();

    /**
     * returns elapsed millis from internal timer
     */
    int millis() const {
        return _t.millis();
    }

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
