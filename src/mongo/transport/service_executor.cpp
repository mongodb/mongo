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


#include <algorithm>
#include <thread>
#include <utility>


#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::transport {

namespace {
static constexpr auto kDiagnosticLogLevel = 4;

auto getServiceExecutorContext =
    Client::declareDecoration<std::unique_ptr<ServiceExecutorContext>>();

SessionManager* getSessionManager(Client* client) {
    if (!client)
        return nullptr;
    auto session = client->session();
    if (!session)
        return nullptr;
    auto tl = session->getTransportLayer();
    if (!tl)
        return nullptr;
    return tl->getSessionManager();
}

ServiceExecutorStats* getServiceExecutorContextStats(Client* client) {
    auto* sm = getSessionManager(client);
    if (!sm)
        return nullptr;
    return &sm->serviceExecutorStats;
}

// This is at best a naive solution. There could be a world where numOpenSessions() changes
// very quickly. We are not taking locks on the SessionManager, so we may chose to schedule
// onto the ServiceExecutorReserved when it is no longer necessary. The upside is that we
// will automatically shift to the ServiceExecutorSynchronous after the first command loop.
bool shouldUseReserved(Client* client) {
    auto* sm = getSessionManager(client);
    invariant(sm);
    return sm->numOpenSessions() > sm->maxOpenSessions();
}

template <typename Func>
void forEachServiceExecutor(ServiceContext* svcCtx, const Func& func) {
    const auto call = [&]<typename T>(std::type_identity<T>) {
        if (auto exec = T::get(svcCtx))
            func(exec);
    };

    call(std::type_identity<ServiceExecutorSynchronous>{});
    call(std::type_identity<ServiceExecutorReserved>{});
    call(std::type_identity<ServiceExecutorInline>{});
}

}  // namespace

ServiceExecutorContext* ServiceExecutorContext::get(Client* client) {
    // Service worker Clients will never have a ServiceExecutorContext.
    return getServiceExecutorContext(client).get();
}

void ServiceExecutorContext::set(Client* client, std::unique_ptr<ServiceExecutorContext> seCtxPtr) {
    auto& seCtx = *seCtxPtr;
    auto& serviceExecutorContext = getServiceExecutorContext(client);
    invariant(!serviceExecutorContext);

    seCtx._client = client;

    if (seCtx._canUseReserved)
        if (auto* stats = getServiceExecutorContextStats(client))
            stats->limitExempt.fetchAndAddRelaxed(1);

    LOGV2_DEBUG(4898000,
                kDiagnosticLogLevel,
                "Setting initial ServiceExecutor context for client",
                "client"_attr = client->desc(),
                "usesDedicatedThread"_attr = true,
                "canUseReserved"_attr = seCtx._canUseReserved);
    serviceExecutorContext = std::move(seCtxPtr);
}

void ServiceExecutorContext::reset(Client* client) {
    if (!client)
        return;
    auto& seCtx = getServiceExecutorContext(client);
    LOGV2_DEBUG(4898001,
                kDiagnosticLogLevel,
                "Resetting ServiceExecutor context for client",
                "client"_attr = client->desc(),
                "threadingModel"_attr = true,
                "canUseReserved"_attr = seCtx->_canUseReserved);

    if (seCtx->_canUseReserved)
        if (auto* stats = getServiceExecutorContextStats(client))
            stats->limitExempt.fetchAndAddRelaxed(-1);

    seCtx.reset();
}

void ServiceExecutorContext::setThreadModel(ThreadModel model) {
    if (_threadModel == model)
        return;

    std::exchange(_threadModel, model);
}

void ServiceExecutorContext::setCanUseReserved(bool canUseReserved) {
    if (_canUseReserved == canUseReserved) {
        // Nothing to do.
        return;
    }

    _canUseReserved = canUseReserved;
    if (_client)
        if (auto* stats = getServiceExecutorContextStats(_client))
            stats->limitExempt.fetchAndAddRelaxed(canUseReserved ? 1 : -1);
}

ServiceExecutor* ServiceExecutorContext::getServiceExecutor() {
    invariant(_client);

    if (_getServiceExecutorForTest)
        return _getServiceExecutorForTest();

    switch (_threadModel) {
        case ThreadModel::kInline:
            return ServiceExecutorInline::get(_client->getServiceContext());
        case ThreadModel::kSynchronous: {
            if (_canUseReserved && !_hasUsedSynchronous && shouldUseReserved(_client)) {
                if (auto exec = ServiceExecutorReserved::get(_client->getServiceContext())) {
                    // All conditions are met:
                    // * We are allowed to use the reserved
                    // * We have not used the synchronous
                    // * We should use the reserved
                    // * The reserved executor exists
                    return exec;
                }
            }

            // Once we use the ServiceExecutorSynchronous, we shouldn't use the
            // ServiceExecutorReserved.
            _hasUsedSynchronous = true;
            return ServiceExecutorSynchronous::get(_client->getServiceContext());
        }
    }

    MONGO_UNREACHABLE;
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

void ServiceExecutor::startupAll(ServiceContext* svcCtx) {
    // Starts each ServiceExecutor in turn until complete or one of them fails.
    forEachServiceExecutor(svcCtx, [&](ServiceExecutor* exec) { exec->start(); });
}

void ServiceExecutor::shutdownAll(ServiceContext* svcCtx, Milliseconds timeout) {
    auto clock = svcCtx->getPreciseClockSource();
    auto deadline = clock->now() + timeout;

    forEachServiceExecutor(svcCtx, [&](ServiceExecutor* exec) {
        const auto myTimeout = std::max(Milliseconds{0}, deadline - clock->now());
        if (auto status = exec->shutdown(myTimeout); !status.isOK()) {
            LOGV2(4907200,
                  "Failed to shutdown ServiceExecutor",
                  "executor"_attr = exec->getName(),
                  "error"_attr = status);
        }
    });
}

void ServiceExecutor::appendAllServerStats(BSONObjBuilder* builder, ServiceContext* svcCtx) {
    forEachServiceExecutor(svcCtx, [&](ServiceExecutor* exec) { exec->appendStats(builder); });
}

}  // namespace mongo::transport
