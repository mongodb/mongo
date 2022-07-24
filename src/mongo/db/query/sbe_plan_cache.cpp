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

#include "mongo/db/query/plan_cache_size_parameter.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::sbe {
namespace {

const auto sbePlanCacheDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<sbe::PlanCache>>();

size_t convertToSizeInBytes(const plan_cache_util::PlanCacheSizeParameter& param) {
    constexpr size_t kBytesInMB = 1014 * 1024;
    constexpr size_t kMBytesInGB = 1014;

    double sizeInMB = param.size;

    switch (param.units) {
        case plan_cache_util::PlanCacheSizeUnits::kPercent:
            sizeInMB *= ProcessInfo::getMemSizeMB() / 100.0;
            break;
        case plan_cache_util::PlanCacheSizeUnits::kMB:
            break;
        case plan_cache_util::PlanCacheSizeUnits::kGB:
            sizeInMB *= kMBytesInGB;
            break;
    }

    return static_cast<size_t>(sizeInMB * kBytesInMB);
}

/**
 * Sets upper size limit on the PlanCache size to 500GB or 25% of the system's memory, whichever is
 * smaller.
 */
size_t capPlanCacheSize(size_t planCacheSize) {
    constexpr size_t kBytesInGB = 1024 * 1024 * 1024;

    // Maximum size of the plan cache expressed in bytes.
    constexpr size_t kMaximumPlanCacheSize = 500 * kBytesInGB;

    // Maximum size of the plan cache expressed as a share of the memory available to the process.
    const plan_cache_util::PlanCacheSizeParameter limitToProcessSize{
        25, plan_cache_util::PlanCacheSizeUnits::kPercent};
    const size_t limitToProcessSizeInBytes = convertToSizeInBytes(limitToProcessSize);

    // The size will be capped by the minimum of the two values defined above.
    const size_t maxPlanCacheSize = std::min(kMaximumPlanCacheSize, limitToProcessSizeInBytes);

    if (planCacheSize > maxPlanCacheSize) {
        planCacheSize = maxPlanCacheSize;
        LOGV2_DEBUG(6007000,
                    1,
                    "The plan cache size has been capped",
                    "maxPlanCacheSize"_attr = maxPlanCacheSize);
    }

    return planCacheSize;
}

size_t getPlanCacheSizeInBytes(const plan_cache_util::PlanCacheSizeParameter& param) {
    size_t planCacheSize = convertToSizeInBytes(param);
    uassert(5968001,
            "Cache size must be at least 1KB * number of cores",
            planCacheSize >= 1024 * ProcessInfo::getNumCores());
    return capPlanCacheSize(planCacheSize);
}

class PlanCacheOnParamChangeUpdaterImpl final : public plan_cache_util::OnParamChangeUpdater {
public:
    void updateCacheSize(ServiceContext* serviceCtx,
                         plan_cache_util::PlanCacheSizeParameter parameter) final {
        if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
            auto size = getPlanCacheSizeInBytes(parameter);
            auto& globalPlanCache = sbePlanCacheDecoration(serviceCtx);
            globalPlanCache->reset(size);
        }
    }

    void clearCache(ServiceContext* serviceCtx) final {
        if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
            auto& globalPlanCache = sbePlanCacheDecoration(serviceCtx);
            globalPlanCache->clear();
        }
    }
};

ServiceContext::ConstructorActionRegisterer planCacheRegisterer{
    "PlanCacheRegisterer", [](ServiceContext* serviceCtx) {
        plan_cache_util::sbePlanCacheOnParamChangeUpdater(serviceCtx) =
            std::make_unique<PlanCacheOnParamChangeUpdaterImpl>();

        if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
            auto status = plan_cache_util::PlanCacheSizeParameter::parse(planCacheSize.get());
            uassertStatusOK(status);

            auto size = getPlanCacheSizeInBytes(status.getValue());
            auto& globalPlanCache = sbePlanCacheDecoration(serviceCtx);
            globalPlanCache = std::make_unique<sbe::PlanCache>(size, ProcessInfo::getNumCores());
        }
    }};

}  // namespace

sbe::PlanCache& getPlanCache(ServiceContext* serviceCtx) {
    uassert(5933402,
            "Cannot getPlanCache() if 'featureFlagSbeFull' is disabled",
            feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV());
    return *sbePlanCacheDecoration(serviceCtx);
}

sbe::PlanCache& getPlanCache(OperationContext* opCtx) {
    uassert(5933401,
            "Cannot getPlanCache() if 'featureFlagSbeFull' is disabled",
            feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV());
    tassert(5933400, "Cannot get the global SBE plan cache by a nullptr", opCtx);
    return getPlanCache(opCtx->getServiceContext());
}

void clearPlanCacheEntriesWith(ServiceContext* serviceCtx,
                               UUID collectionUuid,
                               size_t collectionVersion,
                               bool matchSecondaryCollections) {
    if (feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
        auto removed =
            sbe::getPlanCache(serviceCtx)
                .removeIf([&collectionUuid, collectionVersion, matchSecondaryCollections](
                              const PlanCacheKey& key, const sbe::PlanCacheEntry& entry) {
                    if (key.getMainCollectionState().version == collectionVersion &&
                        key.getMainCollectionState().uuid == collectionUuid) {
                        return true;
                    }
                    if (matchSecondaryCollections) {
                        for (auto& collectionState : key.getSecondaryCollectionStates()) {
                            if (collectionState.version == collectionVersion &&
                                collectionState.uuid == collectionUuid) {
                                return true;
                            }
                        }
                    }
                    return false;
                });

        LOGV2_DEBUG(6006600,
                    1,
                    "Clearing SBE Plan Cache",
                    "collectionUuid"_attr = collectionUuid,
                    "collectionVersion"_attr = collectionVersion,
                    "removedEntries"_attr = removed);
    }
}
}  // namespace mongo::sbe
