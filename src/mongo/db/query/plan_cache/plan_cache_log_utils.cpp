// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/plan_cache_log_utils.h"

#include "mongo/db/query/plan_cache/plan_cache_decision_metrics.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::log_detail {
void logInactiveCacheEntry(const std::string& key) {
    LOGV2_DEBUG(
        20936, 2, "Not using cached entry since it is inactive", "cacheKey"_attr = redact(key));
}

void logCreateInactiveCacheEntry(std::string&& query,
                                 std::string&& planCacheShapeHash,
                                 std::string&& planCacheKey,
                                 PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) {
    LOGV2_DEBUG(20937,
                1,
                "Creating inactive cache entry for query",
                "query"_attr = redact(query),
                "planCacheShapeHash"_attr = planCacheShapeHash,
                // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
                "queryHash"_attr = planCacheShapeHash,
                "planCacheKey"_attr = planCacheKey,
                "newWorks"_attr = newPlanCacheDecisionMetrics.works.value,
                "newReads"_attr = newPlanCacheDecisionMetrics.reads.value);
}

void logReplaceActiveCacheEntry(std::string&& query,
                                std::string&& planCacheShapeHash,
                                std::string&& planCacheKey,
                                PlanCacheDecisionMetrics works,
                                PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) {
    LOGV2_DEBUG(20938,
                1,
                "Replacing active cache entry for query",
                "query"_attr = redact(query),
                "planCacheShapeHash"_attr = planCacheShapeHash,
                // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
                "queryHash"_attr = planCacheShapeHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = works.works.value,
                "newWorks"_attr = newPlanCacheDecisionMetrics.works.value,
                "oldReads"_attr = works.reads.value,
                "newReads"_attr = newPlanCacheDecisionMetrics.reads.value);
}

void logNoop(std::string&& query,
             std::string&& planCacheShapeHash,
             std::string&& planCacheKey,
             PlanCacheDecisionMetrics planCacheDecisionMetrics,
             PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) {
    LOGV2_DEBUG(20939,
                1,
                "Attempt to write to the planCache resulted in a noop, since there's already "
                "an active cache entry with a lower works value",
                "query"_attr = redact(query),
                "planCacheShapeHash"_attr = planCacheShapeHash,
                // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
                "queryHash"_attr = planCacheShapeHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = planCacheDecisionMetrics.works.value,
                "newWorks"_attr = newPlanCacheDecisionMetrics.works.value,
                "oldReads"_attr = planCacheDecisionMetrics.reads.value,
                "newReads"_attr = newPlanCacheDecisionMetrics.reads.value);
}

void logIncreasingWorkValue(std::string&& query,
                            std::string&& planCacheShapeHash,
                            std::string&& planCacheKey,
                            PlanCacheDecisionMetrics planCacheDecisionMetrics,
                            PlanCacheDecisionMetrics increasedWorks) {
    LOGV2_DEBUG(20940,
                1,
                "Increasing work value associated with cache entry",
                "query"_attr = redact(query),
                "planCacheShapeHash"_attr = planCacheShapeHash,
                // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
                "queryHash"_attr = planCacheShapeHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = planCacheDecisionMetrics.works.value,
                "increasedWorks"_attr = increasedWorks.works.value,
                "oldReads"_attr = planCacheDecisionMetrics.reads.value,
                "increasedReads"_attr = increasedWorks.reads.value);
}

void logPromoteCacheEntry(std::string&& query,
                          std::string&& planCacheShapeHash,
                          std::string&& planCacheKey,
                          PlanCacheDecisionMetrics planCacheDecisionMetrics,
                          PlanCacheDecisionMetrics increasedWorks) {
    LOGV2_DEBUG(20941,
                1,
                "Inactive cache entry for query is being promoted to active entry",
                "query"_attr = redact(query),
                "planCacheShapeHash"_attr = planCacheShapeHash,
                // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
                "queryHash"_attr = planCacheShapeHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = planCacheDecisionMetrics.works.value,
                "newWorks"_attr = increasedWorks.works.value,
                "oldReads"_attr = planCacheDecisionMetrics.reads.value,
                "newReads"_attr = increasedWorks.reads.value);
}

void logUnexpectedPinnedCacheEntry(std::string&& query,
                                   std::string&& planCacheShapeHash,
                                   std::string&& planCacheKey,
                                   std::string&& oldEntry,
                                   std::string&& newEntry,
                                   std::string&& oldSbePlan,
                                   std::string&& newSbePlan,
                                   PlanCacheDecisionMetrics newPlanCacheDecisionMetrics) {
    LOGV2(8983103,
          "Found unexpected pinned plan cache entry",
          "query"_attr = redact(query),
          "planCacheShapeHash"_attr = planCacheShapeHash,
          // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
          "queryHash"_attr = planCacheShapeHash,
          "planCacheKey"_attr = planCacheKey,
          "oldEntry"_attr = oldEntry,
          "newEntry"_attr = newEntry,
          "oldSbePlan"_attr = oldSbePlan,
          "newSbePlan"_attr = newSbePlan,
          "newWorks"_attr = newPlanCacheDecisionMetrics.works.value,
          "newReads"_attr = newPlanCacheDecisionMetrics.reads.value);
}
}  // namespace mongo::log_detail
