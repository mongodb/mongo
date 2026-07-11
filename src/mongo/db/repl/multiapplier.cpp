// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <mutex>
#include <ostream>
#include <utility>

namespace mongo {
namespace repl {

MultiApplier::MultiApplier(executor::TaskExecutor* executor,
                           const std::vector<OplogEntry>& operations,
                           const MultiApplyFn& multiApply,
                           CallbackFn onCompletion)
    : _executor(executor),
      _operations(operations),
      _multiApply(multiApply),
      _onCompletion(std::move(onCompletion)) {
    uassert(ErrorCodes::BadValue, "null replication executor", executor);
    uassert(ErrorCodes::BadValue, "empty list of operations", !operations.empty());
    uassert(ErrorCodes::BadValue, "multi apply function cannot be null", multiApply);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", _onCompletion);
}

MultiApplier::~MultiApplier() {
    try {
        shutdown();
        join();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

bool MultiApplier::isActive() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _isActive(lk);
}

bool MultiApplier::_isActive(WithLock lk) const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status MultiApplier::startup() noexcept {
    std::lock_guard<std::mutex> lk(_mutex);

    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::InternalError, "multi applier already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "multi applier shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "multi applier completed");
    }

    auto scheduleResult = _executor->scheduleWork(
        [=, this](const executor::TaskExecutor::CallbackArgs& cbd) { return _callback(cbd); });
    if (!scheduleResult.isOK()) {
        _state = State::kComplete;
        return scheduleResult.getStatus();
    }

    _dbWorkCallbackHandle = scheduleResult.getValue();

    return Status::OK();
}

void MultiApplier::shutdown() {
    std::lock_guard<std::mutex> lk(_mutex);
    switch (_state) {
        case State::kPreStart:
            // Transition directly from PreStart to Complete if not started yet.
            _state = State::kComplete;
            return;
        case State::kRunning:
            _state = State::kShuttingDown;
            break;
        case State::kShuttingDown:
        case State::kComplete:
            // Nothing to do if we are already in ShuttingDown or Complete state.
            return;
    }

    if (_dbWorkCallbackHandle.isValid()) {
        _executor->cancel(_dbWorkCallbackHandle);
    }
}

void MultiApplier::join() {
    std::unique_lock<std::mutex> lk(_mutex);
    _condition.wait(lk, [&]() { return !_isActive(lk); });
}

MultiApplier::State MultiApplier::getState_forTest() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _state;
}

void MultiApplier::_callback(const executor::TaskExecutor::CallbackArgs& cbd) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status);
        return;
    }

    invariant(!_operations.empty());

    StatusWith<OpTime> applyStatus(ErrorCodes::InternalError, "not mutated");
    try {
        auto opCtx = cc().makeOperationContext();
        applyStatus = _multiApply(opCtx.get(), _operations);
    } catch (...) {
        applyStatus = exceptionToStatus();
    }
    _finishCallback(applyStatus.getStatus());
}

void MultiApplier::_finishCallback(const Status& result) {
    // After running callback function, clear '_onCompletion' to release any resources that might be
    // held by this function object.
    // '_onCompletion' must be moved to a temporary copy and destroyed outside the lock in case
    // there is any logic that's invoked at the function object's destruction that might call into
    // this MultiApplier. 'onCompletion' must be declared before lock guard 'lock' so that it is
    // destroyed outside the lock.
    decltype(_onCompletion) onCompletion;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        invariant(_onCompletion);
        std::swap(_onCompletion, onCompletion);
    }

    onCompletion(result);

    std::lock_guard<std::mutex> lk(_mutex);
    invariant(State::kComplete != _state);
    _state = State::kComplete;
    _condition.notify_all();
}

std::ostream& operator<<(std::ostream& os, const MultiApplier::State& state) {
    switch (state) {
        case MultiApplier::State::kPreStart:
            return os << "PreStart";
        case MultiApplier::State::kRunning:
            return os << "Running";
        case MultiApplier::State::kShuttingDown:
            return os << "ShuttingDown";
        case MultiApplier::State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
