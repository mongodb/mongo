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
#include "mongo/util/lockable_adapter.h"
#include "mongo/util/time_support.h"
#include "mongo/util/waitable.h"

namespace mongo {

/**
 * A type which can be used to wait on condition variables with a level triggered one-way interrupt.
 * I.e. after the interrupt is triggered (via some non-public api call) subsequent calls to
 * waitForConditionXXX will fail.  Interrupts must unblock all callers of waitForConditionXXX.
 */
class Interruptible {
protected:
    struct DeadlineState {
        Date_t deadline;
        ErrorCodes::Error error;
        bool hasArtificialDeadline;
    };

    struct IgnoreInterruptsState {
        bool ignoreInterrupts;
        DeadlineState deadline;
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

    /**
     * An interruption guard provides a region where interruption is ignored.
     *
     * Note that this causes the deadline to be reset to Date_t::max(), but that it can also be
     * subsequently reduced in size after the fact.
     */
    class IgnoreInterruptionsGuard {
    public:
        IgnoreInterruptionsGuard(const IgnoreInterruptionsGuard&) = delete;
        IgnoreInterruptionsGuard& operator=(const IgnoreInterruptionsGuard&) = delete;

        IgnoreInterruptionsGuard(IgnoreInterruptionsGuard&& other)
            : _interruptible(other._interruptible), _oldState(other._oldState) {
            other._interruptible = nullptr;
        }

        IgnoreInterruptionsGuard& operator=(IgnoreInterruptionsGuard&&) = delete;

        ~IgnoreInterruptionsGuard() {
            if (_interruptible) {
                _interruptible->popIgnoreInterrupts(_oldState);
            }
        }

    private:
        friend Interruptible;

        explicit IgnoreInterruptionsGuard(Interruptible& interruptible)
            : _interruptible(&interruptible), _oldState(_interruptible->pushIgnoreInterrupts()) {}

        Interruptible* _interruptible;
        IgnoreInterruptsState _oldState;
    };

    IgnoreInterruptionsGuard makeIgnoreInterruptionsGuard() {
        return IgnoreInterruptionsGuard(*this);
    }

public:
    /**
     * Returns a statically allocated instance that cannot be interrupted.  Useful as a default
     * argument to interruptible taking methods.
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
     * Returns the deadline for this interruptible, or Date_t::max() if there is no deadline.
     */
    virtual Date_t getDeadline() const = 0;

    /**
     * Invokes the passed callback with an interruption guard active.  Additionally handles the
     * dance of try/catching the invocation and checking checkForInterrupt with the guard inactive
     * (to allow a higher level timeout to override a lower level one, or for top level interruption
     * to propagate)
     */
    template <typename Callback>
    decltype(auto) runWithoutInterruptionExceptAtGlobalShutdown(Callback&& cb) {
        try {
            const auto guard = makeIgnoreInterruptionsGuard();
            return std::forward<Callback>(cb)();
        } catch (const ExceptionForCat<ErrorCategory::ExceededTimeLimitError>&) {
            // May throw replacement exception
            checkForInterrupt();
            throw;
        }
    }

    /**
     * Raises a AssertionException if this operation is in a killed state.
     */
    void checkForInterrupt() {
        uassertStatusOK(checkForInterruptNoAssert());
    }

    /**
     * Returns Status::OK() unless this operation is in a killed state.
     */
    virtual Status checkForInterruptNoAssert() noexcept = 0;

    /**
     * Waits for either the condition "cv" to be signaled, this operation to be interrupted, or the
     * deadline on this operation to expire.  In the event of interruption or operation deadline
     * expiration, raises a AssertionException with an error code indicating the interruption type.
     */
    template <typename LockT>
    void waitForConditionOrInterrupt(stdx::condition_variable& cv, LockT& m) {
        uassertStatusOK(waitForConditionOrInterruptNoAssert(cv, m));
    }

    /**
     * Waits on condition "cv" for "pred" until "pred" returns true, or this operation
     * is interrupted or its deadline expires. Throws a DBException for interruption and
     * deadline expiration.
     */
    template <typename LockT, typename PredicateT>
    void waitForConditionOrInterrupt(stdx::condition_variable& cv, LockT& m, PredicateT pred) {
        while (!pred()) {
            waitForConditionOrInterrupt(cv, m);
        }
    }

    /**
     * Same as waitForConditionOrInterrupt, except returns a Status instead of throwing
     * a DBException to report interruption.
     */
    template <typename LockT>
    Status waitForConditionOrInterruptNoAssert(stdx::condition_variable& cv, LockT& m) noexcept {
        auto status = waitForConditionOrInterruptNoAssertUntil(cv, m, Date_t::max());
        if (!status.isOK()) {
            return status.getStatus();
        }

        invariant(status.getValue() == stdx::cv_status::no_timeout);
        return Status::OK();
    }

    /**
     * Same as the predicate form of waitForConditionOrInterrupt, except that it returns a not okay
     * status instead of throwing on interruption.
     */
    template <typename LockT, typename PredicateT>
    Status waitForConditionOrInterruptNoAssert(stdx::condition_variable& cv,
                                               LockT& m,
                                               PredicateT pred) noexcept {
        while (!pred()) {
            auto status = waitForConditionOrInterruptNoAssert(cv, m);

            if (!status.isOK()) {
                return status;
            }
        }

        return Status::OK();
    }

    /**
     * Waits for condition "cv" to be signaled, or for the given "deadline" to expire, or
     * for the operation to be interrupted, or for the operation's own deadline to expire.
     *
     * If the operation deadline expires or the operation is interrupted, throws a DBException.  If
     * the given "deadline" expires, returns cv_status::timeout. Otherwise, returns
     * cv_status::no_timeout.
     */
    template <typename LockT>
    stdx::cv_status waitForConditionOrInterruptUntil(stdx::condition_variable& cv,
                                                     LockT& m,
                                                     Date_t deadline) {
        return uassertStatusOK(waitForConditionOrInterruptNoAssertUntil(cv, m, deadline));
    }

    /**
     * Waits on condition "cv" for "pred" until "pred" returns true, or the given "deadline"
     * expires, or this operation is interrupted, or this operation's own deadline expires.
     *
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
        while (!pred()) {
            if (stdx::cv_status::timeout == waitForConditionOrInterruptUntil(cv, m, deadline)) {
                return pred();
            }
        }
        return true;
    }

    /**
     * Same as the non-predicate form of waitForConditionOrInterruptUntil, but takes a relative
     * amount of time to wait instead of an absolute time point.
     */
    template <typename LockT>
    stdx::cv_status waitForConditionOrInterruptFor(stdx::condition_variable& cv,
                                                   LockT& m,
                                                   Milliseconds ms) {
        return uassertStatusOK(
            waitForConditionOrInterruptNoAssertUntil(cv, m, getExpirationDateForWaitForValue(ms)));
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
        const auto deadline = getExpirationDateForWaitForValue(ms);
        while (!pred()) {
            if (stdx::cv_status::timeout == waitForConditionOrInterruptUntil(cv, m, deadline)) {
                return pred();
            }
        }
        return true;
    }

    /**
     * Same as waitForConditionOrInterruptUntil, except returns StatusWith<stdx::cv_status> and
     * non-ok status indicates the error instead of a DBException.
     */
    virtual StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept = 0;

    /**
     * Sleeps until "deadline"; throws an exception if the interruptible is interrupted before then.
     */
    void sleepUntil(Date_t deadline) {
        auto m = MONGO_MAKE_LATCH();
        stdx::condition_variable cv;
        stdx::unique_lock<Latch> lk(m);
        invariant(!waitForConditionOrInterruptUntil(cv, lk, deadline, [] { return false; }));
    }

    /**
     * Sleeps for "duration" ms; throws an exception if the interruptible is interrupted before
     * then.
     */
    void sleepFor(Milliseconds duration) {
        auto m = MONGO_MAKE_LATCH();
        stdx::condition_variable cv;
        stdx::unique_lock<Latch> lk(m);
        invariant(!waitForConditionOrInterruptFor(cv, lk, duration, [] { return false; }));
    }

protected:
    /**
     * Pushes an ignore interruption critical section into the interruptible.  Until an associated
     * popIgnoreInterrupts is invoked, the interruptible should ignore interruptions related to
     * explicit interruption or previously set deadlines.
     *
     * Note that new deadlines can be set after this is called, which will again introduce the
     * possibility of interruption.
     *
     * Returns state needed to pop interruption.
     */
    virtual IgnoreInterruptsState pushIgnoreInterrupts() = 0;

    /**
     * Pops the ignored interruption critical section introduced by push.
     */
    virtual void popIgnoreInterrupts(IgnoreInterruptsState iis) = 0;

    /**
     * Pushes a subsidiary deadline into the interruptible.  Until an associated
     * popArtificialDeadline is
     * invoked, the interruptible will fail checkForInterrupt and waitForConditionOrInterrupt calls
     * with the passed error code if the deadline has passed.
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
     * Returns the equivalent of Date_t::now() + waitFor for the interruptible's clock
     */
    virtual Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) = 0;

    class NotInterruptible;
};

/**
 * A not interruptible type which can be used as a lightweight default arg for interruptible taking
 * functions.
 */
class Interruptible::NotInterruptible final : public Interruptible {
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept override {

        if (deadline == Date_t::max()) {
            cv.wait(m);
            return stdx::cv_status::no_timeout;
        }

        return cv.wait_until(m, deadline.toSystemTimePoint());
    }

    Date_t getDeadline() const override {
        return Date_t::max();
    }

    Status checkForInterruptNoAssert() noexcept override {
        return Status::OK();
    }

    // It's invalid to call the deadline or ignore interruption guards on a possibly noop
    // interruptible.
    //
    // The noop interruptible should only be invoked as a default arg at the bottom of the call
    // stack (with types that won't modify it's invocation)
    IgnoreInterruptsState pushIgnoreInterrupts() override {
        MONGO_UNREACHABLE;
    }

    void popIgnoreInterrupts(IgnoreInterruptsState) override {
        MONGO_UNREACHABLE;
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
