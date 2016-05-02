// @file timer.h

/*    Copyright 2010 10gen Inc.
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

#include "mongo/util/time_support.h"


namespace mongo {

class TickSource;

/**
 * Time tracking object.
 */
class Timer /*copyable*/ {
public:
    /**
     * Creates a timer with the system default tick source. Should not be created before
     * global initialization completes.
     */
    Timer();

    /**
     * Creates a timer using the specified tick source. Caller retains ownership of
     * TickSource, and must keep it in scope until Timer goes out of scope.
     */
    explicit Timer(TickSource* tickSource);

    int seconds() const {
        return static_cast<int>(micros() / 1000000);
    }
    int millis() const {
        return static_cast<int>(micros() / 1000);
    }
    int minutes() const {
        return seconds() / 60;
    }

    /** Get the time interval and reset at the same time.
     *  @return time in milliseconds.
     */
    inline int millisReset() {
        const long long nextNow = now();
        const long long deltaMicros = static_cast<long long>((nextNow - _old) * _microsPerCount);

        _old = nextNow;
        return static_cast<int>(deltaMicros / 1000);
    }

    inline long long micros() const {
        return static_cast<long long>((now() - _old) * _microsPerCount);
    }

    Microseconds elapsed() const {
        return Microseconds{micros()};
    }

    inline void reset() {
        _old = now();
    }

private:
    TickSource* const _tickSource;

    // Derived value from _countsPerSecond. This represents the conversion ratio
    // from clock ticks to microseconds.
    const double _microsPerCount;

    long long now() const;

    long long _old;
};
}  // namespace mongo
