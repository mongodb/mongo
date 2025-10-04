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


#include "mongo/s/async_requests_sender.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/transaction_participant_failed_unyield_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <tuple>
#include <type_traits>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforePollResponse);
MONGO_FAIL_POINT_DEFINE(hangAfterYield);

}  // namespace

AsyncRequestsSender::AsyncRequestsSender(OperationContext* opCtx,
                                         std::shared_ptr<executor::TaskExecutor> executor,
                                         const DatabaseName& dbName,
                                         const std::vector<AsyncRequestsSender::Request>& requests,
                                         const ReadPreferenceSetting& readPreference,
                                         Shard::RetryPolicy retryPolicy,
                                         std::unique_ptr<ResourceYielder> resourceYielder,
                                         const ShardHostMap& designatedHostsMap)
    : _opCtx(opCtx),
      _db(dbName),
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
        auto designatedHostIter = designatedHostsMap.find(request.shardId);
        auto designatedHost = designatedHostIter != designatedHostsMap.end()
            ? designatedHostIter->second
            : HostAndPort();
        _remotes
            .emplace_back(
                this, request.shardId, request.cmdObj, std::move(designatedHost), request.shard)
            .executeRequest();
    }

    CurOp::get(_opCtx)->ensureRecordRemoteOpWait();
}

AsyncRequestsSender::Response AsyncRequestsSender::next() {
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

    auto popResponseAfterInterrupt = [&] {
        invariant(!_interruptStatus.isOK());
        // If we have been interrupted, the response queue should be populated with responses
        // already, go ahead and return one of those.
        auto response = _responseQueue.pop();
        if (_failedUnyield && response.swResponse != _interruptStatus) {
            // If the interrupt was caused by an unyield error, every subsequent response must
            // also have that unyield error.
            auto failedResponse = response;

            // TODO (SERVER-97256): Remove this workaround once a proper solution is implemented.
            //
            // This temporary workaround ensures that the routing information entry is invalidated
            // when a stale exception is raised during a remote request within a transaction. We
            // achieve this by decorating the TransactionParticipantFailedUnyieldInfo with the
            // remote error. This is necessary because the unyield error can override the stale
            // exception, preventing the router from invalidating the stale routing entry and
            // causing it to not converge.
            if (auto si = _interruptStatus.extraInfo<TransactionParticipantFailedUnyieldInfo>();
                si && response.swResponse.isOK()) {
                auto status = getStatusFromCommandResult(response.swResponse.getValue().data);
                failedResponse.swResponse =
                    Status{TransactionParticipantFailedUnyieldInfo(si->getOriginalError(), status),
                           _interruptStatus.reason()};
            } else {
                failedResponse.swResponse = _interruptStatus;
            }

            return failedResponse;
        }
        return response;
    };

    if (!_interruptStatus.isOK()) {
        return popResponseAfterInterrupt();
    }

    // Try to pop a value from the queue
    try {
        if (_resourceYielder) {
            _resourceYielder->yield(_opCtx);

            if (MONGO_unlikely(hangAfterYield.shouldFail())) {
                hangAfterYield.pauseWhileSet();
            }
        }

        auto curOp = CurOp::get(_opCtx);
        // Calculating the total wait time for remote operations relies on the CurOp's timing
        // measurement facility and we can't use such facility when the current operation is marked
        // as done. Some commands such as 'analyzeShardKey' command may send remote operations using
        // AsyncRequestsSender even after marking the current operation done and so we need to check
        // whether the current operation is still in progress.
        auto curOpInProgress = !curOp->isDone();
        if (curOpInProgress) {
            curOp->startRemoteOpWaitTimer();
        }
        // Only wait for the next result without popping it, so an error unyielding doesn't
        // discard an already popped response.
        auto waitStatus = _responseQueue.waitForNonEmptyNoThrow(_opCtx);
        if (curOpInProgress) {
            curOp->stopRemoteOpWaitTimer();
        }

        auto unyieldStatus =
            _resourceYielder ? _resourceYielder->unyieldNoThrow(_opCtx) : Status::OK();

        _failedUnyield = !unyieldStatus.isOK();
        uassertStatusOK(unyieldStatus);
        uassertStatusOK(waitStatus);

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

    // abort any ongoing backoff.
    _cancellationSource.cancel();

    // Stop servicing callbacks
    _subBaton.shutdown();

    // shutdown the scoped task executor
    _subExecutor->shutdown();

    return popResponseAfterInterrupt();
}

void AsyncRequestsSender::stopRetrying() noexcept {
    _stopRetrying = true;
}

bool AsyncRequestsSender::done() const noexcept {
    return !_remotesLeft;
}

AsyncRequestsSender::Request::Request(ShardId shardId, BSONObj cmdObj, std::shared_ptr<Shard> shard)
    : shardId(shardId), cmdObj(cmdObj), shard(std::move(shard)) {}

Status AsyncRequestsSender::Response::getEffectiveStatus(
    const AsyncRequestsSender::Response& response) {
    if (!response.swResponse.isOK()) {
        return response.swResponse.getStatus();
    }

    const auto& cmdResponse = response.swResponse.getValue().data;
    auto commandStatus = getStatusFromCommandResult(cmdResponse);
    if (!commandStatus.isOK()) {
        return commandStatus;
    }
    auto writeConcernStatus = getWriteConcernStatusFromCommandResult(cmdResponse);
    return writeConcernStatus;
}

AsyncRequestsSender::RemoteData::RemoteData(AsyncRequestsSender* ars,
                                            ShardId shardId,
                                            BSONObj cmdObj,
                                            HostAndPort designatedHostAndPort,
                                            std::shared_ptr<Shard> shard)
    : _ars(ars),
      _shardId(std::move(shardId)),
      _cmdObj(std::move(cmdObj)),
      _designatedHostAndPort(std::move(designatedHostAndPort)),
      _shard(std::move(shard)) {}

SemiFuture<std::shared_ptr<Shard>> AsyncRequestsSender::RemoteData::getShard() {
    if (_shard) {
        // Clear the cached shard so any retries will look up the shard again, in case its state has
        // changed.
        using std::swap;
        std::shared_ptr<Shard> temp;
        swap(_shard, temp);
        return temp;
    }
    return Grid::get(getGlobalServiceContext())
        ->shardRegistry()
        ->getShard(*_ars->_subBaton, _shardId);
}

void AsyncRequestsSender::RemoteData::executeRequest() {
    scheduleRequest()
        .thenRunOn(*_ars->_subBaton)
        .getAsync([this](StatusWith<RemoteCommandCallbackArgs> rcr) {
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

auto AsyncRequestsSender::RemoteData::scheduleRequest() -> SemiFuture<RemoteCommandCallbackArgs> {
    return getShard()
        .thenRunOn(*_ars->_subBaton)
        .then([this](const auto& shard) -> SemiFuture<std::vector<HostAndPort>> {
            if (!_ownedRetryStrategy) {
                _ownedRetryStrategy.emplace(shard, _ars->_retryPolicy);
            } else if (_ownedRetryStrategy->owner->getId() != shard->getId()) {
                _ownedRetryStrategy = OwnedRetryStrategy{shard, _ars->_retryPolicy};
            }

            if (!_designatedHostAndPort.empty()) {
                const auto& connStr = shard->getTargeter()->connectionString();
                const auto& servers = connStr.getServers();
                uassert(ErrorCodes::HostNotFound,
                        str::stream() << "Host " << _designatedHostAndPort
                                      << " is not a host in shard " << shard->getId(),
                        std::find(servers.begin(), servers.end(), _designatedHostAndPort) !=
                            servers.end());
                return std::vector<HostAndPort>{_designatedHostAndPort};
            }

            // TODO(SERVER-108323): Evaluate if we need to send targeting information here.
            return shard->getTargeter()->findHosts(_ars->_readPreference,
                                                   CancellationToken::uncancelable());
        })
        .thenRunOn(*_ars->_subBaton)
        .then([this](const auto& hostAndPorts) {
            _shardHostAndPort.emplace(hostAndPorts.front());
            return scheduleRemoteCommand(hostAndPorts);
        })
        .then([this](auto&& rcr) { return handleResponse(std::move(rcr)); })
        .semi();
}

auto AsyncRequestsSender::RemoteData::scheduleRemoteCommand(
    std::span<const HostAndPort> hostAndPorts) -> SemiFuture<RemoteCommandCallbackArgs> {
    // TODO: SERVER-108323 Consider if this function needs to take targeting into account.
    executor::RemoteCommandRequest request(
        hostAndPorts[0], _ars->_db, _cmdObj, _ars->_metadataObj, _ars->_opCtx);

    // We have to make a promise future pair because the TaskExecutor doesn't currently support a
    // future returning variant of scheduleRemoteCommand
    auto [p, f] = makePromiseFuture<RemoteCommandCallbackArgs>();

    // Failures to schedule skip the retry loop
    uassertStatusOK(_ars->_subExecutor->scheduleRemoteCommand(
        request,
        // We have to make a shared_ptr<Promise> here because scheduleRemoteCommand requires
        // copyable callbacks
        [p = std::make_shared<Promise<RemoteCommandCallbackArgs>>(std::move(p))](
            const RemoteCommandCallbackArgs& cbData) { p->emplaceValue(cbData); },
        *_ars->_subBaton));

    return std::move(f).semi();
}

auto AsyncRequestsSender::RemoteData::handleResponse(RemoteCommandCallbackArgs rcr)
    -> SemiFuture<RemoteCommandCallbackArgs> {
    _shardHostAndPort = rcr.response.target;

    auto status = rcr.response.status;
    bool isRemote = false;

    if (MONGO_likely(status.isOK())) {
        status = getStatusFromCommandResult(rcr.response.data);
        isRemote = true;
    }

    invariant(_ownedRetryStrategy);

    if (MONGO_likely(status.isOK())) {
        status = getWriteConcernStatusFromCommandResult(rcr.response.data);
        if (MONGO_likely(status.isOK())) {
            // If we're okay (RemoteCommandResponse, command result and write concern)-wise we're
            // done. Otherwise check for retryability
            _ownedRetryStrategy->strategy.recordSuccess(rcr.request.target);
            _ownedRetryStrategy.reset();
            return rcr;
        }
        _writeConcernErrorRCR.emplace(rcr);
        LOGV2_DEBUG(7810400,
                    1,
                    "Record write concern error",
                    "error"_attr = _writeConcernErrorRCR.value().response.toString());
    }

    // There was an error with either the response or the command.
    return getShard()
        .thenRunOn(*_ars->_subBaton)
        .then([this, status = std::move(status), rcr = std::move(rcr), isRemote](
                  std::shared_ptr<mongo::Shard> shard) {
            if (!ErrorCodes::isShutdownError(status.code()) || isRemote) {
                shard->updateReplSetMonitor(rcr.response.target, status);
            }

            bool isStartingTransaction = _cmdObj.getField("startTransaction").booleanSafe();
            if (!_ars->_stopRetrying && !isStartingTransaction &&
                _ownedRetryStrategy->strategy.recordFailureAndEvaluateShouldRetry(
                    status, rcr.response.target, rcr.response.getErrorLabels())) {
                LOGV2_DEBUG(
                    4615637,
                    1,
                    "Command to remote shard failed with retryable error and will be retried",
                    "shardId"_attr = _shardId,
                    "attemptedHosts"_attr = rcr.request.target,
                    "failedHost"_attr = rcr.response.target,
                    "error"_attr = redact(status));
                // TODO(SERVER-110000): Consider removing this retry count used for logging.
                ++_retryCount;
                // TODO(SERVER-108323): Evaluate if we need to change the targeting logic here.
                _shardHostAndPort.reset();
                const auto delay = _ownedRetryStrategy->strategy.getNextRetryDelay();

                if (delay > Milliseconds{0}) {
                    return _ars->_subBaton
                        ->waitUntil(_ars->_subExecutor->now() + delay,
                                    _ars->_cancellationSource.token())
                        .then([this] {
                            // retry through recursion
                            return scheduleRequest();
                        })
                        .semi();
                }

                // retry through recursion
                return scheduleRequest();
            }


            // We're not okay (on the remote), but still not going to retry
            if (MONGO_unlikely(_writeConcernErrorRCR)) {
                return Future<RemoteCommandCallbackArgs>::makeReady(
                           std::move(*_writeConcernErrorRCR))
                    .semi();
            }

            // Status' in the response.status field that aren't retried get converted to top level
            // errors
            uassertStatusOK(rcr.response.status);
            return Future<RemoteCommandCallbackArgs>::makeReady(std::move(rcr)).semi();
        })
        .semi();
};

}  // namespace mongo
