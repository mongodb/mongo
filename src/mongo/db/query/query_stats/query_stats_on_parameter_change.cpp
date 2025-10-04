/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/query_stats_on_parameter_change.h"

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
 * Given the current 'Client', returns a pointer to the 'ServiceContext' and an interface for
 * updating the queryStats store.
 */
std::pair<ServiceContext*, OnParamChangeUpdater*> getUpdater(const Client& client) {
    auto serviceCtx = client.getServiceContext();
    tassert(7106500, "ServiceContext must be non null", serviceCtx);

    auto updater = queryStatsStoreOnParamChangeUpdater(serviceCtx).get();
    tassert(7106501, "queryStats store size updater must be non null", updater);
    return {serviceCtx, updater};
}
}  // namespace


Status onQueryStatsStoreSizeUpdate(const std::string& str) {
    auto newSize = memory_util::MemorySize::parse(str);
    if (!newSize.isOK()) {
        return newSize.getStatus();
    }

    // The client is nullptr if the parameter is supplied from the command line. In this case, we
    // ignore the update event, the parameter will be processed when initializing the service
    // context.
    if (auto client = Client::getCurrent()) {
        auto&& [serviceCtx, updater] = getUpdater(*client);
        updater->updateCacheSize(serviceCtx, newSize.getValue());
    }

    return Status::OK();
}

Status validateQueryStatsStoreSize(const std::string& str, const boost::optional<TenantId>&) {
    return memory_util::MemorySize::parse(str).getStatus();
}

Status onQueryStatsRateLimiterUpdateImpl() {
    // The client is nullptr if the parameter is supplied from the command line. In this case, we
    // ignore the update event, the parameter will be processed when initializing the service
    // context.
    if (auto client = Client::getCurrent()) {
        auto&& [serviceCtx, updater] = getUpdater(*client);
        updater->updateRateLimiter(serviceCtx);
    }

    return Status::OK();
}

Status onQueryStatsRateLimitUpdate(int) {
    return onQueryStatsRateLimiterUpdateImpl();
}

Status onQueryStatsSamplingRateUpdate(double) {
    return onQueryStatsRateLimiterUpdateImpl();
}

const Decorable<ServiceContext>::Decoration<std::unique_ptr<OnParamChangeUpdater>>
    queryStatsStoreOnParamChangeUpdater =
        ServiceContext::declareDecoration<std::unique_ptr<OnParamChangeUpdater>>();
}  // namespace mongo::query_stats_util
