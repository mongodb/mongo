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
#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
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
 * A type which can be used to wait on condition variables with a level triggered one-way interrupt.
 * I.e. after the interrupt is triggered (via some non-public api call) subsequent calls to
 * waitForConditionXXX will fail.  Interrupts must unblock all callers of waitForConditionXXX.
 *
 * This class should never be derived from directly. Instead, please derive from Interruptible.
 */
class InterruptibleBase {
public:
    virtual ~InterruptibleBase() = default;

    /**
     * Returns the deadline for this InterruptibleBase, or Date_t::max() if there is no deadline.
     */
    virtual Date_t getDeadline() const = 0;

    /**
     * Returns Status::OK() unless this operation is in a killed state.
     */
    virtual Status checkForInterruptNoAssert() noexcept = 0;

protected:
    /**
     * Same as Interruptible::waitForConditionOrInterruptUntil(), except returns
     * StatusWith<stdx::cv_status> and a non-ok status indicates the error instead of a DBException.
     */
    virtual StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept = 0;

    struct DeadlineState {
        Date_t deadline;
        ErrorCodes::Error error;
        bool hasArtificialDeadline;
    };

    /**
     * Pushes a subsidiary deadline into the InterruptibleBase. Until an associated
     * popArtificialDeadline() is invoked, the InterruptibleBase will fail checkForInterrupt() and
     * waitForConditionOrInterrupt() calls with the passed error code if the deadline has passed.
     *
     * Note that deadline's higher than the current value are constrained (such that the passed
     * error code will be returned/thrown, but after the min(oldDeadline, newDeadline) has passed).
     *
     * Returns state needed to pop the deadline.
     */
    virtual DeadlineState pushArtificialDeadline(Date_t deadline, ErrorCodes::Error error) = 0;

    /**
     * Pops the subsidiary deadline introduced by push.
     */
    virtual void popArtificialDeadline(DeadlineState) = 0;

    /**
     * Returns the equivalent of Date_t::now() + waitFor for the InterruptibleBase's clock
     */
    virtual Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) = 0;
};

/**
 * An derived class of InterruptibleBase which provides a variety of helper functions
 *
 * Please derive from this class instead of InterruptibleBase.
 */
class Interruptible : public InterruptibleBase {
private:
    /**
     * Helper class to properly set _isWaiting for Interruptible instances.
     * Every call sequence that waits on a condition/interrupt must hold an instance of WaitContext.
     */
    class WaitContext {
    public:
        WaitContext(Interruptible* interruptible) : _interruptible(interruptible) {
            _interruptible->_isWaiting.store(true);
        }

        ~WaitContext() {
            _interruptible->_isWaiting.store(false);
        }

    private:
        Interruptible* const _interruptible;
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

    private:
        friend Interruptible;

        explicit DeadlineGuard(Interruptible& interruptible,
                               Date_t newDeadline,
                               ErrorCodes::Error error)
            : _interruptible(&interruptible),
              _oldDeadline(_interruptible->pushArtificialDeadline(newDeadline, error)) {}

        Interruptible* _interruptible;
        DeadlineState _oldDeadline;
    };

    DeadlineGuard makeDeadlineGuard(Date_t deadline, ErrorCodes::Error error) {
        return DeadlineGuard(*this, deadline, error);
    }

public:
    class WaitListener;

    Interruptible() : _isWaiting(false) {}

    /**
     * Returns true if currently waiting for a condition/interrupt.
     * This function relies on instances of WaitContext to properly set _isWaiting.
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
        kInterrupt,
    };

    static constexpr auto kFastWakeTimeout = Milliseconds(100);

    /**
     * Enum to convey if an Interruptible woke up before or after kFastWakeTimeout
     */
    enum class WakeSpeed {
        kFast,
        kSlow,
    };

    /**
     * This function adds a WaitListener subclass to the triggers for certain actions.
     *
     * WaitListeners can only be added and not removed. If you wish to deactivate a WaitListeners
     * subclass, please provide the switch on that subclass to noop its functions. It is only safe
     * to add a WaitListener during a MONGO_INITIALIZER.
     */
    template <typename WaitListenerT>
    static void installWaitListener();

    /**
     * Returns a statically allocated instance that cannot be interrupted.  Useful as a default
     * argument to Interruptible-taking methods.
     */
    static Interruptible* notInterruptible();

    /**
     * Invokes the passed callback with a deadline guard active initialized with the passed
     * deadline.  Additionally handles the dance of try/catching the invocation and checking
     * checkForInterrupt with the guard inactive (to allow a higher level timeout to override a
     * lower level one)
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
     * Get the name for a Latch
     */
    template <typename LatchT>
    requires std::is_base_of_v<latch_detail::Latch, LatchT>
    static StringData getLatchName(const stdx::unique_lock<LatchT>& lk) {
        return lk.mutex()->getName();
    }

    /**
     * Get a placeholder name for an arbitrary type
     */
    template <typename LockT>
    static constexpr StringData getLatchName(const LockT&) {
        return "AnonymousLockable"_sd;
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
                                          Date_t finalDeadline,
                                          PredicateT pred) {
        WaitContext waitContext(this);
        auto latchName = getLatchName(m);

        auto handleInterruptAndAssert = [&](Status status, WakeSpeed speed) {
            _onWake(latchName, WakeReason::kInterrupt, speed);
            iassert(std::move(status));
        };

        auto checkForInterruptWithoutLockAndAssert = [&](WakeSpeed speed) {
            // We drop the lock before checking for interrupt since checkForInterruptNoAssert can
            // sometimes try to reacquire the same lock.
            if (auto status = interruptible_detail::doWithoutLock(
                    m, [&] { return checkForInterruptNoAssert(); });
                !status.isOK()) {
                handleInterruptAndAssert(status, speed);
            }
        };

        auto waitUntil = [&](Date_t deadline, WakeSpeed speed) -> boost::optional<WakeReason> {
            // If the result of waitForConditionOrInterruptNoAssertUntil() is non-spurious, return
            // a WakeReason. Otherwise, return boost::none

            auto swResult = waitForConditionOrInterruptNoAssertUntil(cv, m, deadline);
            if (!swResult.isOK()) {
                handleInterruptAndAssert(swResult.getStatus(), speed);
            }

            // Check if an interrupt occurred while waiting.
            checkForInterruptWithoutLockAndAssert(speed);

            // Check the predicate after re-acquiring the lock.
            if (pred()) {
                _onWake(latchName, WakeReason::kPredicate, speed);
                return WakeReason::kPredicate;
            }

            if (swResult.getValue() == stdx::cv_status::timeout) {
                _onWake(latchName, WakeReason::kTimeout, speed);
                return WakeReason::kTimeout;
            }

            return boost::none;
        };

        auto waitUntilNonSpurious = [&](Date_t deadline, WakeSpeed speed) -> WakeReason {
            // Check for interrupt before waiting.
            checkForInterruptWithoutLockAndAssert(speed);

            // Check the predicate after re-acquiring the lock and before waiting.
            if (pred()) {
                _onWake(latchName, WakeReason::kPredicate, speed);
                return WakeReason::kPredicate;
            }

            // Check waitUntil() in a loop until it says it has a genuine WakeReason
            auto maybeWakeReason = waitUntil(deadline, speed);
            while (!maybeWakeReason) {
                maybeWakeReason = waitUntil(deadline, speed);
            };

            return *maybeWakeReason;
        };

        const auto traceDeadline = getExpirationDateForWaitForValue(kFastWakeTimeout);
        const auto firstDeadline = std::min(traceDeadline, finalDeadline);

        // Wait for the first deadline
        if (auto wakeReason = waitUntilNonSpurious(firstDeadline, WakeSpeed::kFast);
            wakeReason == WakeReason::kPredicate) {
            // If our first wait fulfilled our predicate then return true
            return true;
        } else if (firstDeadline == finalDeadline) {
            // If we didn't fulfill our predicate but finalDeadline was less than traceDeadline,
            // then the wait should return false
            return false;
        }

        _onLongSleep(latchName);

        // Wait for the final deadline
        if (auto wakeReason = waitUntilNonSpurious(finalDeadline, WakeSpeed::kSlow);
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
        auto m = MONGO_MAKE_LATCH();
        stdx::condition_variable cv;
        stdx::unique_lock<Latch> lk(m);
        invariant(!waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }));
    }

    /**
     * Sleeps for "duration" ms; throws an exception if the Interruptible is interrupted before
     * then.
     */
    void sleepFor(Milliseconds duration) {
        auto m = MONGO_MAKE_LATCH();
        stdx::condition_variable cv;
        stdx::unique_lock<Latch> lk(m);
        invariant(!waitForConditionOrInterruptFor(cv, lk, duration, [] { return false; }));
    }

protected:
    class NotInterruptible;

    void _onLongSleep(StringData name);
    void _onWake(StringData name, WakeReason reason, WakeSpeed speed);

    static auto& _getListenerState() {
        struct State {
            std::vector<WaitListener*> list;
        };

        // Note that state should no longer be mutated after init-time (ala MONGO_INITIALIZERS). If
        // this changes, than this state needs to be synchronized.
        static State state;
        return state;
    }

private:
    AtomicWord<bool> _isWaiting;
};

/**
 * A set of event handles for an Interruptible type
 */
class Interruptible::WaitListener {
public:
    /**
     * Action to do when a wait does not resolve quickly
     *
     * Any implementation of this function must be safe to invoke when an Interruptible-associated
     * latch is held. As this is hard to reason about, avoid external latches whenever possible.
     */
    virtual void onLongSleep(StringData name) = 0;

    /**
     * Action to do when a wait resolves after a sleep
     *
     * Any implementation of this function must be safe to invoke when an Interruptible-associated
     * latch is held. As this is hard to reason about, avoid external latches whenever possible.
     */
    virtual void onWake(StringData name, WakeReason reason, WakeSpeed speed) = 0;
};

template <typename WaitListenerT>
inline void Interruptible::installWaitListener() {
    auto& state = _getListenerState();

    static auto* listener = new WaitListenerT();  // Intentionally leaked!
    state.list.push_back(listener);
}

inline void Interruptible::_onLongSleep(StringData name) {
    auto& state = _getListenerState();

    for (auto listener : state.list) {
        listener->onLongSleep(name);
    }
}

inline void Interruptible::_onWake(StringData name, WakeReason reason, WakeSpeed speed) {
    auto& state = _getListenerState();

    for (auto listener : state.list) {
        listener->onWake(name, reason, speed);
    }
}

/**
 * A non-interruptible type which can be used as a lightweight default arg for Interruptible-taking
 * functions.
 */
class Interruptible::NotInterruptible final : public Interruptible {
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
