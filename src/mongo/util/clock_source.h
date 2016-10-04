/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#pragma once

#include "mongo/stdx/functional.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Date_t;

/**
 * An interface for getting the current wall clock time.
 */
class ClockSource {
public:
    virtual ~ClockSource() = default;

    /**
     * Returns the minimum time change that the clock can describe.
     */
    virtual Milliseconds getPrecision() = 0;

    /**
     * Returns the current wall clock time, as defined by this source.
     */
    virtual Date_t now() = 0;

    /**
     * Schedules "action" to run sometime after this clock source reaches "when".
     *
     * Returns InternalError if this clock source does not implement setAlarm. May also
     * return ShutdownInProgress during shutdown. Other errors are also allowed.
     */
    virtual Status setAlarm(Date_t when, stdx::function<void()> action) {
        return {ErrorCodes::InternalError, "This clock source does not implement setAlarm."};
    }

    /**
     * Returns true if this clock source (loosely) tracks the OS clock used for things
     * like condition_variable::wait_until. Virtualized clocks used for testing return
     * false here, and should provide an implementation for setAlarm, above.
     */
    bool tracksSystemClock() const {
        return _tracksSystemClock;
    }

protected:
    bool _tracksSystemClock = true;
};

}  // namespace mongo
