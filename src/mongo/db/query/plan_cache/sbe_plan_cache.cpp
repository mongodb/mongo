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


#include "mongo/db/query/plan_cache/sbe_plan_cache.h"

#include "mongo/base/status_with.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache_on_parameter_change.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/synchronized_value.h"

#include <absl/container/node_hash_map.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::sbe {
namespace {

const auto sbePlanCacheDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<sbe::PlanCache>>();


class PlanCacheOnParamChangeUpdaterImpl final : public plan_cache_util::OnParamChangeUpdater {
public:
    void updateCacheSize(ServiceContext* serviceCtx, memory_util::MemorySize memSize) final {
        auto newSizeBytes = memory_util::getRequestedMemSizeInBytes(memSize);
        auto cappedCacheSize = memory_util::capMemorySize(newSizeBytes /*requestedSizeBytes*/,
                                                          500 /*maximumSizeGB*/,
                                                          25 /*percentTotalSystemMemory*/);
        if (cappedCacheSize < newSizeBytes) {
            LOGV2_DEBUG(6007001,
                        1,
                        "The plan cache size has been capped",
                        "cappedSize"_attr = cappedCacheSize);
        }
        auto& globalPlanCache = sbePlanCacheDecoration(serviceCtx);
        globalPlanCache->reset(cappedCacheSize);
    }

    void clearCache(ServiceContext* serviceCtx) final {
        auto& globalPlanCache = sbePlanCacheDecoration(serviceCtx);
        globalPlanCache->clear();
    }
};

ServiceContext::ConstructorActionRegisterer planCacheRegisterer{
    "PlanCacheRegisterer", [](ServiceContext* serviceCtx) {
        plan_cache_util::sbePlanCacheOnParamChangeUpdater(serviceCtx) =
            std::make_unique<PlanCacheOnParamChangeUpdaterImpl>();

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
