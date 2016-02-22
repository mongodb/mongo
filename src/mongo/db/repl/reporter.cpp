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

#include "mongo/db/repl/reporter.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {

using executor::RemoteCommandRequest;

Reporter::Reporter(executor::TaskExecutor* executor,
                   PrepareReplSetUpdatePositionCommandFn prepareOldReplSetUpdatePositionCommandFn,
                   const HostAndPort& target)
    : _executor(executor),
      _prepareOldReplSetUpdatePositionCommandFn(prepareOldReplSetUpdatePositionCommandFn),
      _target(target),
      _status(Status::OK()),
      _willRunAgain(false),
      _active(false) {
    uassert(ErrorCodes::BadValue, "null task executor", executor);
    uassert(ErrorCodes::BadValue,
            "null function to create replSetUpdatePosition command object",
            prepareOldReplSetUpdatePositionCommandFn);
    uassert(ErrorCodes::BadValue, "target name cannot be empty", !target.empty());
}

Reporter::~Reporter() {
    DESTRUCTOR_GUARD(cancel(););
}

void Reporter::cancel() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (!_active) {
        return;
    }

    _status = Status(ErrorCodes::CallbackCanceled, "Reporter no longer valid");
    _willRunAgain = false;
    invariant(_remoteCommandCallbackHandle.isValid());
    _executor->cancel(_remoteCommandCallbackHandle);
}

void Reporter::wait() {
    executor::TaskExecutor::CallbackHandle handle;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (!_active) {
            return;
        }
        if (!_remoteCommandCallbackHandle.isValid()) {
            return;
        }
        handle = _remoteCommandCallbackHandle;
    }
    _executor->wait(handle);
}

Status Reporter::trigger() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _schedule_inlock();
}

Status Reporter::_schedule_inlock() {
    if (!_status.isOK()) {
        return _status;
    }

    if (_active) {
        _willRunAgain = true;
        return _status;
    }

    LOG(2) << "Reporter scheduling report to : " << _target;

    auto prepareResult = _prepareOldReplSetUpdatePositionCommandFn();

    if (!prepareResult.isOK()) {
        // Returning NodeNotFound because currently this is the only way
        // prepareOldReplSetUpdatePositionCommand() can fail in production.
        return Status(ErrorCodes::NodeNotFound,
                      "Reporter failed to create replSetUpdatePositionCommand command.");
    }
    auto cmdObj = prepareResult.getValue();
    StatusWith<executor::TaskExecutor::CallbackHandle> scheduleResult =
        _executor->scheduleRemoteCommand(
            RemoteCommandRequest(_target, "admin", cmdObj),
            stdx::bind(&Reporter::_callback, this, stdx::placeholders::_1));

    if (!scheduleResult.isOK()) {
        _status = scheduleResult.getStatus();
        LOG(2) << "Reporter failed to schedule with status: " << _status;

        return _status;
    }

    _active = true;
    _willRunAgain = false;
    _remoteCommandCallbackHandle = scheduleResult.getValue();
    return Status::OK();
}

void Reporter::_callback(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    _status = rcbd.response.getStatus();
    _active = false;

    LOG(2) << "Reporter ended with status: " << _status << " after reporting to " << _target;
    if (_status.isOK() && _willRunAgain) {
        _schedule_inlock();
    } else {
        _willRunAgain = false;
    }
}

Status Reporter::getStatus() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _status;
}

bool Reporter::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _active;
}

bool Reporter::willRunAgain() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _willRunAgain;
}
}  // namespace repl
}  // namespace mongo
