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


#include "mongo/db/repl/rollback_checker.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

RollbackChecker::RollbackChecker(executor::TaskExecutor* executor, HostAndPort syncSource)
    : _executor(executor), _syncSource(syncSource), _baseRBID(-1), _lastRBID(-1) {
    uassert(ErrorCodes::BadValue, "null task executor", executor);
}

RollbackChecker::~RollbackChecker() {}

StatusWith<RollbackChecker::CallbackHandle> RollbackChecker::checkForRollback(
    const CallbackFn& nextAction) {
    return _scheduleGetRollbackId([this, nextAction](const RemoteCommandCallbackArgs& args) {
        if (!args.response.isOK()) {
            nextAction(args.response.status);
            return;
        }
        if (auto rbidElement = args.response.data["rbid"]) {
            int remoteRBID = rbidElement.numberInt();

            UniqueLock lk(_mutex);
            bool hadRollback = _checkForRollback(lk, remoteRBID);
            lk.unlock();
            nextAction(hadRollback);
        } else {
            nextAction(Status(ErrorCodes::CommandFailed,
                              "replSetGetRBID command failed when checking for rollback"));
        }
    });
}

RollbackChecker::Result RollbackChecker::hasHadRollback() {
    // Default to true in case the callback is not called in an error case.
    Result result(true);
    auto cbh = checkForRollback([&result](const Result& cbResult) { result = cbResult; });

    if (!cbh.isOK()) {
        return cbh.getStatus();
    }

    _executor->wait(cbh.getValue());
    return result;
}

StatusWith<RollbackChecker::CallbackHandle> RollbackChecker::reset(const CallbackFn& nextAction) {
    return _scheduleGetRollbackId([this, nextAction](const RemoteCommandCallbackArgs& args) {
        if (!args.response.isOK()) {
            nextAction(args.response.status);
            return;
        }
        if (auto rbidElement = args.response.data["rbid"]) {
            int newRBID = rbidElement.numberInt();

            UniqueLock lk(_mutex);
            _setRBID(lk, newRBID);
            lk.unlock();

            // Actual bool value does not matter because reset_sync()
            // will convert non-error StatusWith<bool> to Status::OK.
            nextAction(true);
        } else {
            nextAction(Status(ErrorCodes::CommandFailed,
                              "replSetGetRBID command failed when checking for rollback"));
        }
    });
}

Status RollbackChecker::reset_sync() {
    // Default to an error in case the callback is not called in an error case.
    Status resetStatus = Status(ErrorCodes::CommandFailed, "RollbackChecker reset failed");
    auto cbh = reset([&resetStatus](const Result& result) { resetStatus = result.getStatus(); });

    if (!cbh.isOK()) {
        return Status(ErrorCodes::CallbackCanceled,
                      "RollbackChecker reset failed due to callback cancellation");
    }

    _executor->wait(cbh.getValue());
    return resetStatus;
}

int RollbackChecker::getBaseRBID() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _baseRBID;
}

int RollbackChecker::getLastRBID_forTest() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lastRBID;
}

bool RollbackChecker::_checkForRollback(WithLock lk, int remoteRBID) {
    _lastRBID = remoteRBID;
    return remoteRBID != _baseRBID;
}

StatusWith<RollbackChecker::CallbackHandle> RollbackChecker::_scheduleGetRollbackId(
    const RemoteCommandCallbackFn& nextAction) {
    executor::RemoteCommandRequest getRollbackIDReq(
        _syncSource, DatabaseName::kAdmin, BSON("replSetGetRBID" << 1), nullptr);
    return _executor->scheduleRemoteCommand(getRollbackIDReq, nextAction);
}

void RollbackChecker::_setRBID(WithLock lk, int rbid) {
    _baseRBID = rbid;
    _lastRBID = rbid;
}

}  // namespace repl
}  // namespace mongo
