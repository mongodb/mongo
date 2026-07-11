// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/plan_cache/sbe_plan_cache.h"

#include "mongo/base/status_with.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/synchronized_value.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::sbe {
namespace {

const auto sbePlanCacheDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<sbe::PlanCache>>();


ServiceContext::ConstructorActionRegisterer planCacheRegisterer{
    "PlanCacheRegisterer", [](ServiceContext* serviceCtx) {
        auto status = memory_util::MemorySize::parse(planCacheSize.get());
        uassertStatusOK(status);
        auto size = memory_util::getRequestedMemSizeInBytes(status.getValue());
        auto cappedCacheSize = memory_util::capMemorySize(
            size /*requestedSizeBytes*/, 500 /*maximumSizeGB*/, 25 /*percentTotalSystemMemory*/);
        if (cappedCacheSize < size) {
            LOGV2_DEBUG(6007000,
                        1,
                        "The plan cache size has been capped",
                        "cappedSize"_attr = cappedCacheSize);
        }
        auto& globalPlanCache = sbePlanCacheDecoration(serviceCtx);
        globalPlanCache = std::make_unique<sbe::PlanCache>(cappedCacheSize, 32);
    }};

}  // namespace

sbe::PlanCache& getPlanCache(ServiceContext* serviceCtx) {
    return *sbePlanCacheDecoration(serviceCtx);
}

sbe::PlanCache& getPlanCache(OperationContext* opCtx) {
    tassert(5933400, "Cannot get the global SBE plan cache by a nullptr", opCtx);
    return getPlanCache(opCtx->getServiceContext());
}

void clearPlanCacheEntriesWith(ServiceContext* serviceCtx,
                               UUID collectionUuid,
                               size_t collectionVersion,
                               bool matchSecondaryCollections) {
    auto removed = sbe::getPlanCache(serviceCtx)
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
}  // namespace mongo::sbe
