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


// IWYU pragma: no_include <bits/types/clockid_t.h>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <sys/types.h>

#if defined(__linux__)
#include <ctime>
#endif  // defined(__linux__)

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_cpu_timer.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {


#if defined(__linux__)

namespace {

// Reads the thread timer, if available. If not available, results in fatal failure,
// if abortOnFailure was specified.
template <bool abortOnFailure = true>
boost::optional<Nanoseconds> getThreadCPUTime() {
    struct timespec t;
    if (MONGO_unlikely(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0)) {
        if constexpr (abortOnFailure) {
            auto ec = lastSystemError();
            LOGV2_FATAL(4744601,
                        "Failed to read the CPU time for the current thread",
                        "error"_attr = errorMessage(ec));
        }
        return {};
    }
    return Seconds(t.tv_sec) + Nanoseconds(t.tv_nsec);
}

MONGO_FAIL_POINT_DEFINE(hangCPUTimerAfterOnThreadAttach);
MONGO_FAIL_POINT_DEFINE(hangCPUTimerAfterOnThreadDetach);

class PosixOperationCPUTimers final : public OperationCPUTimers {
public:
    explicit PosixOperationCPUTimers(OperationContext* opCtx);
    ~PosixOperationCPUTimers() override;

    OperationCPUTimer makeTimer() override;

    void onThreadAttach() override;
    void onThreadDetach() override;

    size_t runningCount() const override {
        return _runningCount;
    }

private:
    Nanoseconds _getOperationThreadTime() const override;
    void _onTimerStart() override;
    void _onTimerStop() override;

    bool _isAttachedToCurrentThread() const {
        return _threadId && *_threadId == stdx::this_thread::get_id();
    }

    Nanoseconds _getThreadTime() const {
        return getThreadCPUTime<true>().get();
    }

    // Storage for the self pointer as long as this object is valid. The storage is allocated using
    // monotonic allocator in OperationContext, and remain valid until after all decorations are
    // destroyed.
    // The self pointer will be stored here when PosixOperationCPUTimers is constructed and unset
    // when PosixOperationCPUTimers is destroyed.
    OperationCPUTimers** _selfPtr;

    // The id of the attached thread, if any. Any access to current cpu thread time must be
    // done from this thread, as otherwise the value will be meaningless.
    boost::optional<stdx::thread::id> _threadId;

    // Total time adjustments accumulated so far. Includes the time spent when thread was detached
    // as negative.
    Nanoseconds _threadTimeAdjustment{0};

    // Count of currently running timers.
    size_t _runningCount{0};
};

template <typename A, typename T>
T* allocateCopy(A&& alloc, T v) {
    using RebindAllocator =
        typename std::allocator_traits<std::decay_t<A>>::template rebind_alloc<T>;
    using RebindAllocatorTraits = std::allocator_traits<RebindAllocator>;
    auto rebindAlloc = RebindAllocator(alloc);
    auto p = RebindAllocatorTraits::allocate(rebindAlloc, 1);
    RebindAllocatorTraits::construct(rebindAlloc, p, std::move(v));
    return p;
}

PosixOperationCPUTimers::PosixOperationCPUTimers(OperationContext* opCtx)
    // Allocate storage for self pointer using monotonic allocator and assign in to self pointer
    : _selfPtr(allocateCopy(opCtx->monotonicAllocator(), (OperationCPUTimers*)this)) {}

PosixOperationCPUTimers::~PosixOperationCPUTimers() {
    // Reset the self pointer, but don't destroy it's storage. It will be automatically deallocated
    // by the monotonic allocator.
    *_selfPtr = nullptr;
}

OperationCPUTimer PosixOperationCPUTimers::makeTimer() {
    return OperationCPUTimer(_selfPtr);
}

void PosixOperationCPUTimers::onThreadAttach() {
    if (!_runningCount) {
        return;
    }

    invariant(!_threadId, "PosixOperationCPUTimers has already been attached");
    _threadId = stdx::this_thread::get_id();
    _threadTimeAdjustment -= _getThreadTime();

    hangCPUTimerAfterOnThreadAttach.pauseWhileSet();
}

void PosixOperationCPUTimers::onThreadDetach() {
    if (!_runningCount) {
        return;
    }

    invariant(_isAttachedToCurrentThread(),
              "PosixOperationCPUTimers is not attached to current thread");
    _threadId.reset();
    _threadTimeAdjustment += _getThreadTime();

    hangCPUTimerAfterOnThreadDetach.pauseWhileSet();
}

Nanoseconds PosixOperationCPUTimers::_getOperationThreadTime() const {
    invariant(_isAttachedToCurrentThread(),
              "PosixOperationCPUTimers is not attached to current thread");
    return _getThreadTime() + _threadTimeAdjustment;
}

void PosixOperationCPUTimers::_onTimerStart() {
    if (!_runningCount) {
        _threadId = stdx::this_thread::get_id();
    }
    ++_runningCount;
}

void PosixOperationCPUTimers::_onTimerStop() {
    --_runningCount;
    if (!_runningCount) {
        _threadId = {};
    }
}

static auto getCPUTimers =
    OperationContext::declareDecoration<boost::optional<PosixOperationCPUTimers>>();

class OperationCPUTimersClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override {}
    void onDestroyClient(Client* client) override {}
    void onCreateOperationContext(OperationContext* opCtx) override;
    void onDestroyOperationContext(OperationContext* opCtx) override {}
};

void OperationCPUTimersClientObserver::onCreateOperationContext(OperationContext* opCtx) {
    // Checks for time support on POSIX platforms. In particular, it checks for support in
    // presence of SMP systems.
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

    if (!isTimeSupported) {
        return;
    }

    // Construct the PosixOperationCPUTimers
    getCPUTimers(opCtx).emplace(opCtx);
}

// Register ConstructorAction to initialize the OperationCPUTimersHolder right after
// OperationContext is ready.
ServiceContext::ConstructorActionRegisterer operationCPUTimersClientObserverRegisterer{
    "OperationCPUTimersClientObserver", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<OperationCPUTimersClientObserver>());
    }};
}  // namespace

OperationCPUTimers* OperationCPUTimers::get(OperationContext* opCtx) {
    return &*getCPUTimers(opCtx);
}

#else  // not defined(__linux__)

namespace {

class CPUTimersClientObserver final : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client* client) override {}
    void onDestroyClient(Client* client) override {}
    void onCreateOperationContext(OperationContext* opCtx) override {}
    void onDestroyOperationContext(OperationContext* opCtx) override {}
};

ServiceContext::ConstructorActionRegisterer cpuTimersClientObserverRegisterer{
    "CPUTimersClientObserver", [](ServiceContext* service) {
        service->registerClientObserver(std::make_unique<CPUTimersClientObserver>());
    }};
}  // namespace

OperationCPUTimers* OperationCPUTimers::get(OperationContext*) {
    return nullptr;
}

#endif  // defined(__linux__)

OperationCPUTimer::OperationCPUTimer(OperationCPUTimers** timers)
    : _timers(timers), _timerIsRunning(false), _elapsedAdjustment(0) {}

OperationCPUTimer::~OperationCPUTimer() {
    if (_timerIsRunning) {
        if (auto timers = getTimers()) {
            timers->_onTimerStop();
        }
    }
}

Nanoseconds OperationCPUTimer::getElapsed() const {
    if (_timerIsRunning) {
        auto timers = getTimers();
        invariant(timers, "Underlying OperationCPUTimers has already been destroyed");
        return timers->_getOperationThreadTime() + _elapsedAdjustment;
    } else {
        return _elapsedAdjustment;
    }
}

void OperationCPUTimer::start() {
    invariant(!_timerIsRunning, "Timer has already started");
    _timerIsRunning = true;
    if (auto timers = getTimers()) {
        timers->_onTimerStart();
        _elapsedAdjustment = -timers->_getOperationThreadTime();
    }
}

void OperationCPUTimer::stop() {
    invariant(_timerIsRunning, "Timer is not running");
    _timerIsRunning = false;
    if (auto timers = getTimers()) {
        _elapsedAdjustment += timers->_getOperationThreadTime();
        timers->_onTimerStop();
    }
}

}  // namespace mongo
