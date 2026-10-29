// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/condition_variable.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Waitable is a lightweight type that can be used with stdx::condition_variable and can do other
 * work while the condvar 'waits'.
 *
 * It handles this dance by using a special hook that condvar provides to register itself (as a
 * notifiable, which it inherits from) during calls to wait.  Then, rather than actually waiting on
 * the condvar, it invokes its run/run_until methods.
 *
 * The current implementer of Waitable is the transport layer baton type, which performs delayed IO
 * when it would otherwise block.
 *
 * Note that every Waitable should be level-triggered like its base class, Notifiable. See
 * mongo/stdx/condition_variable.h for more details.
 */
class [[MONGO_MOD_OPEN]] Waitable : public Notifiable {
public:
    template <typename LockT>
    static void wait(Waitable* waitable,
                     ClockSource* clkSource,
                     stdx::condition_variable& cv,
                     LockT& lk) {
        if (waitable) {
            cv.waitWithNotifiable(*waitable, [&]() noexcept {
                lk.unlock();
                waitable->run(clkSource);
                lk.lock();
            });
        } else {
            cv.wait(lk);
        }
    }

    template <typename LockT, typename PredicateT>
    static void wait(Waitable* waitable,
                     ClockSource* clkSource,
                     stdx::condition_variable& cv,
                     LockT& lk,
                     PredicateT pred) {
        while (!pred()) {
            wait(waitable, clkSource, cv, lk);
        }
    }

    template <typename LockT>
    static stdx::cv_status wait_until(
        Waitable* waitable,
        ClockSource* clkSource,
        stdx::condition_variable& cv,
        LockT& lk,
        const std::chrono::time_point<std::chrono::system_clock>& timeout_time) {
        if (waitable) {
            auto rval = stdx::cv_status::no_timeout;

            cv.waitWithNotifiable(*waitable, [&]() noexcept {
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

    template <typename LockT, typename PredicateT>
    static bool wait_until(Waitable* waitable,
                           ClockSource* clkSource,
                           stdx::condition_variable& cv,
                           LockT& lk,
                           const std::chrono::time_point<std::chrono::system_clock>& timeout_time,
                           PredicateT pred) {
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
    ~Waitable() = default;
};

}  // namespace mongo
