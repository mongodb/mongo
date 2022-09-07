/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/repl/wait_for_majority_service.h"

#include <utility>

#include "mongo/db/service_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future_util.h"
#include "mongo/util/static_immortal.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);
const auto waitForMajorityServiceDecoration =
    ServiceContext::declareDecoration<WaitForMajorityService>();

constexpr static auto kWaitClientName = "WaitForMajorityServiceWaiter";
constexpr static auto kCancelClientName = "WaitForMajorityServiceCanceler";

std::unique_ptr<ThreadPool> makeThreadPool() {
    ThreadPool::Options options;
    options.poolName = "WaitForMajorityServiceThreadPool";
    options.minThreads = 0;
    // This service must have the ability to use at least two background threads. If it is limited
    // to one, than if that thread is blocking waiting on an opTime, any cancellations cannot be
    // completed until that wait is complete.
    options.maxThreads = 2;
    return std::make_unique<ThreadPool>(options);
}
inline Status waitUntilMajorityCanceledStatus() {
    static StaticImmortal s =
        Status{ErrorCodes::CallbackCanceled, "WaitForMajorityService::waitUntilMajority canceled"};
    return *s;
}
}  // namespace

WaitForMajorityService::~WaitForMajorityService() {
    shutDown();
}

WaitForMajorityService& WaitForMajorityService::get(ServiceContext* service) {
    return waitForMajorityServiceDecoration(service);
}

void WaitForMajorityService::startup(ServiceContext* ctx) {
    stdx::lock_guard lk(_mutex);
    invariant(_state == State::kNotStarted);
    _pool = makeThreadPool();
    _waitForMajorityClient = ClientStrand::make(ctx->makeClient(kWaitClientName));
    _waitForMajorityCancellationClient = ClientStrand::make(ctx->makeClient(kCancelClientName));
    _backgroundWorkComplete = _periodicallyWaitForMajority();
    _pool->startup();
    _state = State::kRunning;
}

void WaitForMajorityService::shutDown() {
    {
        stdx::lock_guard lk(_mutex);

        if (_state != State::kRunning) {
            return;
        }
        _state = State::kShutdown;

        _waitForMajorityClient->getClientPointer()->setKilled();
        _waitForMajorityCancellationClient->getClientPointer()->setKilled();

        for (auto&& request : _queuedOpTimes) {
            if (!request.second->hasBeenProcessed.swap(true)) {
                request.second->result.setError(
                    {ErrorCodes::InterruptedAtShutdown, "Shutting down wait for majority service"});
            }
        }
        _hasNewOpTimeCV.notifyAllAndClose();
    }
    _pool->shutdown();
    _pool->join();
    _backgroundWorkComplete->wait();
    // It's important to reset the clientstrand pointers after waiting for work
    // in the thread pool to complete since that work might be using the client
    // objects.
    _waitForMajorityClient.reset();
    _waitForMajorityCancellationClient.reset();
}

SemiFuture<void> WaitForMajorityService::waitUntilMajority(const repl::OpTime& opTime,
                                                           const CancellationToken& cancelToken) {

    auto [promise, future] = makePromiseFuture<void>();
    auto request = std::make_shared<Request>(std::move(promise));

    stdx::lock_guard lk(_mutex);

    tassert(5065600,
            "WaitForMajorityService must be started before calling waitUntilMajority",
            _state != State::kNotStarted);

    if (_state == State::kShutdown) {
        return {SemiFuture<void>::makeReady(
            Status{ErrorCodes::ShutdownInProgress,
                   "rejecting wait for majority request due to server shutdown"})};
    }

    if (_lastOpTimeWaited >= opTime) {
        return {SemiFuture<void>::makeReady()};
    }

    if (cancelToken.isCanceled()) {
        return {SemiFuture<void>::makeReady(waitUntilMajorityCanceledStatus())};
    }

    const bool wasEmpty = _queuedOpTimes.empty();

    if (!wasEmpty && opTime < _queuedOpTimes.begin()->first) {
        // Background thread could already be actively waiting on a later time, so tell it to stop
        // and wait for the newly requested opTime instead.
        stdx::lock_guard scopedClientLock(*_waitForMajorityClient->getClientPointer());
        if (auto opCtx = _waitForMajorityClient->getClientPointer()->getOperationContext())
            opCtx->getServiceContext()->killOperation(
                scopedClientLock, opCtx, ErrorCodes::WaitForMajorityServiceEarlierOpTimeAvailable);
    }

    _queuedOpTimes.emplace(
        std::piecewise_construct, std::forward_as_tuple(opTime), std::forward_as_tuple(request));


    if (wasEmpty) {
        // Notify the background thread that work is now available.
        _hasNewOpTimeCV.notifyAllAndReset();
    }

    cancelToken.onCancel().thenRunOn(_pool).getAsync([this, request](Status s) {
        if (!s.isOK()) {
            return;
        }
        auto clientGuard = _waitForMajorityCancellationClient->bind();
        if (!request->hasBeenProcessed.swap(true)) {
            request->result.setError(waitUntilMajorityCanceledStatus());
            stdx::lock_guard lk(_mutex);
            auto it = std::find_if(
                std::begin(_queuedOpTimes),
                std::end(_queuedOpTimes),
                [&request](auto&& requestIter) { return request == requestIter.second; });
            invariant(it != _queuedOpTimes.end());
            _queuedOpTimes.erase(it);
        }
    });
    return std::move(future).semi();
}

SemiFuture<void> WaitForMajorityService::_periodicallyWaitForMajority() {
    return AsyncTry([this] {
               auto clientGuard = _waitForMajorityClient->bind();
               stdx::unique_lock<Latch> lk(_mutex);
               if (_queuedOpTimes.empty()) {
                   return _hasNewOpTimeCV.onNotify();
               }
               auto opCtx = clientGuard->makeOperationContext();

               // This needs to be a copy since we unlock the lock before waiting for write concern
               // and the iterator could be invalidated.
               auto lowestOpTime = _queuedOpTimes.begin()->first;

               lk.unlock();

               WriteConcernResult ignoreResult;
               auto status = waitForWriteConcern(
                   opCtx.get(), lowestOpTime, kMajorityWriteConcern, &ignoreResult);

               lk.lock();

               if (status.isOK()) {
                   _lastOpTimeWaited = lowestOpTime;
               }

               if (status != ErrorCodes::WaitForMajorityServiceEarlierOpTimeAvailable) {
                   auto [lowestOpTimeIter, firstElemWithHigherOpTimeIter] =
                       _queuedOpTimes.equal_range(lowestOpTime);

                   for (auto requestIt = lowestOpTimeIter;
                        requestIt != firstElemWithHigherOpTimeIter;
                        /*Increment in loop*/) {
                       if (!requestIt->second->hasBeenProcessed.swap(true)) {
                           requestIt->second->result.setFrom(status);
                           requestIt = _queuedOpTimes.erase(requestIt);
                       } else {
                           ++requestIt;
                       }
                   }
               }
               return SemiFuture<void>::makeReady();
           })
        .until([](Status) {
            // Loop forever until _pool is shut down.
            // TODO (SERVER-53766): Replace with condition-free looping utility.
            return false;
        })
        .on(_pool, CancellationToken::uncancelable())
        .semi();
}

}  // namespace mongo
