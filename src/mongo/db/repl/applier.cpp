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

#include <boost/thread/lock_guard.hpp>
#include <boost/thread/lock_types.hpp>

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/util/assert_util.h"
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
                str::stream() << "last operation missing 'ts' field: " << operations.back(),
                operations.back().hasField("ts"));
        uassert(ErrorCodes::TypeMismatch,
                str::stream() << "'ts' in last operation not a timestamp: " << operations.back(),
                BSONType::bsonTimestamp == operations.back().getField("ts").type());
        uassert(ErrorCodes::BadValue, "apply operation function cannot be null", applyOperation);
        uassert(ErrorCodes::BadValue, "callback function cannot be null", onCompletion);
    }

    Applier::~Applier() {
        DESTRUCTOR_GUARD(
            cancel();
            wait();
        );
    }

    std::string Applier::getDiagnosticString() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        str::stream output;
        output << "Applier";
        output << " executor: " << _executor->getDiagnosticString();
        output << " active: " << _active;
        return output;
    }

    bool Applier::isActive() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _active;
    }

    Status Applier::start() {
        boost::lock_guard<boost::mutex> lk(_mutex);

        if (_active) {
            return Status(ErrorCodes::IllegalOperation, "applier already started");
        }

        auto scheduleResult = _executor->scheduleDBWork(
            stdx::bind(&Applier::_callback, this, stdx::placeholders::_1));
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
            boost::lock_guard<boost::mutex> lk(_mutex);

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
        boost::unique_lock<boost::mutex> lk(_mutex);

        while (_active) {
            _condition.wait(lk);
        }
    }

    void Applier::_callback(const ReplicationExecutor::CallbackData& cbd) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _active = false;

        if (!cbd.status.isOK()) {
            _onCompletion(cbd.status, _operations);
            _condition.notify_all();
            return;
        }

        invariant(cbd.txn);

        // Refer to multiSyncApply() and multiInitialSyncApply() in sync_tail.cpp.
        cbd.txn->setReplicatedWrites(false);

        // allow us to get through the magic barrier
        cbd.txn->lockState()->setIsBatchWriter(true);

        Status applyStatus(ErrorCodes::InternalError, "not mutated");
        auto i = _operations.cbegin();
        invariant(i != _operations.cend());
        for (; i != _operations.cend(); ++i) {
            try {
                applyStatus = _applyOperation(cbd.txn, *i);
            }
            catch (...) {
                applyStatus = exceptionToStatus();
            }
            if (!applyStatus.isOK()) {
                break;
            }
        }
        // 'i' points to last operation that was not applied; or cend() if all operations were
        // applied successfully.
        if (!applyStatus.isOK()) {
            _onCompletion(applyStatus, Operations(i, _operations.cend()));
        }
        else {
            _onCompletion(_operations.back().getField("ts").timestamp(), Operations());
        }
        _condition.notify_all();
    }

} // namespace repl
} // namespace mongo
