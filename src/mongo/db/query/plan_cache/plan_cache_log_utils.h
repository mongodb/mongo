// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_cache/plan_cache_decision_metrics.h"
#include "mongo/util/modules.h"

// The logging facility enforces the rule that logging should not be done in a header file. Since
// template classes and functions below must be defined in the header file and since they use the
// logging facility, we have to define the helper functions below to perform the actual logging
// operation from template code.
namespace mongo::log_detail {
void logInactiveCacheEntry(const std::string& key);
void logCacheEviction(NamespaceString nss, std::string&& evictedEntry);
void logCreateInactiveCacheEntry(std::string&& query,
                                 std::string&& planCacheShapeHash,
                                 std::string&& planCacheKey,
                                 PlanCacheDecisionMetrics newPlanCacheDecisionMetrics);
void logReplaceActiveCacheEntry(std::string&& query,
                                std::string&& planCacheShapeHash,
                                std::string&& planCacheKey,
                                PlanCacheDecisionMetrics plancachedecisionmetrics,
                                PlanCacheDecisionMetrics newPlanCacheDecisionMetrics);
void logNoop(std::string&& query,
             std::string&& planCacheShapeHash,
             std::string&& planCacheKey,
             PlanCacheDecisionMetrics plancachedecisionmetrics,
             PlanCacheDecisionMetrics newPlanCacheDecisionMetrics);
void logIncreasingWorkValue(std::string&& query,
                            std::string&& planCacheShapeHash,
                            std::string&& planCacheKey,
                            PlanCacheDecisionMetrics plancachedecisionmetrics,
                            PlanCacheDecisionMetrics increasedWorks);
void logPromoteCacheEntry(std::string&& query,
                          std::string&& planCacheShapeHash,
                          std::string&& planCacheKey,
                          PlanCacheDecisionMetrics plancachedecisionmetrics,
                          PlanCacheDecisionMetrics newPlanCacheDecisionMetrics);
void logUnexpectedPinnedCacheEntry(std::string&& query,
                                   std::string&& planCacheShapeHash,
                                   std::string&& planCacheKey,
                                   std::string&& oldEntry,
                                   std::string&& newEntry,
                                   std::string&& oldSbePlan,
                                   std::string&& newSbePlan,
                                   PlanCacheDecisionMetrics newPlanCacheDecisionMetrics);
}  // namespace mongo::log_detail
