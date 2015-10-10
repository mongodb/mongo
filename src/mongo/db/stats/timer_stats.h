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
 * \brief Holds timing information
 *
 * It keeps track of number of times it has been updated and the accumulated
 * time so a diff can be computed.
 */
class TimerStats {
public:
    /**
     * \brief Increments the timing information
     *
     * \param micros The number of microseconds to increment the timing stats
     */
    template <typename DType>
    void recordDuration(DType duration) {
        _num.fetchAndAdd(1);
        _totalMicros.fetchAndAdd(duration_cast<Microseconds>(duration).count());
    }

    /**
     * \brief Increment the timing information using a Timer
     *
     * \param timer The timer to use to increment the timing stats
     * \return The number of microseconds that the timing stats was incremented
     */
    Microseconds record(const Timer& timer);

    /**
     * \brief Generate the BSONObj containing the timing stats information
     *
     * The object returned has 3 fields:
     *   - 'n': contains the number of times the stats were incremented.
     *   - 'totalMillis': the timing information in milliseconds.
     *   - 'totalMicros': the timing information in microseconds.
     *
     * \return The BSONObj with the timing stats
     */
    BSONObj getReport() const;

    operator BSONObj() const {
        return getReport();
    }

private:
    AtomicInt64 _num;
    AtomicInt64 _totalMicros;
};

/**
 * \brief Scope based timing class
 *
 * This class is used to update a TimerStat object based on the time this
 * class is alive.
 * When constructed this class initializes a timer and when the instance is
 * destructed, the TimerStat object associated with it is updated with the
 * number of microseconds the timer has counted.
 */
class TimerHolder {
public:
    /**
     * \brief Constructs a TimerHolder with the TimerStats it needs to update
     *
     * \param stats Reference to the TimerStats object to update
     */
    TimerHolder(TimerStats& stats);

    /**
     * \brief Destroys the instance and updates the TimerStats object if
     * necessary
     */
    ~TimerHolder();

    /**
     * \brief Gets the number of microseconds from the internal timer
     *
     * \return The number of microseconds from the internal timer
     */
    Microseconds micros() const {
        return Microseconds(_t.micros());
    }

    /**
     * \brief Updates the TimerStats object using the internal timer
     *
     * \return The number of microseconds that have been added to the
     *  TimerStats object
     */
    Microseconds recordTimerStats();

private:
    TimerStats& _stats;
    bool _recorded;
    Timer _t;
};

} // namespace mongo
