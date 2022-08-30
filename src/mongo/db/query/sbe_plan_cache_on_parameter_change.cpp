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

#include "mongo/db/query/sbe_plan_cache_on_parameter_change.h"

#include "mongo/db/client.h"

namespace mongo::plan_cache_util {
namespace {
/**
 * Given the current 'Client', returns a pointer to the 'ServiceContext' and an interface for
 * updating the SBE plan cache,
 */
std::pair<ServiceContext*, OnParamChangeUpdater*> getUpdater(const Client& client) {
    auto serviceCtx = client.getServiceContext();
    tassert(6007013, "ServiceContext must be non null", serviceCtx);

    auto updater = sbePlanCacheOnParamChangeUpdater(serviceCtx).get();
    tassert(6007014, "Plan cache size updater must be non null", updater);
    return {serviceCtx, updater};
}
}  // namespace

Status clearSbeCacheOnParameterChangeHelper() {
    if (auto client = Client::getCurrent()) {
        auto&& [serviceCtx, updater] = getUpdater(*client);
        updater->clearCache(serviceCtx);
    }
    return Status::OK();
}

Status onPlanCacheSizeUpdate(const std::string& str) {
    auto newSize = PlanCacheSizeParameter::parse(str);
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

Status validatePlanCacheSize(const std::string& str, const boost::optional<TenantId>&) {
    return PlanCacheSizeParameter::parse(str).getStatus();
}

const Decorable<ServiceContext>::Decoration<std::unique_ptr<OnParamChangeUpdater>>
    sbePlanCacheOnParamChangeUpdater =
        ServiceContext::declareDecoration<std::unique_ptr<OnParamChangeUpdater>>();

}  // namespace mongo::plan_cache_util
