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


#include "mongo/db/repl/reporter.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/repl/update_position_args.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

// The number of replSetUpdatePosition commands a node sent to its sync source.
CounterMetric numUpdatePosition("repl.network.replSetUpdatePosition.num");

}  // namespace

Reporter::Reporter(executor::TaskExecutor* executor,
                   PrepareReplSetUpdatePositionCommandFn prepareReplSetUpdatePositionCommandFn,
                   const HostAndPort& target,
                   Milliseconds keepAliveInterval,
                   Milliseconds updatePositionTimeout)
    : _executor(executor),
      _prepareReplSetUpdatePositionCommandFn(prepareReplSetUpdatePositionCommandFn),
      _target(target),
      _keepAliveInterval(keepAliveInterval),
      _updatePositionTimeout(updatePositionTimeout) {
    uassert(ErrorCodes::BadValue, "null task executor", executor);
    uassert(ErrorCodes::BadValue,
            "null function to create replSetUpdatePosition command object",
            prepareReplSetUpdatePositionCommandFn);
    uassert(ErrorCodes::BadValue, "target name cannot be empty", !target.empty());
    uassert(ErrorCodes::BadValue,
            "keep alive interval must be positive",
            keepAliveInterval > Milliseconds(0));
    uassert(ErrorCodes::BadValue,
            "update position timeout must be positive",
            updatePositionTimeout > Milliseconds(0));
}

Reporter::~Reporter() {
    DESTRUCTOR_GUARD(shutdown(); join().transitional_ignore(););
}

std::string Reporter::toString() const {
    return getTarget().toString();
}

HostAndPort Reporter::getTarget() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _target;
}

Milliseconds Reporter::getKeepAliveInterval() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _keepAliveInterval;
}

void Reporter::shutdown() {
    stdx::lock_guard<Latch> lk(_mutex);

    _status = Status(ErrorCodes::CallbackCanceled, "Reporter no longer valid");

    if (!_isActive_inlock()) {
        return;
    }

    _isWaitingToSendReporter = false;

    executor::TaskExecutor::CallbackHandle handle;
    if (_remoteCommandCallbackHandle.isValid()) {
        invariant(!_prepareAndSendCommandCallbackHandle.isValid());
        handle = _remoteCommandCallbackHandle;
    } else {
        invariant(!_remoteCommandCallbackHandle.isValid());
        invariant(_prepareAndSendCommandCallbackHandle.isValid());
        handle = _prepareAndSendCommandCallbackHandle;
    }

    _executor->cancel(handle);
}

Status Reporter::join() {
    stdx::unique_lock<Latch> lk(_mutex);
    _condition.wait(lk, [this]() { return !_isActive_inlock(); });
    return _status;
}

Status Reporter::trigger() {
    stdx::lock_guard<Latch> lk(_mutex);

    // If these was a previous error then the reporter is dead and return that error.
    if (!_status.isOK()) {
        return _status;
    }

    if (_keepAliveTimeoutWhen != Date_t()) {
        // Reset keep alive expiration to signal handler that it was canceled internally.
        invariant(_prepareAndSendCommandCallbackHandle.isValid());
        _keepAliveTimeoutWhen = Date_t();
        _executor->cancel(_prepareAndSendCommandCallbackHandle);
        return Status::OK();
    } else if (_isActive_inlock()) {
        _isWaitingToSendReporter = true;
        return Status::OK();
    }

    auto scheduleResult =
        _executor->scheduleWork([=](const executor::TaskExecutor::CallbackArgs& args) {
            _prepareAndSendCommandCallback(args, true);
        });

    _status = scheduleResult.getStatus();
    if (!_status.isOK()) {
        LOGV2_DEBUG(
            21585,
            2,
            "Reporter failed to schedule callback to prepare and send update command: {error}",
            "Reporter failed to schedule callback to prepare and send update command",
            "error"_attr = _status);
        return _status;
    }

    _prepareAndSendCommandCallbackHandle = scheduleResult.getValue();

    return _status;
}

StatusWith<BSONObj> Reporter::_prepareCommand() {
    auto prepareResult = _prepareReplSetUpdatePositionCommandFn();

    stdx::lock_guard<Latch> lk(_mutex);

    // Reporter could have been canceled while preparing the command.
    if (!_status.isOK()) {
        return _status;
    }

    // If there was an error in preparing the command, abort and return that error.
    if (!prepareResult.isOK()) {
        LOGV2_DEBUG(21586,
                    2,
                    "Reporter failed to prepare update command with status: {error}",
                    "Reporter failed to prepare update command",
                    "error"_attr = prepareResult.getStatus());
        _status = prepareResult.getStatus();
        return _status;
    }

    return prepareResult.getValue();
}

void Reporter::_sendCommand_inlock(BSONObj commandRequest, Milliseconds netTimeout) {
    LOGV2_DEBUG(21587,
                2,
                "Reporter sending oplog progress to upstream updater {target}: {commandRequest}",
                "Reporter sending oplog progress to upstream updater",
                "target"_attr = _target,
                "commandRequest"_attr = commandRequest);

    auto scheduleResult = _executor->scheduleRemoteCommand(
        executor::RemoteCommandRequest(_target, "admin", commandRequest, nullptr, netTimeout),
        [this](const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd) {
            _processResponseCallback(rcbd);
        });

    _status = scheduleResult.getStatus();
    if (!_status.isOK()) {
        LOGV2_DEBUG(21588,
                    2,
                    "Reporter failed to schedule with status: {error}",
                    "Reporter failed to schedule",
                    "error"_attr = _status);
        if (_status != ErrorCodes::ShutdownInProgress) {
            fassert(34434, _status);
        }
        return;
    }

    numUpdatePosition.increment(1);

    _remoteCommandCallbackHandle = scheduleResult.getValue();
}

void Reporter::_processResponseCallback(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd) {
    {
        stdx::lock_guard<Latch> lk(_mutex);

        // If the reporter was shut down before this callback is invoked,
        // return the canceled "_status".
        if (!_status.isOK()) {
            invariant(_status == ErrorCodes::CallbackCanceled);
            _onShutdown_inlock();
            return;
        }

        _status = rcbd.response.status;
        if (!_status.isOK()) {
            _onShutdown_inlock();
            return;
        }

        // Override _status with the one embedded in the command result.
        const auto& commandResult = rcbd.response.data;
        _status = getStatusFromCommandResult(commandResult);

        if (!_status.isOK()) {
            _onShutdown_inlock();
            return;
        }

        if (!_isWaitingToSendReporter) {
            // Since we are also on a timer, schedule a report for that interval, or until
            // triggered.
            auto when = _executor->now() + _keepAliveInterval;
            bool fromTrigger = false;
            auto scheduleResult = _executor->scheduleWorkAt(
                when, [=](const executor::TaskExecutor::CallbackArgs& args) {
                    _prepareAndSendCommandCallback(args, fromTrigger);
                });
            _status = scheduleResult.getStatus();
            if (!_status.isOK()) {
                _onShutdown_inlock();
                return;
            }

            _prepareAndSendCommandCallbackHandle = scheduleResult.getValue();
            _keepAliveTimeoutWhen = when;

            _remoteCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
            return;
        }
    }

    // Must call without holding the lock.
    auto prepareResult = _prepareCommand();

    stdx::lock_guard<Latch> lk(_mutex);
    if (!_status.isOK()) {
        _onShutdown_inlock();
        return;
    }

    _sendCommand_inlock(prepareResult.getValue(), _updatePositionTimeout);
    if (!_status.isOK()) {
        _onShutdown_inlock();
        return;
    }

    invariant(_remoteCommandCallbackHandle.isValid());
    _isWaitingToSendReporter = false;
}

void Reporter::_prepareAndSendCommandCallback(const executor::TaskExecutor::CallbackArgs& args,
                                              bool fromTrigger) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (!_status.isOK()) {
            _onShutdown_inlock();
            return;
        }

        _status = args.status;

        // Ignore CallbackCanceled status if keep alive was canceled by triggered.
        if (!fromTrigger && _status == ErrorCodes::CallbackCanceled &&
            _keepAliveTimeoutWhen == Date_t()) {
            _status = Status::OK();
        }

        if (!_status.isOK()) {
            _onShutdown_inlock();
            return;
        }
    }

    // Must call without holding the lock.
    auto prepareResult = _prepareCommand();

    stdx::lock_guard<Latch> lk(_mutex);
    if (!_status.isOK()) {
        _onShutdown_inlock();
        return;
    }

    _sendCommand_inlock(prepareResult.getValue(), _updatePositionTimeout);
    if (!_status.isOK()) {
        _onShutdown_inlock();
        return;
    }

    invariant(_remoteCommandCallbackHandle.isValid());
    _prepareAndSendCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
    _keepAliveTimeoutWhen = Date_t();
}

void Reporter::_onShutdown_inlock() {
    _isWaitingToSendReporter = false;
    _remoteCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
    _prepareAndSendCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
    _keepAliveTimeoutWhen = Date_t();
    _condition.notify_all();
}

bool Reporter::isActive() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isActive_inlock();
}

bool Reporter::_isActive_inlock() const {
    return _remoteCommandCallbackHandle.isValid() || _prepareAndSendCommandCallbackHandle.isValid();
}

bool Reporter::isWaitingToSendReport() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _isWaitingToSendReporter;
}

Date_t Reporter::getKeepAliveTimeoutWhen_forTest() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _keepAliveTimeoutWhen;
}

Status Reporter::getStatus_forTest() const {
    return _status;
}

}  // namespace repl
}  // namespace mongo
