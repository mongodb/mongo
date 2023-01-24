/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/transport/service_executor.h"

#include <algorithm>
#include <array>
#include <boost/optional.hpp>
#include <utility>

#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor_fixed.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::transport {

bool gInitialUseDedicatedThread = true;

namespace {
static constexpr auto kDiagnosticLogLevel = 4;

auto getServiceExecutorStats =
    ServiceContext::declareDecoration<synchronized_value<ServiceExecutorStats>>();
auto getServiceExecutorContext =
    Client::declareDecoration<std::unique_ptr<ServiceExecutorContext>>();

void incrThreadingModelStats(ServiceExecutorStats& stats, bool useDedicatedThread, int step) {
    (useDedicatedThread ? stats.usesDedicated : stats.usesBorrowed) += step;
}
}  // namespace

ServiceExecutorStats ServiceExecutorStats::get(ServiceContext* ctx) noexcept {
    return getServiceExecutorStats(ctx).get();
}

ServiceExecutorContext* ServiceExecutorContext::get(Client* client) noexcept {
    // Service worker Clients will never have a ServiceExecutorContext.
    return getServiceExecutorContext(client).get();
}

void ServiceExecutorContext::set(Client* client,
                                 std::unique_ptr<ServiceExecutorContext> seCtxPtr) noexcept {
    auto& seCtx = *seCtxPtr;
    auto& serviceExecutorContext = getServiceExecutorContext(client);
    invariant(!serviceExecutorContext);

    seCtx._client = client;
    seCtx._sep = client->getServiceContext()->getServiceEntryPoint();

    {
        auto&& syncStats = *getServiceExecutorStats(client->getServiceContext());
        if (seCtx._canUseReserved)
            ++syncStats->limitExempt;
        incrThreadingModelStats(*syncStats, seCtx._useDedicatedThread, 1);
    }

    LOGV2_DEBUG(4898000,
                kDiagnosticLogLevel,
                "Setting initial ServiceExecutor context for client",
                "client"_attr = client->desc(),
                "useDedicatedThread"_attr = seCtx._useDedicatedThread,
                "canUseReserved"_attr = seCtx._canUseReserved);
    serviceExecutorContext = std::move(seCtxPtr);
}

void ServiceExecutorContext::reset(Client* client) noexcept {
    if (!client)
        return;
    auto& seCtx = getServiceExecutorContext(client);
    LOGV2_DEBUG(4898001,
                kDiagnosticLogLevel,
                "Resetting ServiceExecutor context for client",
                "client"_attr = client->desc(),
                "threadingModel"_attr = seCtx->_useDedicatedThread,
                "canUseReserved"_attr = seCtx->_canUseReserved);
    auto stats = *getServiceExecutorStats(client->getServiceContext());
    if (seCtx->_canUseReserved)
        --stats->limitExempt;
    incrThreadingModelStats(*stats, seCtx->_useDedicatedThread, -1);
}

void ServiceExecutorContext::setUseDedicatedThread(bool b) noexcept {
    if (b == _useDedicatedThread)
        return;
    auto prev = std::exchange(_useDedicatedThread, b);
    if (!_client)
        return;
    auto stats = *getServiceExecutorStats(_client->getServiceContext());
    incrThreadingModelStats(*stats, prev, -1);
    incrThreadingModelStats(*stats, _useDedicatedThread, +1);
}

void ServiceExecutorContext::setCanUseReserved(bool canUseReserved) noexcept {
    if (_canUseReserved == canUseReserved) {
        // Nothing to do.
        return;
    }

    _canUseReserved = canUseReserved;
    if (_client) {
        auto stats = getServiceExecutorStats(_client->getServiceContext()).synchronize();
        if (canUseReserved) {
            ++stats->limitExempt;
        } else {
            --stats->limitExempt;
        }
    }
}

ServiceExecutor* ServiceExecutorContext::getServiceExecutor() noexcept {
    invariant(_client);

    if (_getServiceExecutorForTest)
        return _getServiceExecutorForTest();

    if (!_useDedicatedThread)
        return ServiceExecutorFixed::get(_client->getServiceContext());

    auto shouldUseReserved = [&] {
        // This is at best a naive solution. There could be a world where numOpenSessions() changes
        // very quickly. We are not taking locks on the ServiceEntryPoint, so we may chose to
        // schedule onto the ServiceExecutorReserved when it is no longer necessary. The upside is
        // that we will automatically shift to the ServiceExecutorSynchronous after the first
        // command loop.
        return _sep->numOpenSessions() > _sep->maxOpenSessions();
    };

    if (_canUseReserved && !_hasUsedSynchronous && shouldUseReserved()) {
        if (auto exec = transport::ServiceExecutorReserved::get(_client->getServiceContext())) {
            // We are allowed to use the reserved, we have not used the synchronous, we should use
            // the reserved, and the reserved exists.
            return exec;
        }
    }

    // Once we use the ServiceExecutorSynchronous, we shouldn't use the ServiceExecutorReserved.
    _hasUsedSynchronous = true;
    return transport::ServiceExecutorSynchronous::get(_client->getServiceContext());
}

void ServiceExecutor::yieldIfAppropriate() const {
    /*
     * In perf testing we found that yielding after running a each request produced
     * at 5% performance boost in microbenchmarks if the number of worker threads
     * was greater than the number of available cores.
     */
    static const auto cores = ProcessInfo::getNumAvailableCores();
    if (getRunningThreads() > cores) {
        stdx::this_thread::yield();
    }
}

void ServiceExecutor::shutdownAll(ServiceContext* serviceContext, Date_t deadline) {
    auto getTimeout = [&] {
        auto now = serviceContext->getPreciseClockSource()->now();
        return std::max(Milliseconds{0}, deadline - now);
    };

    if (auto status = transport::ServiceExecutorFixed::get(serviceContext)->shutdown(getTimeout());
        !status.isOK()) {
        LOGV2(4907202, "Failed to shutdown ServiceExecutorFixed", "error"_attr = status);
    }

    if (auto exec = transport::ServiceExecutorReserved::get(serviceContext)) {
        if (auto status = exec->shutdown(getTimeout()); !status.isOK()) {
            LOGV2(4907201, "Failed to shutdown ServiceExecutorReserved", "error"_attr = status);
        }
    }

    if (auto status =
            transport::ServiceExecutorSynchronous::get(serviceContext)->shutdown(getTimeout());
        !status.isOK()) {
        LOGV2(4907200, "Failed to shutdown ServiceExecutorSynchronous", "error"_attr = status);
    }
}

}  // namespace mongo::transport
