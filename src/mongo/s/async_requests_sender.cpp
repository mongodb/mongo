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
#include "mongo/db/server_parameters.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/transaction/router_session_runtime_state.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_EXPORT_SERVER_PARAMETER(AsyncRequestsSenderUseBaton, bool, true);

namespace {

// Maximum number of retries for network and replication notMaster errors (per host).
const int kMaxNumFailedHostRetryAttempts = 3;

}  // namespace

AsyncRequestsSender::AsyncRequestsSender(OperationContext* opCtx,
                                         executor::TaskExecutor* executor,
                                         StringData dbName,
                                         const std::vector<AsyncRequestsSender::Request>& requests,
                                         const ReadPreferenceSetting& readPreference,
                                         Shard::RetryPolicy retryPolicy)
    : _opCtx(opCtx),
      _executor(executor),
      _baton(opCtx),
      _db(dbName.toString()),
      _readPreference(readPreference),
      _retryPolicy(retryPolicy) {
    auto routerSession = RouterSessionRuntimeState::get(opCtx);

    for (const auto& request : requests) {
        auto cmdObj = request.cmdObj;
        if (routerSession) {
            auto& participant = routerSession->getOrCreateParticipant(request.shardId);
            cmdObj = participant.attachTxnFieldsIfNeeded(request.cmdObj);
        }

        _remotes.emplace_back(request.shardId, cmdObj);
    }

    // Initialize command metadata to handle the read preference.
    _metadataObj = readPreference.toContainingBSON();

    // Schedule the requests immediately.
    _scheduleRequests();
}

AsyncRequestsSender::~AsyncRequestsSender() {
    _cancelPendingRequests();

    // Wait on remaining callbacks to run.
    while (!done()) {
        next();
    }
}

AsyncRequestsSender::Response AsyncRequestsSender::next() {
    invariant(!done());

    // If needed, schedule requests for all remotes which had retriable errors.
    // If some remote had success or a non-retriable error, return it.
    boost::optional<Response> readyResponse;
    while (!(readyResponse = _ready())) {
        // Otherwise, wait for some response to be received.
        if (_interruptStatus.isOK()) {
            try {
                _makeProgress(_opCtx);
            } catch (const AssertionException& ex) {
                // If the operation is interrupted, we cancel outstanding requests and switch to
                // waiting for the (canceled) callbacks to finish without checking for interrupts.
                _interruptStatus = ex.toStatus();
                _cancelPendingRequests();
                continue;
            }
        } else {
            _makeProgress(nullptr);
        }
    }
    return *readyResponse;
}

void AsyncRequestsSender::stopRetrying() {
    _stopRetrying = true;
}

bool AsyncRequestsSender::done() {
    return std::all_of(
        _remotes.begin(), _remotes.end(), [](const RemoteData& remote) { return remote.done; });
}

void AsyncRequestsSender::_cancelPendingRequests() {
    _stopRetrying = true;

    // Cancel all outstanding requests so they return immediately.
    for (auto& remote : _remotes) {
        if (remote.cbHandle.isValid()) {
            _executor->cancel(remote.cbHandle);
        }
    }
}

boost::optional<AsyncRequestsSender::Response> AsyncRequestsSender::_ready() {
    if (!_stopRetrying) {
        _scheduleRequests();
    }

    // If we have baton requests, we want to process those before proceeding
    if (_batonRequests) {
        return boost::none;
    }

    // Check if any remote is ready.
    invariant(!_remotes.empty());
    for (auto& remote : _remotes) {
        if (remote.swResponse && !remote.done) {
            remote.done = true;
            if (remote.swResponse->isOK()) {
                invariant(remote.shardHostAndPort);
                return Response(std::move(remote.shardId),
                                std::move(remote.swResponse->getValue()),
                                std::move(*remote.shardHostAndPort));
            } else {
                // If _interruptStatus is set, promote CallbackCanceled errors to it.
                if (!_interruptStatus.isOK() &&
                    ErrorCodes::CallbackCanceled == remote.swResponse->getStatus().code()) {
                    remote.swResponse = _interruptStatus;
                }
                return Response(std::move(remote.shardId),
                                std::move(remote.swResponse->getStatus()),
                                std::move(remote.shardHostAndPort));
            }
        }
    }
    // No remotes were ready.
    return boost::none;
}

void AsyncRequestsSender::_scheduleRequests() {
    invariant(!_stopRetrying);
    // Schedule remote work on hosts for which we have not sent a request or need to retry.
    for (size_t i = 0; i < _remotes.size(); ++i) {
        auto& remote = _remotes[i];

        // First check if the remote had a retriable error, and if so, clear its response field so
        // it will be retried.
        if (remote.swResponse && !remote.done) {
            // We check both the response status and command status for a retriable error.
            Status status = remote.swResponse->getStatus();
            if (status.isOK()) {
                status = getStatusFromCommandResult(remote.swResponse->getValue().data);
            }

            if (!status.isOK()) {
                // There was an error with either the response or the command.
                auto shard = remote.getShard();
                if (!shard) {
                    remote.swResponse =
                        Status(ErrorCodes::ShardNotFound,
                               str::stream() << "Could not find shard " << remote.shardId);
                } else {
                    if (remote.shardHostAndPort) {
                        shard->updateReplSetMonitor(*remote.shardHostAndPort, status);
                    }
                    if (shard->isRetriableError(status.code(), _retryPolicy) &&
                        remote.retryCount < kMaxNumFailedHostRetryAttempts) {
                        LOG(1) << "Command to remote " << remote.shardId << " at host "
                               << *remote.shardHostAndPort
                               << " failed with retriable error and will be retried "
                               << causedBy(redact(status));
                        ++remote.retryCount;
                        remote.swResponse.reset();
                    }
                }
            }
        }

        // If the remote does not have a response or pending request, schedule remote work for it.
        if (!remote.swResponse && !remote.cbHandle.isValid()) {
            auto scheduleStatus = _scheduleRequest(i);
            if (!scheduleStatus.isOK()) {
                remote.swResponse = std::move(scheduleStatus);

                if (_baton) {
                    _batonRequests++;
                    _baton->schedule([this] { _batonRequests--; });
                }

                // Push a noop response to the queue to indicate that a remote is ready for
                // re-processing due to failure.
                _responseQueue.push(boost::none);
            }
        }
    }
}

Status AsyncRequestsSender::_scheduleRequest(size_t remoteIndex) {
    auto& remote = _remotes[remoteIndex];

    invariant(!remote.cbHandle.isValid());
    invariant(!remote.swResponse);

    Status resolveStatus = remote.resolveShardIdToHostAndPort(this, _readPreference);
    if (!resolveStatus.isOK()) {
        return resolveStatus;
    }

    executor::RemoteCommandRequest request(
        *remote.shardHostAndPort, _db, remote.cmdObj, _metadataObj, _opCtx);

    auto callbackStatus = _executor->scheduleRemoteCommand(
        request,
        [remoteIndex, this](const executor::TaskExecutor::RemoteCommandCallbackArgs& cbData) {
            if (_baton) {
                _batonRequests++;
                _baton->schedule([this] { _batonRequests--; });
            }

            _responseQueue.push(Job{cbData, remoteIndex});
        },
        _baton);
    if (!callbackStatus.isOK()) {
        return callbackStatus.getStatus();
    }

    remote.cbHandle = callbackStatus.getValue();
    return Status::OK();
}

// Passing opCtx means you'd like to opt into opCtx interruption.  During cleanup we actually don't.
void AsyncRequestsSender::_makeProgress(OperationContext* opCtx) {
    invariant(!opCtx || opCtx == _opCtx);

    boost::optional<Job> job;

    if (_baton) {
        // If we're using a baton, we peek the queue, and block on the baton if it's empty
        if (boost::optional<boost::optional<Job>> tryJob = _responseQueue.tryPop()) {
            job = std::move(*tryJob);
        } else {
            _baton->run(opCtx, boost::none);
        }
    } else {
        // Otherwise we block on the queue
        job = opCtx ? _responseQueue.pop(opCtx) : _responseQueue.pop();
    }

    if (!job) {
        return;
    }

    auto& remote = _remotes[job->remoteIndex];
    invariant(!remote.swResponse);

    // Clear the callback handle. This indicates that we are no longer waiting on a response from
    // 'remote'.
    remote.cbHandle = executor::TaskExecutor::CallbackHandle();

    // Store the response or error.
    if (job->cbData.response.status.isOK()) {
        remote.swResponse = std::move(job->cbData.response);

        if (auto routerSession = RouterSessionRuntimeState::get(opCtx)) {
            auto& participant = routerSession->getOrCreateParticipant(remote.shardId);
            participant.markAsCommandSent();
        }
    } else {
        // TODO: call participant.markAsCommandSent on "transaction already started" errors?
        remote.swResponse = std::move(job->cbData.response.status);
    }
}

AsyncRequestsSender::Request::Request(ShardId shardId, BSONObj cmdObj)
    : shardId(shardId), cmdObj(cmdObj) {}

AsyncRequestsSender::Response::Response(ShardId shardId,
                                        executor::RemoteCommandResponse response,
                                        HostAndPort hp)
    : shardId(std::move(shardId)),
      swResponse(std::move(response)),
      shardHostAndPort(std::move(hp)) {}

AsyncRequestsSender::Response::Response(ShardId shardId,
                                        Status status,
                                        boost::optional<HostAndPort> hp)
    : shardId(std::move(shardId)), swResponse(std::move(status)), shardHostAndPort(std::move(hp)) {}

AsyncRequestsSender::RemoteData::RemoteData(ShardId shardId, BSONObj cmdObj)
    : shardId(std::move(shardId)), cmdObj(std::move(cmdObj)) {}

Status AsyncRequestsSender::RemoteData::resolveShardIdToHostAndPort(
    AsyncRequestsSender* ars, const ReadPreferenceSetting& readPref) {
    const auto shard = getShard();
    if (!shard) {
        return Status(ErrorCodes::ShardNotFound,
                      str::stream() << "Could not find shard " << shardId);
    }

    auto clock = ars->_opCtx->getServiceContext()->getFastClockSource();

    auto deadline = clock->now() + Seconds(20);

    auto targeter = shard->getTargeter();

    auto findHostStatus = [&] {
        // If we don't have a baton, just go ahead and block in targeting
        if (!ars->_baton) {
            return targeter->findHostWithMaxWait(readPref, Seconds{20});
        }

        // If we do have a baton, and we can target quickly, just do that
        {
            auto findHostStatus = targeter->findHostNoWait(readPref);
            if (findHostStatus.isOK()) {
                return findHostStatus;
            }
        }

        // If it's going to take a while to target, we spin up a background thread to do our
        // targeting, while running the baton on the calling thread.  This allows us to make forward
        // progress on previous requests.
        auto pf = makePromiseFuture<HostAndPort>();

        ars->_batonRequests++;
        stdx::thread bgChecker([&] {
            pf.promise.setWith(
                [&] { return targeter->findHostWithMaxWait(readPref, deadline - clock->now()); });

            ars->_baton->schedule([ars] { ars->_batonRequests--; });
        });
        const auto guard = MakeGuard([&] { bgChecker.join(); });

        while (!pf.future.isReady()) {
            if (!ars->_baton->run(nullptr, deadline)) {
                break;
            }
        }

        return pf.future.getNoThrow();
    }();

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

AsyncRequestsSender::BatonDetacher::BatonDetacher(OperationContext* opCtx)
    : _baton(AsyncRequestsSenderUseBaton.load()
                 ? (opCtx->getServiceContext()->getTransportLayer()
                        ? opCtx->getServiceContext()->getTransportLayer()->makeBaton(opCtx)
                        : nullptr)
                 : nullptr) {}

AsyncRequestsSender::BatonDetacher::~BatonDetacher() {
    if (_baton) {
        _baton->detach();
    }
}

}  // namespace mongo
