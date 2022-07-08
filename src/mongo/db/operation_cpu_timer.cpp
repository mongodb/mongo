/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include <boost/optional.hpp>
#include <fmt/format.h>

#if defined(__linux__)
#include <time.h>
#endif  // defined(__linux__)

#include "mongo/db/operation_cpu_timer.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

using namespace fmt::literals;

#if defined(__linux__)

namespace {

// Reads the thread timer, and throws with `InternalError` if that fails.
Nanoseconds getThreadCPUTime() {
    struct timespec t;
    if (auto ret = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); ret != 0) {
        auto ec = lastSystemError();
        iassert(
            Status(ErrorCodes::InternalError, "Unable to get time: {}"_format(errorMessage(ec))));
    }
    return Seconds(t.tv_sec) + Nanoseconds(t.tv_nsec);
}

MONGO_FAIL_POINT_DEFINE(hangCPUTimerAfterOnThreadAttach);
MONGO_FAIL_POINT_DEFINE(hangCPUTimerAfterOnThreadDetach);

class PosixTimer final : public OperationCPUTimer {
public:
    Nanoseconds getElapsed() const override;

    void start() override;
    void stop() override;

    void onThreadAttach() override;
    void onThreadDetach() override;

private:
    bool _timerIsRunning() const;
    bool _isAttachedToCurrentThread() const;

    // Returns the elapsed time since the creation of the current thread.
    Nanoseconds _getThreadTime() const;

    // Holds the value returned by `_getThreadTime()` at the time of starting/resuming the timer.
    boost::optional<Nanoseconds> _startedOn;
    boost::optional<stdx::thread::id> _threadId;
    Nanoseconds _elapsedBeforeInterrupted = Nanoseconds(0);
};

Nanoseconds PosixTimer::getElapsed() const {
    invariant(_isAttachedToCurrentThread(), "Not attached to current thread");
    auto elapsed = _elapsedBeforeInterrupted;
    if (_timerIsRunning())
        elapsed += _getThreadTime() - _startedOn.get();
    return elapsed;
}

bool PosixTimer::_timerIsRunning() const {
    return _startedOn.has_value();
}

bool PosixTimer::_isAttachedToCurrentThread() const {
    return _threadId.has_value() && _threadId.get() == stdx::this_thread::get_id();
}

void PosixTimer::start() {
    invariant(!_timerIsRunning(), "Timer has already started");

    _startedOn = _getThreadTime();
    _threadId = stdx::this_thread::get_id();
    _elapsedBeforeInterrupted = Nanoseconds(0);
}

void PosixTimer::stop() {
    invariant(_timerIsRunning(), "Timer is not running");
    invariant(_isAttachedToCurrentThread());

    _elapsedBeforeInterrupted = getElapsed();
    _startedOn.reset();
}

void PosixTimer::onThreadAttach() {
    if (!_timerIsRunning())
        return;

    invariant(!_threadId.has_value(), "Timer has already been attached");
    _threadId = stdx::this_thread::get_id();
    _startedOn = _getThreadTime();

    hangCPUTimerAfterOnThreadAttach.pauseWhileSet();
}

void PosixTimer::onThreadDetach() {
    if (!_timerIsRunning())
        return;

    invariant(_threadId.has_value(), "Timer is not attached");
    _threadId.reset();
    _elapsedBeforeInterrupted += _getThreadTime() - _startedOn.get();

    hangCPUTimerAfterOnThreadDetach.pauseWhileSet();
}

Nanoseconds PosixTimer::_getThreadTime() const try {
    return getThreadCPUTime();
} catch (const ExceptionFor<ErrorCodes::InternalError>& ex) {
    // Abort the process as the timer cannot account for the elapsed time. This path is only
    // reachable if the platform supports CPU time measurement at startup, but returns an error
    // for a subsequent attempt to get thread-specific CPU consumption.
    LOGV2_FATAL(4744601, "Failed to read the CPU time for the current thread", "error"_attr = ex);
}

static auto getCPUTimer = OperationContext::declareDecoration<PosixTimer>();

}  // namespace

OperationCPUTimer* OperationCPUTimer::get(OperationContext* opCtx) {
    invariant(Client::getCurrent() && Client::getCurrent()->getOperationContext() == opCtx,
              "Operation not attached to the current thread");

    // Checks for time support on POSIX platforms. In particular, it checks for support in presence
    // of SMP systems.
    static bool isTimeSupported = [] {
        clockid_t cid;
        if (clock_getcpuclockid(0, &cid) != 0)
            return false;

        try {
            getThreadCPUTime();
        } catch (const ExceptionFor<ErrorCodes::InternalError>&) {
            // Unable to collect the CPU time for the current thread.
            return false;
        }

        return true;
    }();

    if (!isTimeSupported)
        return nullptr;
    return &getCPUTimer(opCtx);
}

#else  // not defined(__linux__)

OperationCPUTimer* OperationCPUTimer::get(OperationContext*) {
    return nullptr;
}

#endif  // defined(__linux__)

}  // namespace mongo
