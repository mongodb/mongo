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
#include "mongo/platform/basic.h"

#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

class Mutex;

class ConditionVariableActions {
public:
    virtual ~ConditionVariableActions() = default;
    virtual void onUnfulfilledConditionVariable(const StringData& name) = 0;
    virtual void onFulfilledConditionVariable() = 0;
};

class ConditionVariable {
    friend class ::mongo::Waitable;

public:
    static constexpr Milliseconds kUnfulfilledConditionVariableTimeout = Milliseconds(100);

    template <class Lock>
    void wait(Lock& lock);

    template <class Lock, class Predicate>
    void wait(Lock& lock, Predicate pred);

    template <class Lock, class Rep, class Period>
    stdx::cv_status wait_for(Lock& lock, const stdx::chrono::duration<Rep, Period>& rel_time);

    template <class Lock, class Rep, class Period, class Predicate>
    bool wait_for(Lock& lock, const stdx::chrono::duration<Rep, Period>& rel_time, Predicate pred);

    template <class Lock, class Clock, class Duration>
    stdx::cv_status wait_until(Lock& lock,
                               const stdx::chrono::time_point<Clock, Duration>& timeout_time);

    template <class Lock, class Clock, class Duration, class Predicate>
    bool wait_until(Lock& lock,
                    const stdx::chrono::time_point<Clock, Duration>& timeout_time,
                    Predicate pred);

    void notify_one() noexcept;
    void notify_all() noexcept;

    static void setConditionVariableActions(std::unique_ptr<ConditionVariableActions> actions);

protected:
    template <typename Callback>
    void _runWithNotifyable(Notifyable& notifyable, Callback&& cb) noexcept {
        _condvar._runWithNotifyable(notifyable, cb);
    }

private:
    const Seconds _conditionVariableTimeout = Seconds(604800);
    stdx::condition_variable_any _condvar;

    inline static std::unique_ptr<ConditionVariableActions> _conditionVariableActions;

    template <class Lock, class Duration>
    auto _wait(Lock& lock, const Duration& rel_time) {
        const auto guard = makeGuard([&] {
            if (_conditionVariableActions) {
                _conditionVariableActions->onFulfilledConditionVariable();
            }
        });

        if (auto cvstatus = _condvar.wait_for(
                lock, std::min(rel_time, kUnfulfilledConditionVariableTimeout.toSystemDuration()));
            cvstatus == stdx::cv_status::no_timeout ||
            rel_time <= kUnfulfilledConditionVariableTimeout.toSystemDuration()) {
            return cvstatus;
        }

        if (_conditionVariableActions) {
            if constexpr (std::is_same<decltype(lock), Mutex>::value) {
                _conditionVariableActions->onUnfulfilledConditionVariable(lock.getName());
            } else {
                _conditionVariableActions->onUnfulfilledConditionVariable("AnonymousLock");
            }
        }

        if (auto cvstatus = _condvar.wait_for(
                lock, rel_time - kUnfulfilledConditionVariableTimeout.toSystemDuration());
            cvstatus == stdx::cv_status::no_timeout) {
            return cvstatus;
        }

        uasserted(ErrorCodes::InternalError, "Unable to take latch, wait time exceeds set timeout");
    }

    template <class Lock, class Duration, class Predicate>
    auto _waitWithPredicate(Lock& lock, const Duration& rel_time, Predicate pred) {
        while (!pred()) {
            if (_wait(lock, rel_time) == std::cv_status::timeout) {
                return pred();
            }
        }
        return true;
    }
};

template <class Lock>
void ConditionVariable::wait(Lock& lock) {
    _wait(lock, _conditionVariableTimeout.toSystemDuration());
}

template <class Lock, class Predicate>
void ConditionVariable::wait(Lock& lock, Predicate pred) {
    _waitWithPredicate(lock, _conditionVariableTimeout.toSystemDuration(), std::move(pred));
}

template <class Lock, class Rep, class Period>
stdx::cv_status ConditionVariable::wait_for(Lock& lock,
                                            const stdx::chrono::duration<Rep, Period>& rel_time) {
    return _wait(lock, rel_time);
}

template <class Lock, class Rep, class Period, class Predicate>
bool ConditionVariable::wait_for(Lock& lock,
                                 const stdx::chrono::duration<Rep, Period>& rel_time,
                                 Predicate pred) {
    return _waitWithPredicate(lock, rel_time, pred);
}

template <class Lock, class Clock, class Duration>
stdx::cv_status ConditionVariable::wait_until(
    Lock& lock, const stdx::chrono::time_point<Clock, Duration>& timeout_time) {
    return _wait(lock, timeout_time - stdx::chrono::steady_clock::now());
}

template <class Lock, class Clock, class Duration, class Predicate>
bool ConditionVariable::wait_until(Lock& lock,
                                   const stdx::chrono::time_point<Clock, Duration>& timeout_time,
                                   Predicate pred) {
    return _waitWithPredicate(lock, timeout_time - stdx::chrono::steady_clock::now(), pred);
}

}  // namespace mongo
