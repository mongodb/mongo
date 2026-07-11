// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
using UniqueLock = std::unique_lock<std::mutex>;

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
    std::lock_guard<std::mutex> lk(_mutex);
    return _baseRBID;
}

int RollbackChecker::getLastRBID_forTest() {
    std::lock_guard<std::mutex> lk(_mutex);
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
