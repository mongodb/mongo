/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/s/async_requests_sender.h"

#include "mongo/client/remote_command_targeter.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

// Maximum number of retries for network and replication notMaster errors (per host).
const int kMaxNumFailedHostRetryAttempts = 3;

}  // namespace

AsyncRequestsSender::AsyncRequestsSender(OperationContext* txn,
                                         executor::TaskExecutor* executor,
                                         std::string db,
                                         const std::vector<AsyncRequestsSender::Request>& requests,
                                         const ReadPreferenceSetting& readPreference,
                                         bool allowPartialResults)
    : _executor(executor),
      _db(std::move(db)),
      _readPreference(readPreference),
      _allowPartialResults(allowPartialResults) {
    for (const auto& request : requests) {
        _remotes.emplace_back(request.shardId, request.cmdObj);
    }

    // Initialize command metadata to handle the read preference.
    BSONObjBuilder metadataBuilder;
    rpc::ServerSelectionMetadata metadata(_readPreference.pref != ReadPreference::PrimaryOnly,
                                          boost::none);
    uassertStatusOK(metadata.writeToMetadata(&metadataBuilder));
    _metadataObj = metadataBuilder.obj();

    // Schedule the requests immediately.
    _scheduleRequestsIfNeeded(txn);
}

AsyncRequestsSender::~AsyncRequestsSender() {
    invariant(_done());
}

std::vector<AsyncRequestsSender::Response> AsyncRequestsSender::waitForResponses(
    OperationContext* txn) {
    // Until all remotes have received a response or error, keep scheduling retries and waiting on
    // outstanding requests.
    while (!_done()) {
        _notification->get();

        // Note: if we have been interrupt()'d or if some remote had a non-retriable error and
        // allowPartialResults is false, no retries will be scheduled.
        _scheduleRequestsIfNeeded(txn);
    }

    // Construct the responses.
    std::vector<Response> responses;
    for (const auto& remote : _remotes) {
        invariant(remote.swResponse);
        if (remote.swResponse->isOK()) {
            invariant(remote.shardHostAndPort);
            responses.emplace_back(remote.swResponse->getValue(), *remote.shardHostAndPort);
        } else {
            responses.emplace_back(remote.swResponse->getStatus());
        }
    }
    return responses;
}

void AsyncRequestsSender::interrupt() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stopRetrying = true;
}

void AsyncRequestsSender::kill() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _stopRetrying = true;

    // Cancel all outstanding requests so they return immediately.
    for (auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            _executor->cancel(remote.cbHandle);
        }
    }
}

bool AsyncRequestsSender::_done() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _done_inlock();
}

bool AsyncRequestsSender::_done_inlock() {
    for (const auto& remote : _remotes) {
        if (!remote.swResponse) {
            return false;
        }
    }
    return true;
}

/*
 * Note: If _scheduleRequestsIfNeeded() does retries, only the remotes with retriable errors will be
 * rescheduled because:
 *
 * 1. Other pending remotes still have callback assigned to them.
 * 2. Remotes that already successfully received a response will have a non-empty 'response'.
 * 3. Remotes that have reached maximum retries will have an error status.
 */
void AsyncRequestsSender::_scheduleRequestsIfNeeded(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // We can't make a new notification if there was a previous one that has not been signaled.
    invariant(!_notification || *_notification);

    if (_done_inlock()) {
        return;
    }

    _notification.emplace();

    if (_stopRetrying) {
        return;
    }

    // Schedule remote work on hosts for which we have not sent a request or need to retry.
    for (size_t i = 0; i < _remotes.size(); ++i) {
        auto& remote = _remotes[i];

        // If we have not yet received a response or error for this remote, and we do not have an
        // outstanding request for this remote, schedule remote work to send the command.
        if (!remote.swResponse && !remote.cbHandle.isValid()) {
            auto scheduleStatus = _scheduleRequest_inlock(txn, i);
            if (!scheduleStatus.isOK()) {
                // Being unable to schedule a request to a remote is a non-retriable error.
                remote.swResponse = std::move(scheduleStatus);

                // If partial results are not allowed, stop scheduling requests on other remotes and
                // just wait for outstanding requests to come back.
                if (!_allowPartialResults) {
                    _stopRetrying = true;
                    break;
                }
            }
        }
    }
}

Status AsyncRequestsSender::_scheduleRequest_inlock(OperationContext* txn, size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];

    invariant(!remote.cbHandle.isValid());
    invariant(!remote.swResponse);

    Status resolveStatus = remote.resolveShardIdToHostAndPort(_readPreference);
    if (!resolveStatus.isOK()) {
        return resolveStatus;
    }

    executor::RemoteCommandRequest request(
        remote.getTargetHost(), _db, remote.cmdObj, _metadataObj, txn);

    auto callbackStatus = _executor->scheduleRemoteCommand(
        request,
        stdx::bind(
            &AsyncRequestsSender::_handleResponse, this, stdx::placeholders::_1, txn, remoteIndex));
    if (!callbackStatus.isOK()) {
        return callbackStatus.getStatus();
    }

    remote.cbHandle = callbackStatus.getValue();
    return Status::OK();
}

void AsyncRequestsSender::_handleResponse(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData,
    OperationContext* txn,
    size_t remoteIndex) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    auto& remote = _remotes[remoteIndex];
    invariant(!remote.swResponse);

    // Clear the callback handle. This indicates that we are no longer waiting on a response from
    // 'remote'.
    remote.cbHandle = executor::TaskExecutor::CallbackHandle();

    // On early return from this point on, signal anyone waiting on the current notification if
    // _done() is true, since this might be the last outstanding request.
    ScopeGuard signaller =
        MakeGuard(&AsyncRequestsSender::_signalCurrentNotificationIfDone_inlock, this);

    // We check both the response status and command status for a retriable error.
    Status status = cbData.response.status;
    if (status.isOK()) {
        status = getStatusFromCommandResult(cbData.response.data);
        if (status.isOK()) {
            remote.swResponse = std::move(cbData.response);
            return;
        }
    }

    // There was an error with either the response or the command.

    auto shard = remote.getShard();
    if (!shard) {
        remote.swResponse =
            Status(ErrorCodes::ShardNotFound,
                   str::stream() << "Could not find shard " << remote.shardId << " containing host "
                                 << remote.getTargetHost().toString());
        return;
    }
    shard->updateReplSetMonitor(remote.getTargetHost(), status);

    if (shard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent) && !_stopRetrying &&
        remote.retryCount < kMaxNumFailedHostRetryAttempts) {
        LOG(1) << "Command to remote " << remote.shardId << " at host " << *remote.shardHostAndPort
               << " failed with retriable error and will be retried" << causedBy(redact(status));
        ++remote.retryCount;

        // Even if _done() is not true, signal the thread sleeping in waitForResponses() to make
        // it schedule a retry for this remote without waiting for all outstanding requests to
        // come back.
        signaller.Dismiss();
        _signalCurrentNotification_inlock();
    } else {
        // Non-retriable error, out of retries, or _stopRetrying is true.

        // Even though we examined the command status to check for retriable errors, we just return
        // the response or response status here. It is up to the caller to parse the response as
        // a command result.
        if (cbData.response.status.isOK()) {
            remote.swResponse = std::move(cbData.response);
        } else {
            remote.swResponse = std::move(cbData.response.status);
        }

        // If the caller can't use partial results, there's no point continuing to retry on
        // retriable errors for other remotes.
        if (!_allowPartialResults) {
            _stopRetrying = true;
        }
    }
}

void AsyncRequestsSender::_signalCurrentNotification_inlock() {
    // Only signal the notification if it has not already been signalled.
    if (!*_notification) {
        _notification->set();
    }
}

void AsyncRequestsSender::_signalCurrentNotificationIfDone_inlock() {
    if (_done_inlock()) {
        _signalCurrentNotification_inlock();
    }
}

AsyncRequestsSender::Request::Request(ShardId shardId, BSONObj cmdObj)
    : shardId(shardId), cmdObj(cmdObj) {}

AsyncRequestsSender::Response::Response(executor::RemoteCommandResponse response, HostAndPort hp)
    : swResponse(response), shardHostAndPort(hp) {}

AsyncRequestsSender::Response::Response(Status status) : swResponse(status) {}

AsyncRequestsSender::RemoteData::RemoteData(ShardId shardId, BSONObj cmdObj)
    : shardId(std::move(shardId)), cmdObj(std::move(cmdObj)) {}

const HostAndPort& AsyncRequestsSender::RemoteData::getTargetHost() const {
    invariant(shardHostAndPort);
    return *shardHostAndPort;
}

Status AsyncRequestsSender::RemoteData::resolveShardIdToHostAndPort(
    const ReadPreferenceSetting& readPref) {
    const auto shard = getShard();
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Could not find shard " << shardId);
    }

    auto findHostStatus = shard->getTargeter()->findHostWithMaxWait(readPref, Seconds{20});
    if (!findHostStatus.isOK()) {
        return findHostStatus.getStatus();
    }

    shardHostAndPort = std::move(findHostStatus.getValue());

    return Status::OK();
}

std::shared_ptr<Shard> AsyncRequestsSender::RemoteData::getShard() {
    // TODO: Pass down an OperationContext* to use here.
    return grid.shardRegistry()->getShardNoReload(shardId);
}

}  // namespace mongo
