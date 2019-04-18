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

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Waitable is a lightweight type that can be used with stdx::condition_variable and can do other
 * work while the condvar 'waits'.
 *
 * It handles this dance by using a special hook that condvar provides to register itself (as a
 * notifyable, which it inherits from) during calls to wait.  Then, rather than actually waiting on
 * the condvar, it invokes its run/run_until methods.
 *
 * The current implementer of Waitable is the transport layer baton type, which performs delayed IO
 * when it would otherwise block.
 */
class Waitable : public Notifyable {
public:
    static void wait(Waitable* waitable,
                     ClockSource* clkSource,
                     stdx::condition_variable& cv,
                     stdx::unique_lock<stdx::mutex>& lk) {
        if (waitable) {
            cv._runWithNotifyable(*waitable, [&]() noexcept {
                lk.unlock();
                waitable->run(clkSource);
                lk.lock();
            });
        } else {
            cv.wait(lk);
        }
    }

    template <typename Predicate>
    static void wait(Waitable* waitable,
                     ClockSource* clkSource,
                     stdx::condition_variable& cv,
                     stdx::unique_lock<stdx::mutex>& lk,
                     Predicate pred) {
        while (!pred()) {
            wait(waitable, clkSource, cv, lk);
        }
    }

    static stdx::cv_status wait_until(
        Waitable* waitable,
        ClockSource* clkSource,
        stdx::condition_variable& cv,
        stdx::unique_lock<stdx::mutex>& lk,
        const stdx::chrono::time_point<stdx::chrono::system_clock>& timeout_time) {
        if (waitable) {
            auto rval = stdx::cv_status::no_timeout;

            cv._runWithNotifyable(*waitable, [&]() noexcept {
                lk.unlock();
                if (waitable->run_until(clkSource, Date_t(timeout_time)) == TimeoutState::Timeout) {
                    rval = stdx::cv_status::timeout;
                }
                lk.lock();
            });

            return rval;
        } else {
            return cv.wait_until(lk, timeout_time);
        }
    }

    template <typename Predicate>
    static bool wait_until(Waitable* waitable,
                           ClockSource* clkSource,
                           stdx::condition_variable& cv,
                           stdx::unique_lock<stdx::mutex>& lk,
                           const stdx::chrono::time_point<stdx::chrono::system_clock>& timeout_time,
                           Predicate pred) {
        while (!pred()) {
            if (wait_until(waitable, clkSource, cv, lk, timeout_time) == stdx::cv_status::timeout) {
                return pred();
            }
        }

        return true;
    }

    enum class TimeoutState {
        NoTimeout,
        Timeout,
    };

    /**
     * Run some amount of work.  The intention is that this function perform work until it's
     * possible that the surrounding condvar clause could have finished.
     *
     * Note that like regular condvar.wait, this allows implementers the flexibility to possibly
     * return early.
     *
     * We take a clock source here to allow for synthetic timeouts.
     */
    virtual void run(ClockSource* clkSource) noexcept = 0;

    /**
     * Like run, but only until the passed deadline has passed.
     */
    virtual TimeoutState run_until(ClockSource* clkSource, Date_t deadline) noexcept = 0;

protected:
    ~Waitable() noexcept {}
};

}  // namespace mongo
