// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/reporter.h"

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

namespace {

// The number of replSetUpdatePosition commands a node sent to its sync source.
auto& numUpdatePosition = *MetricBuilder<Counter64>{"repl.network.replSetUpdatePosition.num"};

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
    try {
        shutdown();
        join().transitional_ignore();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

std::string Reporter::toString() const {
    return getTarget().toString();
}

HostAndPort Reporter::getTarget() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _target;
}

Milliseconds Reporter::getKeepAliveInterval() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _keepAliveInterval;
}

void Reporter::shutdown() {
    std::lock_guard<std::mutex> lk(_mutex);

    _status = Status(ErrorCodes::CallbackCanceled, "Reporter no longer valid");

    _requestWaitingStatus = RequestWaitingStatus::kNoWaiting;

    if (_isActive(lk)) {
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

    if (_isBackupActive(lk)) {
        executor::TaskExecutor::CallbackHandle handle;
        if (_backupRemoteCommandCallbackHandle.isValid()) {
            invariant(!_backupPrepareAndSendCommandCallbackHandle.isValid());
            handle = _backupRemoteCommandCallbackHandle;
        } else {
            invariant(!_backupRemoteCommandCallbackHandle.isValid());
            invariant(_backupPrepareAndSendCommandCallbackHandle.isValid());
            handle = _backupPrepareAndSendCommandCallbackHandle;
        }

        _executor->cancel(handle);
    }
}

Status Reporter::join() {
    std::unique_lock<std::mutex> lk(_mutex);
    _condition.wait(lk, [&]() { return !_isActive(lk) && !_isBackupActive(lk); });
    return _status;
}

Status Reporter::trigger(bool allowOneMore) {
    std::lock_guard<std::mutex> lk(_mutex);

    // If these was a previous error then the reporter is dead and return that error.
    if (!_status.isOK()) {
        return _status;
    }

    bool useBackupChannel = false;
    if (_keepAliveTimeoutWhen != Date_t()) {
        // Reset keep alive expiration to signal handler that it was canceled internally.
        invariant(_prepareAndSendCommandCallbackHandle.isValid());
        _keepAliveTimeoutWhen = Date_t();
        _executor->cancel(_prepareAndSendCommandCallbackHandle);
        return Status::OK();
    } else if (_isActive(lk)) {
        if (!allowOneMore) {
            // If it is already scheduled to be prioritized request, keep it as it is.
            if (_requestWaitingStatus == RequestWaitingStatus::kNoWaiting) {
                _requestWaitingStatus = RequestWaitingStatus::kNormalWaiting;
            }
            return Status::OK();
        } else if (_isBackupActive(lk)) {
            _requestWaitingStatus = RequestWaitingStatus::kPrioritizedWaiting;
            return Status::OK();
        } else {
            useBackupChannel = true;
        }
    }

    auto scheduleResult =
        _executor->scheduleWork([=, this](const executor::TaskExecutor::CallbackArgs& args) {
            _prepareAndSendCommandCallback(args, true, useBackupChannel);
        });

    _status = scheduleResult.getStatus();
    if (!_status.isOK()) {
        LOGV2_DEBUG(21585,
                    2,
                    "Reporter failed to schedule callback to prepare and send update command",
                    "error"_attr = _status);
        return _status;
    }

    if (!useBackupChannel) {
        _prepareAndSendCommandCallbackHandle = scheduleResult.getValue();
    } else {
        _backupPrepareAndSendCommandCallbackHandle = scheduleResult.getValue();
    }

    return _status;
}

StatusWith<BSONObj> Reporter::_prepareCommand() {
    auto prepareResult = _prepareReplSetUpdatePositionCommandFn();

    std::lock_guard<std::mutex> lk(_mutex);

    // Reporter could have been canceled while preparing the command.
    if (!_status.isOK()) {
        return _status;
    }

    // If there was an error in preparing the command, abort and return that error.
    if (!prepareResult.isOK()) {
        LOGV2_DEBUG(21586,
                    2,
                    "Reporter failed to prepare update command",
                    "error"_attr = prepareResult.getStatus());
        _status = prepareResult.getStatus();
        return _status;
    }

    return prepareResult.getValue();
}

void Reporter::_sendCommand(WithLock lk,
                            BSONObj commandRequest,
                            Milliseconds netTimeout,
                            bool useBackupChannel) {
    LOGV2_DEBUG(21587,
                2,
                "Reporter sending oplog progress to upstream updater",
                "target"_attr = _target,
                "commandRequest"_attr = commandRequest);

    auto scheduleResult = _executor->scheduleRemoteCommand(
        executor::RemoteCommandRequest(
            _target, DatabaseName::kAdmin, commandRequest, nullptr, netTimeout),
        [this, useBackupChannel](const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd) {
            _processResponseCallback(rcbd, useBackupChannel);
        });

    _status = scheduleResult.getStatus();
    if (!_status.isOK()) {
        LOGV2_DEBUG(21588, 2, "Reporter failed to schedule", "error"_attr = _status);
        if (_status != ErrorCodes::ShutdownInProgress) {
            fassert(34434, _status);
        }
        return;
    }

    numUpdatePosition.increment(1);

    if (useBackupChannel) {
        _backupRemoteCommandCallbackHandle = scheduleResult.getValue();
    } else {
        _remoteCommandCallbackHandle = scheduleResult.getValue();
    }
}

void Reporter::_processResponseCallback(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd, bool useBackupChannel) {
    {
        std::lock_guard<std::mutex> lk(_mutex);

        // If the reporter was shut down before this callback is invoked,
        // return the canceled "_status".
        if (!_status.isOK()) {
            _onShutdown(lk, useBackupChannel);
            return;
        }

        _status = rcbd.response.status;
        if (!_status.isOK()) {
            _onShutdown(lk, useBackupChannel);
            return;
        }

        // Override _status with the one embedded in the command result.
        const auto& commandResult = rcbd.response.data;
        _status = getStatusFromCommandResult(commandResult);

        if (!_status.isOK()) {
            _onShutdown(lk, useBackupChannel);
            return;
        }

        if (useBackupChannel &&
            _requestWaitingStatus != RequestWaitingStatus::kPrioritizedWaiting) {
            _backupRemoteCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
            return;
        }


        if (_requestWaitingStatus == RequestWaitingStatus::kNoWaiting) {
            // Since we are also on a timer, schedule a report for that interval, or until
            // triggered.
            auto when = _executor->now() + _keepAliveInterval;
            bool fromTrigger = false;
            auto scheduleResult = _executor->scheduleWorkAt(
                when, [=, this](const executor::TaskExecutor::CallbackArgs& args) {
                    _prepareAndSendCommandCallback(args, fromTrigger, false
                                                   /*useBackupChannel*/);
                });
            _status = scheduleResult.getStatus();
            if (!_status.isOK()) {
                _onShutdown(lk, useBackupChannel);
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

    // Since we unlock above, there is a chance that the main channel and backup channel reach this
    // point at about the same time and the updatePosition request would be almost the same. We may
    // save one request in that case but for now we leave it for future optimization.
    std::lock_guard<std::mutex> lk(_mutex);
    if (!_status.isOK()) {
        _onShutdown(lk, useBackupChannel);
        return;
    }

    _sendCommand(lk, prepareResult.getValue(), _updatePositionTimeout, useBackupChannel);
    if (!_status.isOK()) {
        _onShutdown(lk, useBackupChannel);
        return;
    }

    auto& remoteCommandCallbackHandle =
        useBackupChannel ? _backupRemoteCommandCallbackHandle : _remoteCommandCallbackHandle;
    invariant(remoteCommandCallbackHandle.isValid());
    _requestWaitingStatus = RequestWaitingStatus::kNoWaiting;
}

void Reporter::_prepareAndSendCommandCallback(const executor::TaskExecutor::CallbackArgs& args,
                                              bool fromTrigger,
                                              bool useBackupChannel) {
    {
        std::lock_guard<std::mutex> lk(_mutex);
        if (!_status.isOK()) {
            _onShutdown(lk, useBackupChannel);
            return;
        }

        _status = args.status;

        // Ignore CallbackCanceled status if keep alive was canceled by triggered.
        if (!fromTrigger && _status == ErrorCodes::CallbackCanceled &&
            _keepAliveTimeoutWhen == Date_t()) {
            _status = Status::OK();
        }

        if (!_status.isOK()) {
            _onShutdown(lk, useBackupChannel);
            return;
        }
    }

    // Must call without holding the lock.
    auto prepareResult = _prepareCommand();

    std::lock_guard<std::mutex> lk(_mutex);
    if (!_status.isOK()) {
        _onShutdown(lk, useBackupChannel);
        return;
    }

    _sendCommand(lk, prepareResult.getValue(), _updatePositionTimeout, useBackupChannel);
    if (!_status.isOK()) {
        _onShutdown(lk, useBackupChannel);
        return;
    }

    auto& remoteCommandCallbackHandle =
        useBackupChannel ? _backupRemoteCommandCallbackHandle : _remoteCommandCallbackHandle;
    auto& prepareAndSendCommandCallbackHandle = useBackupChannel
        ? _backupPrepareAndSendCommandCallbackHandle
        : _prepareAndSendCommandCallbackHandle;
    invariant(remoteCommandCallbackHandle.isValid());
    prepareAndSendCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
    if (!useBackupChannel) {
        // Only reset keepAliveTimeout when it is triggered for the main channel.
        _keepAliveTimeoutWhen = Date_t();
    }
}

void Reporter::_onShutdown(WithLock lk, bool useBackupChannel) {
    _requestWaitingStatus = RequestWaitingStatus::kNoWaiting;
    if (!useBackupChannel) {
        _remoteCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
        _prepareAndSendCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
        _keepAliveTimeoutWhen = Date_t();
    } else {
        _backupRemoteCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
        _backupPrepareAndSendCommandCallbackHandle = executor::TaskExecutor::CallbackHandle();
    }
    _condition.notify_all();
}

bool Reporter::isActive() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _isActive(lk);
}

bool Reporter::isBackupActive() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _isBackupActive(lk);
}

bool Reporter::_isActive(WithLock lk) const {
    return _remoteCommandCallbackHandle.isValid() || _prepareAndSendCommandCallbackHandle.isValid();
}

bool Reporter::_isBackupActive(WithLock lk) const {
    return _backupRemoteCommandCallbackHandle.isValid() ||
        _backupPrepareAndSendCommandCallbackHandle.isValid();
}

bool Reporter::isWaitingToSendReport() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _requestWaitingStatus != RequestWaitingStatus::kNoWaiting;
}

Date_t Reporter::getKeepAliveTimeoutWhen_forTest() const {
    std::lock_guard<std::mutex> lk(_mutex);
    return _keepAliveTimeoutWhen;
}

Status Reporter::getStatus_forTest() const {
    return _status;
}

}  // namespace repl
}  // namespace mongo
