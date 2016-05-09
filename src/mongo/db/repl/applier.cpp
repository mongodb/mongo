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

#include "mongo/db/repl/applier.h"

#include <algorithm>

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

Applier::Applier(ReplicationExecutor* executor,
                 const Operations& operations,
                 const ApplyOperationFn& applyOperation,
                 const CallbackFn& onCompletion)
    : _executor(executor),
      _operations(operations),
      _applyOperation(applyOperation),
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
    uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);
}

Applier::~Applier() {
    DESTRUCTOR_GUARD(cancel(); wait(););
}

std::string Applier::getDiagnosticString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    str::stream output;
    output << "Applier";
    output << " executor: " << _executor->getDiagnosticString();
    output << " active: " << _active;
    return output;
}

bool Applier::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _active;
}

Status Applier::start() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "applier already started");
    }

    auto scheduleResult =
        _executor->scheduleDBWork(stdx::bind(&Applier::_callback, this, stdx::placeholders::_1));
    if (!scheduleResult.isOK()) {
        return scheduleResult.getStatus();
    }

    _active = true;
    _dbWorkCallbackHandle = scheduleResult.getValue();

    return Status::OK();
}

void Applier::cancel() {
    ReplicationExecutor::CallbackHandle dbWorkCallbackHandle;
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

void Applier::wait() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    while (_active) {
        _condition.wait(lk);
    }
}

void Applier::_callback(const ReplicationExecutor::CallbackArgs& cbd) {
    if (!cbd.status.isOK()) {
        _finishCallback(cbd.status, _operations);
        return;
    }

    invariant(cbd.txn);

    // Refer to multiSyncApply() and multiInitialSyncApply() in sync_tail.cpp.
    cbd.txn->setReplicatedWrites(false);

    // allow us to get through the magic barrier
    cbd.txn->lockState()->setIsBatchWriter(true);

    Status applyStatus(ErrorCodes::InternalError, "not mutated");

    invariant(!_operations.empty());
    for (auto i = _operations.cbegin(); i != _operations.cend(); ++i) {
        try {
            applyStatus = _applyOperation(cbd.txn, *i);
        } catch (...) {
            applyStatus = exceptionToStatus();
        }
        if (!applyStatus.isOK()) {
            // 'i' points to last operation that was not applied.
            _finishCallback(applyStatus, Operations(i, _operations.cend()));
            return;
        }
    }
    _finishCallback(_operations.back().raw.getField("ts").timestamp(), Operations());
}

void Applier::_finishCallback(const StatusWith<Timestamp>& result, const Operations& operations) {
    _onCompletion(result, operations);
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _active = false;
    _condition.notify_all();
}

namespace {

void pauseBeforeCompletion(const StatusWith<Timestamp>& result,
                           const Applier::Operations& operationsOnCompletion,
                           const PauseDataReplicatorFn& pauseDataReplicator,
                           const Applier::CallbackFn& onCompletion) {
    if (result.isOK()) {
        pauseDataReplicator();
    }
    onCompletion(result, operationsOnCompletion);
};

}  // namespace

StatusWith<std::pair<std::unique_ptr<Applier>, Applier::Operations>> applyUntilAndPause(
    ReplicationExecutor* executor,
    const Applier::Operations& operations,
    const Applier::ApplyOperationFn& applyOperation,
    const Timestamp& lastTimestampToApply,
    const PauseDataReplicatorFn& pauseDataReplicator,
    const Applier::CallbackFn& onCompletion) {
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
        Applier::Operations operationsInRange(operations.cbegin(), j);
        Applier::Operations operationsNotInRange(j, operations.cend());
        if (!found) {
            return std::make_pair(std::unique_ptr<Applier>(new Applier(
                                      executor, operationsInRange, applyOperation, onCompletion)),
                                  operationsNotInRange);
        }

        return std::make_pair(
            std::unique_ptr<Applier>(new Applier(executor,
                                                 operationsInRange,
                                                 applyOperation,
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
