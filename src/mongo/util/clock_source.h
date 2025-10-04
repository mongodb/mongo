/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/lockable_adapter.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/time_support.h"

#include <type_traits>

namespace mongo {

class Waitable;

/**
 * An interface for getting the current wall clock time.
 */
class MONGO_MOD_OPEN ClockSource {
    // We need a type trait to differentiate waitable ptr args from predicates.
    //
    // This returns true for non-pointers and function pointers
    template <typename PredicateT>
    struct CouldBePredicate : public std::integral_constant<
                                  bool,
                                  !std::is_pointer<PredicateT>::value ||
                                      std::is_function<std::remove_pointer_t<PredicateT>>::value> {
    };

    static constexpr auto kMaxTimeoutForArtificialClocks = Seconds(1);

public:
    /**
     * A StopWatch tracks the time that its ClockSource believes has passed since the creation of
     * the StopWatch or since 'restart' has been invoked.
     *
     * For microsecond accurate metrics, use a Timer instead.
     */
    class StopWatch {
    public:
        StopWatch(ClockSource* clockSource, Date_t start)
            : _clockSource{clockSource}, _start{start} {}
        StopWatch(ClockSource* clockSource) : StopWatch(clockSource, clockSource->now()) {}
        StopWatch(/** SystemClockSource::get() */);

        Date_t now() {
            return _clockSource->now();
        }

        ClockSource* getClockSource() noexcept {
            return _clockSource;
        }

        auto start() const noexcept {
            return _start;
        }

        auto elapsed() {
            return now() - _start;
        }

        auto restart() {
            _start = now();
        }

    private:
        ClockSource* _clockSource;
        Date_t _start;
    };

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
     * Schedules `action` to run sometime after this clock source reaches `when`.
     *
     * Throws `InternalError` if this clock source does not implement `setAlarm`. May also throw
     * other errors.
     */
    virtual void setAlarm(Date_t when, unique_function<void()> action) {
        iasserted({ErrorCodes::InternalError, "This clock source does not implement setAlarm."});
    }

    /**
     * Returns true if this clock source (loosely) tracks the OS clock used for things
     * like condition_variable::wait_until. Virtualized clocks used for testing return
     * false here, and should provide an implementation for setAlarm, above.
     */
    bool tracksSystemClock() const {
        return _tracksSystemClock;
    }

    /**
     * Like cv.wait_until(m, deadline), but uses this ClockSource instead of
     * stdx::chrono::system_clock to measure the passage of time.
     *
     * Note that this can suffer spurious wakeups like cw.wait_until() and, when used with a mocked
     * clock source, may sleep in system time for kMaxTimeoutForArtificialClocks due to unfortunate
     * implementation details.
     */
    stdx::cv_status waitForConditionUntil(stdx::condition_variable& cv,
                                          BasicLockableAdapter m,
                                          Date_t deadline,
                                          Waitable* waitable = nullptr);

    /**
     * Like cv.wait_until(m, deadline, pred), but uses this ClockSource instead of
     * stdx::chrono::system_clock to measure the passage of time.
     */
    template <typename LockT,
              typename PredicateT,
              std::enable_if_t<CouldBePredicate<PredicateT>::value, int> = 0>
    bool waitForConditionUntil(stdx::condition_variable& cv,
                               LockT& m,
                               Date_t deadline,
                               const PredicateT& pred,
                               Waitable* waitable = nullptr) {
        while (!pred()) {
            if (waitForConditionUntil(cv, m, deadline, waitable) == stdx::cv_status::timeout) {
                return pred();
            }
        }
        return true;
    }

    /**
     * Like cv.wait_for(m, duration, pred), but uses this ClockSource instead of
     * stdx::chrono::system_clock to measure the passage of time.
     */
    template <typename LockT,
              typename Duration,
              typename PredicateT,
              std::enable_if_t<CouldBePredicate<PredicateT>::value, int> = 0>
    bool waitForConditionFor(stdx::condition_variable& cv,
                             LockT& m,
                             Duration duration,
                             const PredicateT& pred,
                             Waitable* waitable = nullptr) {
        return waitForConditionUntil(cv, m, now() + duration, pred, waitable);
    }

    /**
     * Return a StopWatch that uses this ClockSource to track time
     */
    StopWatch makeStopWatch() {
        return StopWatch(this);
    }

protected:
    bool _tracksSystemClock = true;
};

}  // namespace mongo
