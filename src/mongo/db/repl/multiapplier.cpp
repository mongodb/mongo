/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/multiapplier.h"

#include <utility>

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/destructor_guard.h"

namespace mongo {
namespace repl {

MultiApplier::MultiApplier(executor::TaskExecutor* executor,
                           const Operations& operations,
                           const ApplyOperationFn& applyOperation,
                           const MultiApplyFn& multiApply,
                           const CallbackFn& onCompletion)
    : _executor(executor),
      _operations(operations),
      _applyOperation(applyOperation),
      _multiApply(multiApply),
      _onCompletion(onCompletion) {
    uassert(ErrorCodes::BadValue, "null replication executor", executor);
    uassert(ErrorCodes::BadValue, "empty list of operations", !operations.empty());
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "last operation missing 'ts' field: " << operations.back().raw,
            operations.back().raw.hasField("ts"));
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "'ts' in last operation not a timestamp: " << operations.back().raw,
            BSONType::bsonTimestamp == operations.back().raw.getField("ts").type());
    uassert(ErrorCodes::BadValue, "apply operation function cannot be null", applyOperation);
    uassert(ErrorCodes::BadValue, "multi apply function cannot be null", multiApply);
    uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);
}

MultiApplier::~MultiApplier() {
    DESTRUCTOR_GUARD(shutdown(); join(););
}

std::string MultiApplier::toString() const {
    return getDiagnosticString();
}

std::string MultiApplier::getDiagnosticString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    str::stream output;
    output << "MultiApplier";
    output << " active: " << _isActive_inlock();
    output << ", ops: " << _operations.front().ts.timestamp().toString();
    output << " - " << _operations.back().ts.timestamp().toString();
    output << ", executor: " << _executor->getDiagnosticString();
    return output;
}

bool MultiApplier::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isActive_inlock();
}

bool MultiApplier::_isActive_inlock() const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status MultiApplier::startup() noexcept {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

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

    auto scheduleResult =
        _executor->scheduleWork(stdx::bind(&MultiApplier::_callback, this, stdx::placeholders::_1));
    if (!scheduleResult.isOK()) {
        _state = State::kComplete;
        return scheduleResult.getStatus();
    }

    _dbWorkCallbackHandle = scheduleResult.getValue();

    return Status::OK();
}

void MultiApplier::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _condition.wait(lk, [this]() { return !_isActive_inlock(); });
}

MultiApplier::State MultiApplier::getState_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
        applyStatus = _multiApply(opCtx.get(), _operations, _applyOperation);
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
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_onCompletion);
        std::swap(_onCompletion, onCompletion);
    }

    onCompletion(result);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
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
