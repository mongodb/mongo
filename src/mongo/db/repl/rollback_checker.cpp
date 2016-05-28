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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_checker.h"

#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

RollbackChecker::RollbackChecker(executor::TaskExecutor* executor, HostAndPort syncSource)
    : _executor(executor), _syncSource(syncSource), _baseRBID(-1), _lastRBID(-1) {
    uassert(ErrorCodes::BadValue, "null task executor", executor);
}

RollbackChecker::~RollbackChecker() {}

RollbackChecker::CallbackHandle RollbackChecker::checkForRollback(const CallbackFn& nextAction) {
    return _scheduleGetRollbackId(
        [this, nextAction](const RemoteCommandCallbackArgs& args) {
            if (args.response.getStatus() == ErrorCodes::CallbackCanceled) {
                return;
            }
            if (!args.response.isOK()) {
                nextAction(args.response.getStatus());
                return;
            }
            if (auto rbidElement = args.response.getValue().data["rbid"]) {
                int remoteRBID = rbidElement.numberInt();

                UniqueLock lk(_mutex);
                bool hadRollback = _checkForRollback_inlock(remoteRBID);
                lk.unlock();

                if (hadRollback) {
                    nextAction(Status(ErrorCodes::UnrecoverableRollbackError,
                                      "RollbackChecker detected rollback occurred"));
                } else {
                    nextAction(Status::OK());
                }
            } else {
                nextAction(Status(ErrorCodes::CommandFailed,
                                  "replSetGetRBID command failed when checking for rollback"));
            }
        },
        nextAction);
}

bool RollbackChecker::hasHadRollback() {
    // Default to true in case the callback is not called in an error case.
    bool hasHadRollback = true;
    auto cbh = checkForRollback(
        [&hasHadRollback](const Status& status) { hasHadRollback = !status.isOK(); });
    _executor->wait(cbh);
    return hasHadRollback;
}

RollbackChecker::CallbackHandle RollbackChecker::reset(const CallbackFn& nextAction) {
    return _scheduleGetRollbackId(
        [this, nextAction](const RemoteCommandCallbackArgs& args) {
            if (args.response.getStatus() == ErrorCodes::CallbackCanceled) {
                return;
            }
            if (!args.response.isOK()) {
                nextAction(args.response.getStatus());
                return;
            }
            if (auto rbidElement = args.response.getValue().data["rbid"]) {
                int newRBID = rbidElement.numberInt();

                UniqueLock lk(_mutex);
                _setRBID_inlock(newRBID);
                lk.unlock();

                nextAction(Status::OK());
            } else {
                nextAction(Status(ErrorCodes::CommandFailed,
                                  "replSetGetRBID command failed when checking for rollback"));
            }
        },
        nextAction);
}

Status RollbackChecker::reset_sync() {
    // Default to an error in case the callback is not called in an error case.
    Status resetStatus = Status(ErrorCodes::CommandFailed, "RollbackChecker reset failed");
    auto cbh = reset([&resetStatus](const Status& status) { resetStatus = status; });
    _executor->wait(cbh);
    return resetStatus;
}

int RollbackChecker::getBaseRBID_forTest() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _baseRBID;
}

int RollbackChecker::getLastRBID_forTest() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lastRBID;
}

bool RollbackChecker::_checkForRollback_inlock(int remoteRBID) {
    _lastRBID = remoteRBID;
    return remoteRBID != _baseRBID;
}

RollbackChecker::CallbackHandle RollbackChecker::_scheduleGetRollbackId(
    const RemoteCommandCallbackFn& nextAction, const CallbackFn& errorFn) {
    executor::RemoteCommandRequest getRollbackIDReq(
        _syncSource, "admin", BSON("replSetGetRBID" << 1));
    auto cbh = _executor->scheduleRemoteCommand(getRollbackIDReq, nextAction);

    if (cbh.getStatus() == ErrorCodes::ShutdownInProgress) {
        return RollbackChecker::CallbackHandle();
    }
    if (!cbh.isOK()) {
        errorFn(cbh.getStatus());
        return RollbackChecker::CallbackHandle();
    }
    return cbh.getValue();
}

void RollbackChecker::_setRBID_inlock(int rbid) {
    _baseRBID = rbid;
    _lastRBID = rbid;
}

}  // namespace repl
}  // namespace mongo
