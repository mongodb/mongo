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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/service_executor.h"

#include <boost/optional.hpp>

#include "mongo/logv2/log.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor_fixed.h"
#include "mongo/transport/service_executor_reserved.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/util/synchronized_value.h"

namespace mongo {
namespace transport {
namespace {
static constexpr auto kDiagnosticLogLevel = 4;

auto getServiceExecutorStats =
    ServiceContext::declareDecoration<synchronized_value<ServiceExecutorStats>>();
auto getServiceExecutorContext =
    Client::declareDecoration<boost::optional<ServiceExecutorContext>>();
}  // namespace

StringData toString(ServiceExecutorContext::ThreadingModel threadingModel) {
    switch (threadingModel) {
        case ServiceExecutorContext::ThreadingModel::kDedicated:
            return "Dedicated"_sd;
        case ServiceExecutorContext::ThreadingModel::kBorrowed:
            return "Borrowed"_sd;
        default:
            MONGO_UNREACHABLE;
    }
}

ServiceExecutorStats ServiceExecutorStats::get(ServiceContext* ctx) noexcept {
    return getServiceExecutorStats(ctx).get();
}

ServiceExecutorContext* ServiceExecutorContext::get(Client* client) noexcept {
    auto& serviceExecutorContext = getServiceExecutorContext(client);

    if (!serviceExecutorContext) {
        // Service worker Clients will never have a ServiceExecutorContext.
        return nullptr;
    }

    return &serviceExecutorContext.get();
}

void ServiceExecutorContext::set(Client* client, ServiceExecutorContext seCtx) noexcept {
    auto& serviceExecutorContext = getServiceExecutorContext(client);
    invariant(!serviceExecutorContext);

    seCtx._client = client;
    seCtx._sep = client->getServiceContext()->getServiceEntryPoint();

    {
        auto stats = getServiceExecutorStats(client->getServiceContext()).synchronize();
        if (seCtx._canUseReserved) {
            ++stats->limitExempt;
        }

        switch (seCtx._threadingModel) {
            case ThreadingModel::kBorrowed: {
                ++stats->usesBorrowed;
            } break;
            case ThreadingModel::kDedicated: {
                ++stats->usesDedicated;
            } break;
            default:
                MONGO_UNREACHABLE;
        }
    }

    LOGV2_DEBUG(4898000,
                kDiagnosticLogLevel,
                "Setting initial ServiceExecutor context for client",
                "client"_attr = client->desc(),
                "threadingModel"_attr = seCtx._threadingModel,
                "canUseReserved"_attr = seCtx._canUseReserved);
    serviceExecutorContext = std::move(seCtx);
}

void ServiceExecutorContext::reset(Client* client) noexcept {
    if (client) {
        auto& serviceExecutorContext = getServiceExecutorContext(client);

        auto stats = getServiceExecutorStats(client->getServiceContext()).synchronize();

        LOGV2_DEBUG(4898001,
                    kDiagnosticLogLevel,
                    "Resetting ServiceExecutor context for client",
                    "client"_attr = client->desc(),
                    "threadingModel"_attr = serviceExecutorContext->_threadingModel,
                    "canUseReserved"_attr = serviceExecutorContext->_canUseReserved);

        if (serviceExecutorContext->_canUseReserved) {
            --stats->limitExempt;
        }

        switch (serviceExecutorContext->_threadingModel) {
            case ThreadingModel::kBorrowed: {
                --stats->usesBorrowed;
            } break;
            case ThreadingModel::kDedicated: {
                --stats->usesDedicated;
            } break;
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void ServiceExecutorContext::setThreadingModel(ThreadingModel threadingModel) noexcept {

    if (_threadingModel == threadingModel) {
        // Nothing to do.
        return;
    }

    auto lastThreadingModel = std::exchange(_threadingModel, threadingModel);

    if (_client) {
        auto stats = getServiceExecutorStats(_client->getServiceContext()).synchronize();

        // Decrement the stats for the previous ThreadingModel.
        switch (lastThreadingModel) {
            case ThreadingModel::kBorrowed: {
                --stats->usesBorrowed;
            } break;
            case ThreadingModel::kDedicated: {
                --stats->usesDedicated;
            } break;
            default:
                MONGO_UNREACHABLE;
        }
        // Increment the stats for the next ThreadingModel.
        switch (_threadingModel) {
            case ThreadingModel::kBorrowed: {
                ++stats->usesBorrowed;
            } break;
            case ThreadingModel::kDedicated: {
                ++stats->usesDedicated;
            } break;
            default:
                MONGO_UNREACHABLE;
        }
    }
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

    switch (_threadingModel) {
        case ThreadingModel::kBorrowed:
            return ServiceExecutorFixed::get(_client->getServiceContext());
        case ThreadingModel::kDedicated: {
            // Continue on.
        } break;
        default:
            MONGO_UNREACHABLE;
    }

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

}  // namespace transport
}  // namespace mongo
