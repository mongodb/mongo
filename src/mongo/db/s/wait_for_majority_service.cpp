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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/wait_for_majority_service.h"

#include <utility>

#include "mongo/db/service_context.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
const WriteConcernOptions kMajorityWriteConcern(WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout);

const auto waitForMajorityServiceDecoration =
    ServiceContext::declareDecoration<WaitForMajorityService>();
}  // namespace

WaitForMajorityService::~WaitForMajorityService() {
    shutDown();
}

WaitForMajorityService& WaitForMajorityService::get(ServiceContext* service) {
    return waitForMajorityServiceDecoration(service);
}

void WaitForMajorityService::setUp(ServiceContext* service) {
    stdx::lock_guard lk(_mutex);

    if (!_thread.joinable() && !_inShutDown) {
        _thread = stdx::thread([this, service] { _periodicallyWaitForMajority(service); });
    }
}

void WaitForMajorityService::shutDown() {
    {
        stdx::lock_guard lk(_mutex);

        if (std::exchange(_inShutDown, true)) {
            return;
        }

        if (_opCtx) {
            stdx::lock_guard scopedClientLock(*_opCtx->getClient());
            _opCtx->getServiceContext()->killOperation(
                scopedClientLock, _opCtx, ErrorCodes::InterruptedAtShutdown);
        }
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    stdx::lock_guard lk(_mutex);
    for (auto&& pendingRequest : _queuedOpTimes) {
        pendingRequest.second.setError(
            {ErrorCodes::InterruptedAtShutdown, "Shutting down wait for majority service"});
    }

    _queuedOpTimes.clear();
}

SharedSemiFuture<void> WaitForMajorityService::waitUntilMajority(const repl::OpTime& opTime) {
    stdx::lock_guard lk(_mutex);

    if (_inShutDown) {
        return {Future<void>::makeReady(
            Status{ErrorCodes::ShutdownInProgress,
                   "rejecting wait for majority request due to server shutdown"})};
    }

    // Background thread must be running before requesting.
    invariant(_thread.joinable());

    if (_lastOpTimeWaited >= opTime) {
        return {Future<void>::makeReady()};
    }

    auto iter = _queuedOpTimes.lower_bound(opTime);
    if (iter != _queuedOpTimes.end()) {
        if (iter->first == opTime) {
            return iter->second.getFuture();
        }
    }

    if (iter == _queuedOpTimes.begin()) {
        // Background thread could already be actively waiting on a later time, so tell it to stop
        // and wait for the newly requested opTime instead.
        if (_opCtx) {
            stdx::lock_guard scopedClientLock(*_opCtx->getClient());
            _opCtx->getServiceContext()->killOperation(
                scopedClientLock, _opCtx, ErrorCodes::WaitForMajorityServiceEarlierOpTimeAvailable);
        }
    }

    const bool wasEmpty = _queuedOpTimes.empty();
    auto resultIter = _queuedOpTimes.emplace_hint(
        iter, std::piecewise_construct, std::forward_as_tuple(opTime), std::forward_as_tuple());

    if (wasEmpty) {
        _hasNewOpTimeCV.notify_one();
    }

    return resultIter->second.getFuture();
}

void WaitForMajorityService::_periodicallyWaitForMajority(ServiceContext* service) {
    ThreadClient tc("waitForMajority", service);

    stdx::unique_lock<Latch> lk(_mutex);

    while (!_inShutDown) {
        auto opCtx = tc->makeOperationContext();
        _opCtx = opCtx.get();

        if (!_queuedOpTimes.empty()) {
            auto lowestOpTimeIter = _queuedOpTimes.begin();
            auto lowestOpTime = lowestOpTimeIter->first;

            lk.unlock();

            WriteConcernResult ignoreResult;
            auto status =
                waitForWriteConcern(_opCtx, lowestOpTime, kMajorityWriteConcern, &ignoreResult);

            lk.lock();

            if (status.isOK()) {
                _lastOpTimeWaited = lowestOpTime;
            }

            if (status == ErrorCodes::WaitForMajorityServiceEarlierOpTimeAvailable) {
                _opCtx = nullptr;
                continue;
            }

            if (status.isOK()) {
                lowestOpTimeIter->second.emplaceValue();
            } else {
                lowestOpTimeIter->second.setError(status);
            }

            _queuedOpTimes.erase(lowestOpTimeIter);
        }

        try {
            _opCtx->waitForConditionOrInterrupt(
                _hasNewOpTimeCV, lk, [&] { return !_queuedOpTimes.empty() || _inShutDown; });
        } catch (const DBException& e) {
            LOG(1) << "Unable to wait for new op time due to: " << e;
        }

        _opCtx = nullptr;
    }
}

}  // namespace mongo
