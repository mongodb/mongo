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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <mutex>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/lockable_adapter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

namespace mongo {

namespace interruptible_detail {
// Helper to release a lock, call a callable, and then reacquire the lock.
template <typename Callable>
auto doWithoutLock(BasicLockableAdapter m, Callable&& callable) {
    m.unlock();
    ON_BLOCK_EXIT([&] { m.lock(); });
    return callable();
}
}  // namespace interruptible_detail

/**
 * A type which can be used to perform an interruptible wait on a stdx::condition_variable.
 *
 * The interrupt is a one-time event; after the interrupt is triggered (via some non-public api
 * call), _all_ subsequent calls to waitForConditionXXX will fail. Interrupts must unblock all
 * callers of waitForConditionXXX.
 */
class Interruptible {
public:
    /**
     * Returns true if currently waiting for a condition/interrupt.
     * This function relies on waitForConditionOrInterrupt to properly set _isWaiting.
     * Note that _isWaiting remains true until waitForConditionOrInterruptUntil() returns.
     */
    bool isWaitingForConditionOrInterrupt() const {
        return _isWaiting.loadRelaxed();
    }

    /**
     * Enum to convey why an Interruptible woke up
     */
    enum class WakeReason {
        kPredicate,
        kTimeout,
    };

    /**
     * Returns a statically allocated instance that cannot be interrupted.  Useful as a default
     * argument to Interruptible-taking methods.
     */
    static Interruptible* notInterruptible();

    /**
     * Invokes the passed callback with a deadline guard active initialized with the passed
     * deadline.  Additionally handles the dance of try/catching the invocation and checking
     * checkForInterrupt with the guard inactive, to allow an interruption or timeout error _not_
     * caused by the provided deadline to propogate out / take precedence over an error produced
     * due to the provided deadline.
     */
    template <typename Callback>
    decltype(auto) runWithDeadline(Date_t deadline, ErrorCodes::Error error, Callback&& cb) {
        invariant(ErrorCodes::isExceededTimeLimitError(error));

        try {
            const auto guard = makeDeadlineGuard(deadline, error);
            return std::forward<Callback>(cb)();
        } catch (const ExceptionForCat<ErrorCategory::ExceededTimeLimitError>&) {
            // May throw replacement exception
            checkForInterrupt();
            throw;
        }
    }

    bool hasDeadline() const {
        return getDeadline() != Date_t::max();
    }

    /**
     * Raises a AssertionException if this operation is in a killed state.
     */
    void checkForInterrupt() {
        iassert(checkForInterruptNoAssert());
    }

    /**
     * Waits on condition "cv" for "pred" until "pred" returns true, or the given "deadline"
     * expires, or this operation is interrupted, or this operation's own deadline expires.
     *
     * If the operation deadline expires or the operation is interrupted, throws a DBException.  If
     * the given "deadline" expires, returns cv_status::timeout. Otherwise, returns
     * cv_status::no_timeout indicating that "pred" finally returned true.
     */
    template <typename LockT, typename PredicateT>
    bool waitForConditionOrInterruptUntil(stdx::condition_variable& cv,
                                          LockT& m,
                                          Date_t deadline,
                                          PredicateT pred) {
        _isWaiting.store(true);
        ON_BLOCK_EXIT([this] { _isWaiting.store(false); });

        auto checkForInterruptWithoutLockAndAssert = [&]() {
            // We drop the lock before checking for interrupt since checkForInterruptNoAssert can
            // sometimes try to reacquire the same lock.
            if (auto status = interruptible_detail::doWithoutLock(
                    m, [&] { return checkForInterruptNoAssert(); });
                !status.isOK()) {
                iassert(std::move(status));
            }
        };

        auto waitUntilOptionalWakeReason = [&](Date_t deadline) -> boost::optional<WakeReason> {
            // If the result of waitForConditionOrInterruptNoAssertUntil() is non-spurious, return
            // a WakeReason. Otherwise, return boost::none

            auto swResult = waitForConditionOrInterruptNoAssertUntil(cv, m, deadline);
            iassert(swResult);

            // Check if an interrupt occurred while waiting.
            checkForInterruptWithoutLockAndAssert();

            // Check the predicate after re-acquiring the lock.
            if (pred()) {
                return WakeReason::kPredicate;
            }

            if (swResult.getValue() == stdx::cv_status::timeout) {
                return WakeReason::kTimeout;
            }

            return boost::none;
        };

        auto waitUntilRealWakeReason = [&](Date_t deadline) -> WakeReason {
            // Check for interrupt before waiting.
            checkForInterruptWithoutLockAndAssert();

            // Check the predicate after re-acquiring the lock and before waiting.
            if (pred()) {
                return WakeReason::kPredicate;
            }

            // Check waitUntilOptionalWakeReason() in a loop until it says it has a genuine
            // WakeReason
            auto maybeWakeReason = waitUntilOptionalWakeReason(deadline);
            while (!maybeWakeReason) {
                maybeWakeReason = waitUntilOptionalWakeReason(deadline);
            };

            return *maybeWakeReason;
        };

        if (auto wakeReason = waitUntilRealWakeReason(deadline);
            wakeReason == WakeReason::kPredicate) {
            return true;
        } else {
            return false;
        }
    }

    /**
     * Waits on condition "cv" for "pred" until "pred" returns true, or this operation
     * is interrupted or its deadline expires. Throws a DBException for interruption and
     * deadline expiration.
     */
    template <typename LockT, typename PredicateT>
    void waitForConditionOrInterrupt(stdx::condition_variable& cv, LockT& m, PredicateT pred) {
        waitForConditionOrInterruptUntil(cv, m, Date_t::max(), std::move(pred));
    }

    /**
     * Same as the predicate form of waitForConditionOrInterruptUntil, but takes a relative
     * amount of time to wait instead of an absolute time point.
     */
    template <typename LockT, typename PredicateT>
    bool waitForConditionOrInterruptFor(stdx::condition_variable& cv,
                                        LockT& m,
                                        Milliseconds ms,
                                        PredicateT pred) {
        return waitForConditionOrInterruptUntil(
            cv, m, getExpirationDateForWaitForValue(ms), std::move(pred));
    }

    /**
     * Sleeps until "deadline"; throws an exception if the Interruptible is interrupted before then.
     */
    void sleepUntil(Date_t deadline) {
        stdx::mutex m;
        stdx::condition_variable cv;
        stdx::unique_lock<stdx::mutex> lk(m);
        invariant(!waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }));
    }

    /**
     * Sleeps for "duration" ms; throws an exception if the Interruptible is interrupted before
     * then.
     */
    void sleepFor(Milliseconds duration) {
        stdx::mutex m;
        stdx::condition_variable cv;
        stdx::unique_lock<stdx::mutex> lk(m);
        invariant(!waitForConditionOrInterruptFor(cv, lk, duration, [] { return false; }));
    }

    struct DeadlineState {
        Date_t deadline;
        ErrorCodes::Error error;
        bool hasArtificialDeadline;
    };

    /**
     * A deadline guard provides a subsidiary deadline to the parent.
     */
    class DeadlineGuard {
    public:
        DeadlineGuard(const DeadlineGuard&) = delete;
        DeadlineGuard& operator=(const DeadlineGuard&) = delete;

        DeadlineGuard(DeadlineGuard&& other)
            : _interruptible(other._interruptible), _oldDeadline(other._oldDeadline) {
            other._interruptible = nullptr;
        }

        DeadlineGuard& operator=(DeadlineGuard&& other) = delete;

        ~DeadlineGuard() {
            if (_interruptible) {
                _interruptible->popArtificialDeadline(_oldDeadline);
            }
        }

        DeadlineGuard(Interruptible& interruptible, Date_t newDeadline, ErrorCodes::Error error)
            : _interruptible(&interruptible),
              _oldDeadline(_interruptible->pushArtificialDeadline(newDeadline, error)) {}

        Interruptible* _interruptible;
        DeadlineState _oldDeadline;
    };

    DeadlineGuard makeDeadlineGuard(Date_t deadline, ErrorCodes::Error error) {
        return DeadlineGuard(*this, deadline, error);
    }

    /**
     * Same as Interruptible::waitForConditionOrInterruptUntil(), except returns
     * StatusWith<stdx::cv_status> and a non-ok status indicates the error instead of a DBException.
     *
     * The inheritor of Interruptible must provide this core implementation, which the remainder of
     * the waitForConditionOrInterrupt... family of functions delegate to. The function should
     * return with an appropriate status when `cv` is notified, `deadline` expires, or the
     * Interruptible is interrupted.
     */
    virtual StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept = 0;

    virtual Date_t getDeadline() const = 0;

    virtual Status checkForInterruptNoAssert() noexcept = 0;

    /**
     * Pushes a subsidiary deadline into the Interruptible. Until an associated
     * popArtificialDeadline() is invoked, the Interruptible will fail checkForInterrupt() and
     * waitForConditionOrInterrupt() calls with the passed error code if the deadline has passed.
     *
     * If a deadline later than the Interruptible's current deadline is provided, the provided
     * error code will be used, but after the earlier of the existing deadline and new deadline has
     * passed. In other words, this function cannot be used to 'extend' the current deadline of an
     * interruptible, only to temporarily 'shorten' it or replace the error code used.
     *
     * Returns state needed to pop the deadline.
     */
    virtual DeadlineState pushArtificialDeadline(Date_t deadline, ErrorCodes::Error error) = 0;

    /**
     * Pops the subsidiary deadline introduced by push.
     */
    virtual void popArtificialDeadline(DeadlineState) = 0;

    /**
     * Returns the equivalent of Date_t::now() + waitFor for the Interruptible's clock
     */
    virtual Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) = 0;


private:
    AtomicWord<bool> _isWaiting{false};
};

/**
 * A non-interruptible type which can be used as a lightweight default arg for Interruptible-taking
 * functions.
 */
class NotInterruptible final : public Interruptible {
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept override {

        if (deadline == Date_t::max()) {
            cv.wait(m);
            return stdx::cv_status::no_timeout;
        }

        try {
            // If the system clock's time_point's compiler-dependent resolution is higher than
            // Date_t's milliseconds, it's possible for the conversion from Date_t to time_point
            // to overflow and trigger an exception. We catch that here to maintain the noexcept
            // contract.
            return cv.wait_until(m, deadline.toSystemTimePoint());
        } catch (const ExceptionFor<ErrorCodes::DurationOverflow>& ex) {
            return ex.toStatus();
        }
    }

    Date_t getDeadline() const override {
        return Date_t::max();
    }

    Status checkForInterruptNoAssert() noexcept override {
        return Status::OK();
    }

    DeadlineState pushArtificialDeadline(Date_t deadline, ErrorCodes::Error error) override {
        MONGO_UNREACHABLE;
    }

    void popArtificialDeadline(DeadlineState) override {
        MONGO_UNREACHABLE;
    }

    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) override {
        return Date_t::now() + waitFor;
    }
};

inline Interruptible* Interruptible::notInterruptible() {
    thread_local static NotInterruptible notInterruptible{};

    return &notInterruptible;
}

}  // namespace mongo
