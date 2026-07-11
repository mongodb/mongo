// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/query_stats_on_parameter_change.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/client.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/db/service_context.h"

#include <climits>
#include <utility>

#include <boost/optional/optional.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::query_stats_util {

namespace {

/**
 * This helper expects a lambda that takes a ServiceContext* and a OnParamChangeUpdater*. If there
 * is a client, it will execute the given lambda.
 *
 * The client is nullptr if the parameter is supplied from the command line. In this case, we ignore
 * the update event, the parameter will be processed when initializing the service context.
 */
void onParamChange(auto&& fn) {
    if (auto client = Client::getCurrent()) {
        auto serviceCtx = client->getServiceContext();
        tassert(7106500, "ServiceContext must be non null", serviceCtx);
        auto updater = queryStatsStoreOnParamChangeUpdater(serviceCtx).get();
        tassert(7106501, "queryStats store size updater must be non null", updater);
        fn(serviceCtx, updater);
    }
}

}  // namespace


Status onQueryStatsStoreSizeUpdate(const std::string& str) {
    auto newSize = memory_util::MemorySize::parse(str);
    if (!newSize.isOK()) {
        return newSize.getStatus();
    }

    onParamChange([&newSize](ServiceContext* serviceCtx, OnParamChangeUpdater* updater) {
        updater->updateCacheSize(serviceCtx, newSize.getValue());
    });

    return Status::OK();
}

Status validateQueryStatsStoreSize(const std::string& str, const boost::optional<TenantId>&) {
    return memory_util::MemorySize::parse(str).getStatus();
}

Status onQueryStatsRateLimiterUpdateImpl() {
    onParamChange([](ServiceContext* serviceCtx, OnParamChangeUpdater* updater) {
        updater->updateRateLimiter(serviceCtx);
    });

    return Status::OK();
}

Status onQueryStatsWriteCmdRateLimiterUpdateImpl() {
    onParamChange([](ServiceContext* serviceCtx, OnParamChangeUpdater* updater) {
        updater->updateWriteCmdRateLimiter(serviceCtx);
    });

    return Status::OK();
}

Status onQueryStatsRateLimitUpdate(int) {
    return onQueryStatsRateLimiterUpdateImpl();
}

Status onQueryStatsSamplingRateUpdate(double) {
    return onQueryStatsRateLimiterUpdateImpl();
}

Status onQueryStatsWriteCmdSamplingRateUpdate(double) {
    return onQueryStatsWriteCmdRateLimiterUpdateImpl();
}

const Decorable<ServiceContext>::Decoration<std::unique_ptr<OnParamChangeUpdater>>
    queryStatsStoreOnParamChangeUpdater =
        ServiceContext::declareDecoration<std::unique_ptr<OnParamChangeUpdater>>();
}  // namespace mongo::query_stats_util
