/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/query/sbe_plan_cache.h"

#include "mongo/db/server_options.h"

namespace mongo {
namespace sbe {
namespace {
const auto sbePlanCacheDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<sbe::PlanCache>>();

ServiceContext::ConstructorActionRegisterer planCacheRegisterer{
    "PlanCacheRegisterer", [](ServiceContext* serviceCtx) {
        // Max memory size in bytes of the PlanCache.
        constexpr size_t kQueryCacheMaxSizeInBytes = 100 * 1024 * 1024;
        if (feature_flags::gFeatureFlagSbePlanCache.isEnabledAndIgnoreFCV()) {
            auto& globalPlanCache = sbePlanCacheDecoration(serviceCtx);
            globalPlanCache = std::make_unique<sbe::PlanCache>(kQueryCacheMaxSizeInBytes);
        }
    }};

}  // namespace

sbe::PlanCache& getPlanCache(ServiceContext* serviceCtx) {
    uassert(5933402,
            "Cannot getPlanCache() if gFeatureFlagSbePlanCache is disabled",
            feature_flags::gFeatureFlagSbePlanCache.isEnabledAndIgnoreFCV());
    return *sbePlanCacheDecoration(serviceCtx);
}

sbe::PlanCache& getPlanCache(OperationContext* opCtx) {
    uassert(5933401,
            "Cannot getPlanCache() if gFeatureFlagSbePlanCache is disabled",
            feature_flags::gFeatureFlagSbePlanCache.isEnabledAndIgnoreFCV());
    tassert(5933400, "Cannot get the global SBE plan cache by a nullptr", opCtx);
    return getPlanCache(opCtx->getServiceContext());
}

uint32_t PlanCacheKey::queryHash() const {
    return static_cast<uint32_t>(PlanCacheKeyHasher{}(*this));
}

uint32_t PlanCacheKey::planCacheKeyHash() const {
    return static_cast<uint32_t>(PlanCacheKeyHasher{}(*this));
}

}  // namespace sbe
}  // namespace mongo
