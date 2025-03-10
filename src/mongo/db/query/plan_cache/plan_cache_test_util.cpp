/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/plan_cache/plan_cache_test_util.h"

#include "mongo/db/exec/plan_cache_util.h"

namespace mongo {
namespace {
const CollectionPtr emptyCollectionPtr{};
}

std::unique_ptr<plan_ranker::PlanRankingDecision> createDecision(size_t numPlans, size_t works) {
    auto why = std::make_unique<plan_ranker::PlanRankingDecision>();
    std::vector<std::unique_ptr<PlanStageStats>> stats;
    for (size_t i = 0; i < numPlans; ++i) {
        CommonStats common("COLLSCAN");
        auto stat = std::make_unique<PlanStageStats>(common, STAGE_COLLSCAN);
        stat->specific.reset(new CollectionScanStats());
        stat->common.works = works;
        stats.push_back(std::move(stat));
        why->scores.push_back(0U);
        why->candidateOrder.push_back(i);
    }
    why->stats.candidatePlanStats = std::move(stats);
    return why;
}

PlanCacheCallbacksImpl<PlanCacheKey, SolutionCacheData, plan_cache_debug_info::DebugInfo>
createCallback(const CanonicalQuery& cq, const plan_ranker::PlanRankingDecision& decision) {
    auto buildDebugInfoFn = [&]() -> plan_cache_debug_info::DebugInfo {
        return plan_cache_util::buildDebugInfo(cq, decision.clone());
    };
    auto printCachedPlanFn = [](const SolutionCacheData& plan) {
        return plan.toString();
    };
    return {cq, std::move(buildDebugInfoFn), printCachedPlanFn, emptyCollectionPtr};
}
}  // namespace mongo
