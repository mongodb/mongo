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


#include "mongo/platform/basic.h"

#include "mongo/s/async_requests_sender.h"

#include <fmt/format.h>
#include <memory>

#include "mongo/client/remote_command_targeter.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/hedge_options_util.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


using namespace fmt::literals;

namespace mongo {

namespace {

// Maximum number of retries for network and replication NotPrimary errors (per host).
const int kMaxNumFailedHostRetryAttempts = 3;

MONGO_FAIL_POINT_DEFINE(hangBeforeSchedulingRemoteCommand);
MONGO_FAIL_POINT_DEFINE(hangBeforePollResponse);

}  // namespace

AsyncRequestsSender::AsyncRequestsSender(OperationContext* opCtx,
                                         std::shared_ptr<executor::TaskExecutor> executor,
                                         StringData dbName,
                                         const std::vector<AsyncRequestsSender::Request>& requests,
                                         const ReadPreferenceSetting& readPreference,
                                         Shard::RetryPolicy retryPolicy,
                                         std::unique_ptr<ResourceYielder> resourceYielder)
    : _opCtx(opCtx),
      _db(dbName.toString()),
      _readPreference(readPreference),
      _retryPolicy(retryPolicy),
      _subExecutor(std::move(executor)),
      _subBaton(opCtx->getBaton()->makeSubBaton()),
      _resourceYielder(std::move(resourceYielder)) {

    _remotesLeft = requests.size();

    // Initialize command metadata to handle the read preference.
    _metadataObj = readPreference.toContainingBSON();

    _remotes.reserve(requests.size());
    for (const auto& request : requests) {
        // Kick off requests immediately.
        _remotes.emplace_back(this, request.shardId, request.cmdObj).executeRequest();
    }
}

AsyncRequestsSender::Response AsyncRequestsSender::next() noexcept {
    invariant(!done());

    hangBeforePollResponse.executeIf(
        [&](const BSONObj& data) {
            while (MONGO_unlikely(hangBeforePollResponse.shouldFail())) {
                LOGV2(4840900, "Hanging in ARS::next due to 'hangBeforePollResponse' failpoint");
                sleepmillis(100);
            }
        },
        [&](const BSONObj& data) {
            return MONGO_unlikely(_remotesLeft == (size_t)data.getIntField("remotesLeft"));
        });

    _remotesLeft--;

    // If we've been interrupted, the response queue should be filled with interrupted answers, go
    // ahead and return one of those
    if (!_interruptStatus.isOK()) {
        return _responseQueue.pop();
    }

    // Try to pop a value from the queue
    try {
        if (_resourceYielder) {
            _resourceYielder->yield(_opCtx);
        }

        // Only wait for the next result without popping it, so an error unyielding doesn't
        // discard an already popped response.
        auto waitStatus = _responseQueue.waitForNonEmptyNoThrow(_opCtx);

        auto unyieldStatus =
            _resourceYielder ? _resourceYielder->unyieldNoThrow(_opCtx) : Status::OK();

        uassertStatusOK(waitStatus);
        uassertStatusOK(unyieldStatus);

        // There should always be a response ready after the wait above.
        auto response = _responseQueue.tryPop();
        invariant(response);
        return *response;
    } catch (const DBException& ex) {
        // If we're interrupted, save that value and overwrite all outstanding requests (that we're
        // not going to wait to collect)
        _interruptStatus = ex.toStatus();
    }

    // Make failed responses for all outstanding remotes with the interruption status and push them
    // onto the response queue
    for (auto& remote : _remotes) {
        if (!remote) {
            _responseQueue.push(std::move(remote).makeFailedResponse(_interruptStatus));
        }
    }

    // Stop servicing callbacks
    _subBaton.shutdown();

    // shutdown the scoped task executor
    _subExecutor->shutdown();

    return _responseQueue.pop();
}

void AsyncRequestsSender::stopRetrying() noexcept {
    _stopRetrying = true;
}

bool AsyncRequestsSender::done() noexcept {
    return !_remotesLeft;
}

AsyncRequestsSender::Request::Request(ShardId shardId, BSONObj cmdObj)
    : shardId(shardId), cmdObj(cmdObj) {}

AsyncRequestsSender::RemoteData::RemoteData(AsyncRequestsSender* ars,
                                            ShardId shardId,
                                            BSONObj cmdObj)
    : _ars(ars), _shardId(std::move(shardId)), _cmdObj(std::move(cmdObj)) {}

SemiFuture<std::shared_ptr<Shard>> AsyncRequestsSender::RemoteData::getShard() noexcept {
    return Grid::get(getGlobalServiceContext())
        ->shardRegistry()
        ->getShard(*_ars->_subBaton, _shardId);
}

void AsyncRequestsSender::RemoteData::executeRequest() {
    scheduleRequest()
        .thenRunOn(*_ars->_subBaton)
        .getAsync([this](StatusWith<RemoteCommandOnAnyCallbackArgs> rcr) {
            _done = true;
            if (rcr.isOK()) {
                _ars->_responseQueue.push(
                    {std::move(_shardId), rcr.getValue().response, std::move(_shardHostAndPort)});
            } else {
                _ars->_responseQueue.push(
                    {std::move(_shardId), rcr.getStatus(), std::move(_shardHostAndPort)});
            }
        });
}

auto AsyncRequestsSender::RemoteData::scheduleRequest()
    -> SemiFuture<RemoteCommandOnAnyCallbackArgs> {
    return getShard()
        .thenRunOn(*_ars->_subBaton)
        .then([this](auto&& shard) {
            return shard->getTargeter()->findHosts(_ars->_readPreference,
                                                   CancellationToken::uncancelable());
        })
        .thenRunOn(*_ars->_subBaton)
        .then([this](auto&& hostAndPorts) {
            _shardHostAndPort.emplace(hostAndPorts.front());
            return scheduleRemoteCommand(std::move(hostAndPorts));
        })
        .then([this](auto&& rcr) { return handleResponse(std::move(rcr)); })
        .semi();
}

auto AsyncRequestsSender::RemoteData::scheduleRemoteCommand(std::vector<HostAndPort>&& hostAndPorts)
    -> SemiFuture<RemoteCommandOnAnyCallbackArgs> {
    hangBeforeSchedulingRemoteCommand.executeIf(
        [&](const BSONObj& data) {
            while (MONGO_unlikely(hangBeforeSchedulingRemoteCommand.shouldFail())) {
                LOGV2(4625505,
                      "Hanging in ARS due to "
                      "'hangBeforeSchedulingRemoteCommand' failpoint");
                sleepmillis(100);
            }
        },
        [&](const BSONObj& data) {
            return MONGO_unlikely(std::count(hostAndPorts.begin(),
                                             hostAndPorts.end(),
                                             HostAndPort(data.getStringField("hostAndPort"))));
        });

    executor::RemoteCommandRequestOnAny::Options options;
    extractHedgeOptions(_cmdObj, _ars->_readPreference, options);
    executor::RemoteCommandRequestOnAny request(
        std::move(hostAndPorts), _ars->_db, _cmdObj, _ars->_metadataObj, _ars->_opCtx, options);

    // We have to make a promise future pair because the TaskExecutor doesn't currently support a
    // future returning variant of scheduleRemoteCommand
    auto [p, f] = makePromiseFuture<RemoteCommandOnAnyCallbackArgs>();

    // Failures to schedule skip the retry loop
    uassertStatusOK(_ars->_subExecutor->scheduleRemoteCommandOnAny(
        request,
        // We have to make a shared_ptr<Promise> here because scheduleRemoteCommand requires
        // copyable callbacks
        [p = std::make_shared<Promise<RemoteCommandOnAnyCallbackArgs>>(std::move(p))](
            const RemoteCommandOnAnyCallbackArgs& cbData) { p->emplaceValue(cbData); },
        *_ars->_subBaton));

    return std::move(f).semi();
}


auto AsyncRequestsSender::RemoteData::handleResponse(RemoteCommandOnAnyCallbackArgs&& rcr)
    -> SemiFuture<RemoteCommandOnAnyCallbackArgs> {
    if (rcr.response.target) {
        _shardHostAndPort = rcr.response.target;
    }

    auto status = rcr.response.status;

    if (status.isOK()) {
        status = getStatusFromCommandResult(rcr.response.data);
    }

    if (status.isOK()) {
        status = getWriteConcernStatusFromCommandResult(rcr.response.data);
    }

    // If we're okay (RemoteCommandResponse, command result and write concern)-wise we're done.
    // Otherwise check for retryability
    if (status.isOK()) {
        return std::move(rcr);
    }

    // There was an error with either the response or the command.
    return getShard()
        .thenRunOn(*_ars->_subBaton)
        .then([this, status = std::move(status), rcr = std::move(rcr)](
                  std::shared_ptr<mongo::Shard>&& shard) {
            std::vector<HostAndPort> failedTargets;

            if (rcr.response.target) {
                failedTargets = {*rcr.response.target};
            } else {
                failedTargets = rcr.request.target;
            }

            shard->updateReplSetMonitor(failedTargets.front(), status);
            bool isStartingTransaction = _cmdObj.getField("startTransaction").booleanSafe();
            if (!_ars->_stopRetrying &&
                shard->isRetriableError(status.code(), _ars->_retryPolicy) &&
                _retryCount < kMaxNumFailedHostRetryAttempts && !isStartingTransaction) {

                LOGV2_DEBUG(
                    4615637,
                    1,
                    "Command to remote {shardId} for hosts {hosts} failed with retryable error "
                    "{error} and will be retried",
                    "Command to remote shard failed with retryable error and will be retried",
                    "shardId"_attr = _shardId,
                    "hosts"_attr = failedTargets,
                    "error"_attr = redact(status));
                ++_retryCount;
                _shardHostAndPort.reset();
                // retry through recursion
                return scheduleRequest();
            }

            // Status' in the response.status field that aren't retried get converted to top level
            // errors
            uassertStatusOK(rcr.response.status);

            // We're not okay (on the remote), but still not going to retry
            return Future<RemoteCommandOnAnyCallbackArgs>::makeReady(std::move(rcr)).semi();
        })
        .semi();
};

}  // namespace mongo
