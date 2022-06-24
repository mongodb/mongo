/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/db/repl/delayable_timeout_callback.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace repl {

DelayableTimeoutCallback::~DelayableTimeoutCallback() {
    cancel();
}

void DelayableTimeoutCallback::cancel() {
    stdx::lock_guard lk(_mutex);
    _cancel(lk);
}

void DelayableTimeoutCallback::_cancel(WithLock) {
    if (_cbHandle) {
        _executor->cancel(_cbHandle);
        _cbHandle = executor::TaskExecutor::CallbackHandle();
        _nextCall = Date_t();
    }
    invariant(_nextCall == Date_t());
}

Date_t DelayableTimeoutCallback::getNextCall() const {
    stdx::lock_guard lk(_mutex);
    return _nextCall;
}

bool DelayableTimeoutCallback::isActive() const {
    return getNextCall() != Date_t();
}

Status DelayableTimeoutCallback::scheduleAt(Date_t when) {
    stdx::lock_guard lk(_mutex);
    return _scheduleAt(lk, when);
}

Status DelayableTimeoutCallback::_scheduleAt(WithLock lk, Date_t when) {
    if (_cbHandle && when < _nextCall) {
        LOGV2_DEBUG(6602300,
                    3,
                    "Moving a delayable timeout call backwards, which is inefficient",
                    "timerName"_attr = _timerName,
                    "when"_attr = when,
                    "nextCall"_attr = _nextCall);
        _cancel(lk);
    }
    return _delayUntil(lk, when);
}

Status DelayableTimeoutCallback::delayUntil(Date_t when) {
    stdx::lock_guard lk(_mutex);
    return _delayUntil(lk, when);
}

Status DelayableTimeoutCallback::_delayUntil(WithLock lk, Date_t when) {
    if (!_cbHandle) {
        // No timeout is active; just schedule it
        return _reschedule(lk, when);
    }
    if (when == _nextCall) {
        LOGV2_DEBUG(6602301,
                    5,
                    "'Rescheduling' to same time",
                    "timerName"_attr = _timerName,
                    "when"_attr = when,
                    "nextCall"_attr = _nextCall);
    }
    _nextCall = when;
    return Status::OK();
}

void DelayableTimeoutCallback::_handleTimeout(const executor::TaskExecutor::CallbackArgs& args) {
    {
        stdx::lock_guard lk(_mutex);
        if (args.myHandle != _cbHandle) {
            // This is normal when scheduleAt() or cancel() is used.
            LOGV2_DEBUG(6602302,
                        5,
                        "DelayableTimeoutCallback::_handleTimeout got a timeout after a new handle "
                        "was scheduled",
                        "timerName"_attr = _timerName);
            return;
        }
        Date_t now = _executor->now();
        if (args.status == ErrorCodes::CallbackCanceled) {
            // If args.status is CallbackCanceled yet the handles matched, that means the
            // executor canceled the callback itself, probably as part of shutdown.  We
            // do not want to reschedule in this case, nor call the callback.
            _cbHandle = executor::TaskExecutor::CallbackHandle();
            _nextCall = Date_t();
            return;
        } else if (_nextCall > now) {
            Status status = _reschedule(lk, _nextCall);
            if (!status.isOK()) {
                LOGV2_DEBUG(6602303,
                            2,
                            "DelayableTimeoutCallback::_handleTimeout unable to schedule",
                            "timerName"_attr = _timerName,
                            "error"_attr = status);
                fassert(6602305, status == ErrorCodes::ShutdownInProgress);
            }
            return;
        }
        _cbHandle = executor::TaskExecutor::CallbackHandle();
        _nextCall = Date_t();
    }
    _callback(args);
}

Status DelayableTimeoutCallback::_reschedule(WithLock, Date_t when) {
    // We clear _cbHandle and _nextCall in advance so if scheduleWorkAt fails for any reason
    // (including by exception), the invariant that _cbHandle and _nextCall are clear when no
    // callback is scheduled is maintained.
    _cbHandle = executor::TaskExecutor::CallbackHandle();
    _nextCall = Date_t();
    auto cbh = _executor->scheduleWorkAt(
        when, [this](const executor::TaskExecutor::CallbackArgs& args) { _handleTimeout(args); });
    if (cbh == ErrorCodes::ShutdownInProgress) {
        return cbh.getStatus();
    }
    _nextCall = when;
    _cbHandle = fassert(6602304, cbh);
    return Status::OK();
}

void DelayableTimeoutCallbackWithJitter::_resetRandomization(WithLock) {
    _lastRandomizationTime = Date_t();
    _currentJitter = Milliseconds(0);
}

Status DelayableTimeoutCallbackWithJitter::scheduleAt(Date_t when) {
    stdx::lock_guard lk(_mutex);
    _resetRandomization(lk);
    return _scheduleAt(lk, when);
}

Status DelayableTimeoutCallbackWithJitter::delayUntil(Date_t when) {
    stdx::lock_guard lk(_mutex);
    _resetRandomization(lk);
    return _delayUntil(lk, when);
}

Status DelayableTimeoutCallbackWithJitter::delayUntilWithJitter(Date_t when,
                                                                Milliseconds jitterUpperBound) {
    if (jitterUpperBound == Milliseconds::zero())
        return delayUntil(when);
    stdx::lock_guard lk(_mutex);
    Date_t now = _getExecutor()->now();
    Milliseconds elapsed = now - _lastRandomizationTime;
    if (_lastRandomizationTime == Date_t() || elapsed < Milliseconds::zero() ||
        elapsed >= jitterUpperBound || jitterUpperBound < _currentJitter) {
        _lastRandomizationTime = now;
        _currentJitter = Milliseconds(_randomSource(durationCount<Milliseconds>(jitterUpperBound)));
    }
    return _delayUntil(lk, when + _currentJitter);
}

}  // namespace repl
}  // namespace mongo
