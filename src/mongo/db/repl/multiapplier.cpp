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
      _onCompletion(onCompletion),
      _active(false) {
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
    output << " active: " << _active;
    output << ", ops: " << _operations.front().ts.timestamp().toString();
    output << " - " << _operations.back().ts.timestamp().toString();
    output << ", executor: " << _executor->getDiagnosticString();
    return output;
}

bool MultiApplier::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _active;
}

Status MultiApplier::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "applier already started");
    }

    auto scheduleResult =
        _executor->scheduleWork(stdx::bind(&MultiApplier::_callback, this, stdx::placeholders::_1));
    if (!scheduleResult.isOK()) {
        return scheduleResult.getStatus();
    }

    _active = true;
    _dbWorkCallbackHandle = scheduleResult.getValue();

    return Status::OK();
}

void MultiApplier::shutdown() {
    executor::TaskExecutor::CallbackHandle dbWorkCallbackHandle;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (!_active) {
            return;
        }

        dbWorkCallbackHandle = _dbWorkCallbackHandle;
    }

    if (dbWorkCallbackHandle.isValid()) {
        _executor->cancel(dbWorkCallbackHandle);
    }
}

void MultiApplier::join() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    while (_active) {
        _condition.wait(lk);
    }
}

void MultiApplier::_callback(const executor::TaskExecutor::CallbackArgs& cbd) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status);
        return;
    }

    invariant(!_operations.empty());

    StatusWith<OpTime> applyStatus(ErrorCodes::InternalError, "not mutated");
    try {
        auto txn = cc().makeOperationContext();
        applyStatus = _multiApply(txn.get(), _operations, _applyOperation);
    } catch (...) {
        applyStatus = exceptionToStatus();
    }
    _finishCallback(applyStatus.getStatus());
}

void MultiApplier::_finishCallback(const Status& result) {
    _onCompletion(result);

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _active = false;
    _condition.notify_all();
}

}  // namespace repl
}  // namespace mongo
