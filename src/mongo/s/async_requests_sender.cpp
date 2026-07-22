// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/s/async_requests_sender.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/telemetry_context_holder.h"
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
#include <type_traits>

#include <boost/optional/optional.hpp>

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
    : _impl(std::make_shared<Impl>(opCtx,
                                   std::move(executor),
                                   dbName,
                                   requests,
                                   readPreference,
                                   retryPolicy,
                                   std::move(resourceYielder),
                                   designatedHostsMap)) {
    Impl::executeRequests(_impl);
}

AsyncRequestsSender::~AsyncRequestsSender() {
    // Cancel outstanding work and detach ARS-facing callbacks, then drop our reference.
    // The underlying Impl object will be destroyed when all callbacks are destroyed.
    _impl->detachFromCaller();
}

bool AsyncRequestsSender::done() const noexcept {
    return _impl->done();
}

AsyncRequestsSender::Response AsyncRequestsSender::next() {
    return _impl->next();
}

void AsyncRequestsSender::stopRetrying() noexcept {
    _impl->stopRetrying();
}

AsyncRequestsSender::Request::Request(ShardRef shardRef,
                                      BSONObj cmdObj,
                                      std::shared_ptr<Shard> shard)
    : shardRef(std::move(shardRef)), cmdObj(std::move(cmdObj)), shard(std::move(shard)) {}

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

AsyncRequestsSender::Impl::Impl(OperationContext* opCtx,
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
      // Initialize command metadata to handle the read preference.
      _metadataObj(_readPreference.toContainingBSON()),
      _retryPolicy(retryPolicy),
      _remotesLeft(requests.size()),
      _subExecutor(std::move(executor)),
      _subBaton(opCtx->getBaton()->makeSubBaton()),
      _resourceYielder(std::move(resourceYielder)) {

    _remotes.reserve(requests.size());
    for (const auto& request : requests) {
        auto designatedHostIter = designatedHostsMap.find(request.shardRef);
        auto designatedHost = designatedHostIter != designatedHostsMap.end()
            ? designatedHostIter->second
            : HostAndPort();
        _remotes.emplace_back(
            _opCtx, request.shardRef, request.cmdObj, std::move(designatedHost), request.shard);
    }

    CurOp::get(_opCtx)->ensureRecordRemoteOpWait();
}

void AsyncRequestsSender::Impl::executeRequests(std::shared_ptr<Impl> impl) {
    for (auto& remote : impl->_remotes) {
        remote.executeRequest(impl);
    }
}

void AsyncRequestsSender::Impl::detachFromCaller() {
    // It is necessary to cancel all outstanding retries here. This is required because the
    // callbacks for retry requests need access to the AsyncRequestsSender's internals, and we want
    // to stop scheduling new work as soon as the caller lets go of the handle.
    _cancellationSource.cancel();

    // Any scheduled continuations from this instance cannot be serviced anymore.
    _subBaton.shutdown();

    // Cancel any outstanding work in the task executor. Callbacks for cancelled commands still run
    // (they push a cancellation response onto the queue), but each of those callbacks shares the
    // Impl, so it remains alive for them.
    _subExecutor->shutdown();
}

bool AsyncRequestsSender::Impl::done() const noexcept {
    return !_remotesLeft;
}

void AsyncRequestsSender::Impl::stopRetrying() noexcept {
    _stopRetrying = true;

    // Cancel all pending retry operations, as they are not needed anymore.
    _cancellationSource.cancel();
}

AsyncRequestsSender::Response AsyncRequestsSender::Impl::next() {
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
            auto failedResponse = std::move(response);

            // This workaround ensures that the routing information entry is invalidated when a
            // stale exception is raised during a remote request within a transaction. We achieve
            // this by decorating the TransactionParticipantFailedUnyieldInfo with the remote error.
            // This is necessary because the unyield error can override the stale exception,
            // preventing the router from invalidating the stale routing entry and causing it to not
            // converge.
            if (auto si = _interruptStatus.extraInfo<TransactionParticipantFailedUnyieldInfo>();
                si && failedResponse.swResponse.isOK()) {
                auto status = getStatusFromCommandResult(failedResponse.swResponse.getValue().data);
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

    // abort any ongoing backoff.
    _cancellationSource.cancel();

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

    return popResponseAfterInterrupt();
}

AsyncRequestsSender::Impl::RemoteData::RemoteData(OperationContext* opCtx,
                                                  ShardRef shardRef,
                                                  BSONObj cmdObj,
                                                  HostAndPort designatedHostAndPort,
                                                  std::shared_ptr<Shard> shard)
    : _shardRef(std::move(shardRef)),
      _cmdObj(std::move(cmdObj)),
      _designatedHostAndPort(std::move(designatedHostAndPort)),
      _shard(std::move(shard)),
      _telemetryCtx(otel::TelemetryContextHolder::getDecoration(opCtx).cloneTelemetryContext()) {}

SemiFuture<std::shared_ptr<Shard>> AsyncRequestsSender::Impl::RemoteData::getShard(
    std::shared_ptr<Impl> impl) {
    if (_shard) {
        // Clear the cached shard so any retries will look up the shard again, in case its state has
        // changed.
        using std::swap;
        std::shared_ptr<Shard> temp;
        swap(_shard, temp);
        _shardHandle = temp->getHandle();
        return temp;
    }

    return Grid::get(getGlobalServiceContext())
        ->shardRegistry()
        ->getShard(*impl->_subBaton, _shardRef)
        .thenRunOn(*impl->_subBaton)
        .then([this, impl = std::move(impl)](auto&& shard) {
            _shardHandle = shard->getHandle();
            return shard;
        })
        .semi();
}

void AsyncRequestsSender::Impl::RemoteData::executeRequest(std::shared_ptr<Impl> impl) {
    scheduleRequest(impl)
        .thenRunOn(*impl->_subBaton)
        .getAsync([this, impl = std::move(impl)](StatusWith<RemoteCommandCallbackArgs> rcr) {
            _done = true;
            if (rcr.isOK()) {
                impl->_responseQueue.push(
                    {std::move(_shardRef), rcr.getValue().response, std::move(_shardHostAndPort)});
            } else {
                impl->_responseQueue.push(
                    {std::move(_shardRef), rcr.getStatus(), std::move(_shardHostAndPort)});
            }
        });
}

auto AsyncRequestsSender::Impl::RemoteData::scheduleRequest(std::shared_ptr<Impl> impl)
    -> SemiFuture<RemoteCommandCallbackArgs> {
    return getShard(impl)
        .thenRunOn(*impl->_subBaton)
        .then([this, impl = impl](const auto& shard) -> SemiFuture<HostAndPort> {
            if (!_retryStrategy) {
                Shard::RetryStrategy::RequestStartTransactionState isStartTransaction =
                    _cmdObj.getField("startTransaction").booleanSafe()
                    ? Shard::RetryStrategy::RequestStartTransactionState::kStartingTransaction
                    : Shard::RetryStrategy::RequestStartTransactionState::kNotStartingTransaction;
                _retryStrategy.emplace(shard, impl->_retryPolicy, isStartTransaction);
            }

            if (!_designatedHostAndPort.empty()) {
                const auto& connStr = shard->getTargeter()->connectionString();
                const auto& servers = connStr.getServers();
                uassert(ErrorCodes::HostNotFound,
                        str::stream() << "Host " << _designatedHostAndPort
                                      << " is not a host in shard " << shard->getId(),
                        std::find(servers.begin(), servers.end(), _designatedHostAndPort) !=
                            servers.end());
                return _designatedHostAndPort;
            }

            return shard->getTargeter()->findHost(impl->_readPreference,
                                                  CancellationToken::uncancelable(),
                                                  _retryStrategy->getTargetingMetadata());
        })
        .thenRunOn(*impl->_subBaton)
        .then([this, impl = impl](const auto& hostAndPort) {
            _shardHostAndPort.emplace(hostAndPort);
            return scheduleRemoteCommand(hostAndPort, std::move(impl));
        })
        .then([this, impl = std::move(impl)](auto&& rcr) {
            return handleResponse(std::move(rcr), std::move(impl));
        })
        .semi();
}

auto AsyncRequestsSender::Impl::RemoteData::scheduleRemoteCommand(const HostAndPort& hostAndPort,
                                                                  std::shared_ptr<Impl> impl)
    -> SemiFuture<RemoteCommandCallbackArgs> {
    executor::RemoteCommandRequest request(
        hostAndPort,
        impl->_db,
        _cmdObj,
        impl->_metadataObj,
        impl->_opCtx,
        executor::RemoteCommandRequest::Options{.telemetryContext = _telemetryCtx});

    // We have to make a promise future pair because the TaskExecutor doesn't currently support a
    // future returning variant of scheduleRemoteCommand
    auto [p, f] = makePromiseFuture<RemoteCommandCallbackArgs>();

    // Failures to schedule skip the retry loop
    uassertStatusOK(impl->_subExecutor->scheduleRemoteCommand(
        request,
        // We have to make a shared_ptr<Promise> here because scheduleRemoteCommand requires
        // copyable callbacks
        [p = std::make_shared<Promise<RemoteCommandCallbackArgs>>(std::move(p)),
         impl = impl](const RemoteCommandCallbackArgs& cbData) { p->emplaceValue(cbData); },
        *impl->_subBaton));

    return std::move(f).semi();
}

auto AsyncRequestsSender::Impl::RemoteData::handleResponse(RemoteCommandCallbackArgs rcr,
                                                           std::shared_ptr<Impl> impl)
    -> SemiFuture<RemoteCommandCallbackArgs> {
    _shardHostAndPort = rcr.response.target;

    auto status = rcr.response.status;
    bool isRemote = false;

    if (MONGO_likely(status.isOK())) {
        status = getStatusFromCommandResult(rcr.response.data);
        isRemote = true;
    }

    invariant(_retryStrategy);

    if (MONGO_likely(status.isOK())) {
        status = getWriteConcernStatusFromCommandResult(rcr.response.data);
        if (MONGO_likely(status.isOK())) {
            // If we're okay (RemoteCommandResponse, command result and write concern)-wise we're
            // done. Otherwise check for retryability
            _retryStrategy->recordSuccess(rcr.request.target);
            _retryStrategy.reset();
            return rcr;
        }
        _writeConcernErrorRCR.emplace(rcr);
        LOGV2_DEBUG(7810400,
                    1,
                    "Record write concern error",
                    "error"_attr = _writeConcernErrorRCR.value().response.toString());
    }

    // There was an error with either the response or the command.
    return getShard(impl)
        .thenRunOn(*impl->_subBaton)
        .then([this,
               impl = std::move(impl),
               status = std::move(status),
               rcr = std::move(rcr),
               isRemote](std::shared_ptr<mongo::Shard> shard) {
            if (!ErrorCodes::isShutdownError(status.code()) || isRemote) {
                shard->updateReplSetMonitor(rcr.response.target, status);
            }

            if (_retryStrategy->recordFailureAndEvaluateShouldRetry(
                    status,
                    rcr.response.target,
                    rcr.response.getErrorLabels(),
                    rcr.response.getBaseBackoffMS()) &&
                !impl->_stopRetrying) {
                const auto delay = _retryStrategy->getNextRetryDelay();

                LOGV2_DEBUG(
                    4615637,
                    1,
                    "Command to remote shard failed with retryable error and will be retried",
                    "shardRef"_attr = _shardRef,
                    "attemptedHosts"_attr = rcr.request.target,
                    "failedHost"_attr = rcr.response.target,
                    "error"_attr = redact(status),
                    "delay"_attr = delay);
                _shardHostAndPort.reset();

                if (delay > Milliseconds{0}) {
                    return impl->_subBaton
                        ->waitUntil(impl->_subExecutor->now() + delay,
                                    impl->_cancellationSource.token())
                        .then([this, impl, delay] {
                            _retryStrategy->recordBackoff(delay);
                            // retry through recursion
                            return scheduleRequest(std::move(impl));
                        })
                        .semi();
                }

                // retry through recursion
                return scheduleRequest(std::move(impl));
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
