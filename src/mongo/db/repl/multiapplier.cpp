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
    DESTRUCTOR_GUARD(cancel(); wait(););
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

Status MultiApplier::start() {
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

void MultiApplier::cancel() {
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

void MultiApplier::wait() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    while (_active) {
        _condition.wait(lk);
    }
}

void MultiApplier::_callback(const executor::TaskExecutor::CallbackArgs& cbd) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status, _operations);
        return;
    }

    invariant(!_operations.empty());

    StatusWith<OpTime> applyStatus(ErrorCodes::InternalError, "not mutated");
    try {
        auto txn = cc().makeOperationContext();

        // Refer to multiSyncApply() and multiInitialSyncApply() in sync_tail.cpp.
        txn->setReplicatedWrites(false);

        // allow us to get through the magic barrier
        txn->lockState()->setIsBatchWriter(true);

        applyStatus = _multiApply(txn.get(), _operations, _applyOperation);
    } catch (...) {
        applyStatus = exceptionToStatus();
    }
    if (!applyStatus.isOK()) {
        _finishCallback(applyStatus.getStatus(), _operations);
        return;
    }
    _finishCallback(applyStatus.getValue().getTimestamp(), _operations);
}

void MultiApplier::_finishCallback(const StatusWith<Timestamp>& result,
                                   const Operations& operations) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _active = false;
    _condition.notify_all();
    auto finish = _onCompletion;
    lk.unlock();

    // This instance may be destroyed during the "finish" call.
    finish(result, operations);
}

namespace {

void pauseBeforeCompletion(const StatusWith<Timestamp>& result,
                           const MultiApplier::Operations& operationsOnCompletion,
                           const PauseDataReplicatorFn& pauseDataReplicator,
                           const MultiApplier::CallbackFn& onCompletion) {
    if (result.isOK()) {
        pauseDataReplicator();
    }
    onCompletion(result, operationsOnCompletion);
};

}  // namespace

StatusWith<std::pair<std::unique_ptr<MultiApplier>, MultiApplier::Operations>> applyUntilAndPause(
    executor::TaskExecutor* executor,
    const MultiApplier::Operations& operations,
    const MultiApplier::ApplyOperationFn& applyOperation,
    const MultiApplier::MultiApplyFn& multiApply,
    const Timestamp& lastTimestampToApply,
    const PauseDataReplicatorFn& pauseDataReplicator,
    const MultiApplier::CallbackFn& onCompletion) {
    try {
        auto comp = [](const OplogEntry& left, const OplogEntry& right) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Operation missing 'ts' field': " << left.raw,
                    left.raw.hasField("ts"));
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "Operation missing 'ts' field': " << right.raw,
                    right.raw.hasField("ts"));
            return left.raw["ts"].timestamp() < right.raw["ts"].timestamp();
        };
        auto wrapped = OplogEntry(BSON("ts" << lastTimestampToApply));
        auto i = std::lower_bound(operations.cbegin(), operations.cend(), wrapped, comp);
        bool found = i != operations.cend() && !comp(wrapped, *i);
        auto j = found ? i + 1 : i;
        MultiApplier::Operations operationsInRange(operations.cbegin(), j);
        MultiApplier::Operations operationsNotInRange(j, operations.cend());
        if (!found) {
            return std::make_pair(
                std::unique_ptr<MultiApplier>(new MultiApplier(
                    executor, operationsInRange, applyOperation, multiApply, onCompletion)),
                operationsNotInRange);
        }

        return std::make_pair(
            std::unique_ptr<MultiApplier>(new MultiApplier(executor,
                                                           operationsInRange,
                                                           applyOperation,
                                                           multiApply,
                                                           stdx::bind(pauseBeforeCompletion,
                                                                      stdx::placeholders::_1,
                                                                      stdx::placeholders::_2,
                                                                      pauseDataReplicator,
                                                                      onCompletion))),
            operationsNotInRange);
    } catch (...) {
        return exceptionToStatus();
    }
    MONGO_UNREACHABLE;
    return Status(ErrorCodes::InternalError, "unreachable");
}

}  // namespace repl
}  // namespace mongo
