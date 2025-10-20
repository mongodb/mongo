/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_cache_util.h"

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <queue>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/plan_cache_callbacks.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/plan_explainer_factory.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/sbe_plan_cache.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/overloaded_visitor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace plan_cache_util {
namespace {
void logTieForBest(std::string&& query,
                   double winnerScore,
                   double runnerUpScore,
                   std::string winnerPlanSummary,
                   std::string runnerUpPlanSummary) {
    LOGV2_DEBUG(20594,
                1,
                "Winning plan tied with runner-up, skip caching",
                "query"_attr = redact(query),
                "winnerScore"_attr = winnerScore,
                "winnerPlanSummary"_attr = winnerPlanSummary,
                "runnerUpScore"_attr = runnerUpScore,
                "runnerUpPlanSummary"_attr = runnerUpPlanSummary);
}

void logNotCachingZeroResults(std::string&& query, double score, std::string winnerPlanSummary) {
    LOGV2_DEBUG(20595,
                1,
                "Winning plan had zero results, skip caching",
                "query"_attr = redact(query),
                "winnerScore"_attr = score,
                "winnerPlanSummary"_attr = winnerPlanSummary);
}

void logNotCachingNoData(std::string&& solution) {
    LOGV2_DEBUG(20596,
                5,
                "Not caching query because this solution has no cache data",
                "solutions"_attr = redact(solution));
}

/**
 * Returns a pair of PlanExplainers consisting of the explainer of 'winningPlan' and the explainer
 * of the second best in 'candidates'. If there are less than two candidates, second one remains
 * nullptr.
 */
std::pair<std::unique_ptr<PlanExplainer>, std::unique_ptr<PlanExplainer>> getClassicPlanExplainers(
    const plan_ranker::CandidatePlan& winningPlan,
    const plan_ranker::PlanRankingDecision& ranking,
    const std::vector<plan_ranker::CandidatePlan>& candidates) {
    std::unique_ptr<PlanExplainer> winnerExplainer = plan_explainer_factory::make(winningPlan.root);

    std::unique_ptr<PlanExplainer> runnerUpExplainer;
    if (ranking.candidateOrder.size() > 1) {
        invariant(ranking.candidateOrder.size() > 1U);
        auto runnerUpIdx = ranking.candidateOrder[1];
        runnerUpExplainer = plan_explainer_factory::make(candidates[runnerUpIdx].root);
    }

    return std::make_pair(std::move(winnerExplainer), std::move(runnerUpExplainer));
}

bool shouldCacheBasedOnQueryAndPlan(const CanonicalQuery& query, const QuerySolution* winningPlan) {
    if (!query.isUncacheableSbe() && shouldCacheQuery(query) &&
        winningPlan->isEligibleForPlanCache()) {
        if (!winningPlan->cacheData) {
            logNotCachingNoData(winningPlan->toString());
            return false;
        } else {
            return true;
        }
    }

    return false;
}

void updateClassicPlanCacheFromClassicCandidatesImpl(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CanonicalQuery& query,
    ReadsOrWorks readsOrWorks,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates) {
    auto winnerIdx = ranking->candidateOrder[0];
    invariant(winnerIdx >= 0 && winnerIdx < candidates.size());
    auto& winningPlan = candidates[winnerIdx];

    if (!shouldCacheBasedOnQueryAndPlan(query, winningPlan.solution.get())) {
        return;
    }

    auto buildDebugInfoFn = [&]() -> plan_cache_debug_info::DebugInfo {
        return buildDebugInfo(query, std::move(ranking));
    };
    auto printCachedPlanFn = [](const SolutionCacheData& plan) {
        return plan.toString();
    };
    PlanCacheCallbacksImpl<PlanCacheKey, SolutionCacheData, plan_cache_debug_info::DebugInfo>
        callbacks{query, buildDebugInfoFn, printCachedPlanFn};
    winningPlan.solution->cacheData->indexFilterApplied = winningPlan.solution->indexFilterApplied;
    winningPlan.solution->cacheData->solutionHash = winningPlan.solution->hash();
    auto isSensitive = CurOp::get(opCtx)->getShouldOmitDiagnosticInformation();

    auto key = plan_cache_key_factory::make<PlanCacheKey>(query, collection);

    uassertStatusOK(
        CollectionQueryInfo::get(collection)
            .getPlanCache()
            ->set(std::move(key),
                  winningPlan.solution->cacheData->clone(),
                  readsOrWorks,
                  opCtx->getServiceContext()->getPreciseClockSource()->now(),
                  &callbacks,
                  isSensitive ? PlanSecurityLevel::kSensitive : PlanSecurityLevel::kNotSensitive,
                  boost::none /* worksGrowthCoefficient */));
}

void updateSbePlanCache(OperationContext* opCtx,
                        const MultipleCollectionAccessor& collections,
                        const CanonicalQuery& query,
                        NumReads nReads,
                        const QuerySolution* soln,
                        std::unique_ptr<sbe::CachedSbePlan> cachedPlan) {
    auto buildDebugInfoFn = [soln]() -> plan_cache_debug_info::DebugInfoSBE {
        return buildDebugInfo(soln);
    };
    auto printCachedPlanFn = [](const sbe::CachedSbePlan& plan) {
        sbe::DebugPrinter p;
        return p.print(*plan.root.get());
    };
    PlanCacheCallbacksImpl<sbe::PlanCacheKey,
                           sbe::CachedSbePlan,
                           plan_cache_debug_info::DebugInfoSBE>
        callbacks{query, buildDebugInfoFn, printCachedPlanFn};

    auto isSensitive = CurOp::get(opCtx)->getShouldOmitDiagnosticInformation();

    uassertStatusOK(sbe::getPlanCache(opCtx).set(
        plan_cache_key_factory::make(
            query, collections, canonical_query_encoder::Optimizer::kSbeStageBuilders),
        std::move(cachedPlan),
        nReads,
        opCtx->getServiceContext()->getPreciseClockSource()->now(),
        &callbacks,
        isSensitive ? PlanSecurityLevel::kSensitive : PlanSecurityLevel::kNotSensitive,
        boost::none /* worksGrowthCoefficient */));
}

}  // namespace

void updateClassicPlanCacheFromClassicCandidatesForSbeExecution(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CanonicalQuery& query,
    NumReads reads,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates) {
    invariant(query.isSbeCompatible());

    updateClassicPlanCacheFromClassicCandidatesImpl(
        opCtx, collection, query, reads, std::move(ranking), candidates);
}

void updateClassicPlanCacheFromClassicCandidatesForClassicExecution(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const CanonicalQuery& query,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates) {
    ReadsOrWorks nWorks =
        visit(OverloadedVisitor{[&](const plan_ranker::StatsDetails& details) -> ReadsOrWorks {
                                    return NumWorks{details.candidatePlanStats[0]->common.works};
                                },
                                [](const plan_ranker::SBEStatsDetails& details) -> ReadsOrWorks {
                                    MONGO_UNREACHABLE;
                                }},
              ranking->stats);

    updateClassicPlanCacheFromClassicCandidatesImpl(
        opCtx, collection, query, nWorks, std::move(ranking), candidates);
}

void updateSbePlanCacheFromSbeCandidates(OperationContext* opCtx,
                                         const MultipleCollectionAccessor& collections,
                                         const CanonicalQuery& query,
                                         std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
                                         std::vector<sbe::plan_ranker::CandidatePlan>& candidates) {
    auto winnerIdx = ranking->candidateOrder[0];
    invariant(winnerIdx >= 0 && winnerIdx < candidates.size());
    auto& winningPlan = candidates[winnerIdx];

    if (!shouldCacheBasedOnQueryAndPlan(query, winningPlan.solution.get())) {
        return;
    }

    tassert(6142201,
            "The winning CandidatePlan should contain the original plan",
            winningPlan.clonedPlan);

    // Clone the winning SBE plan and its auxiliary data.
    auto cachedPlan =
        std::make_unique<sbe::CachedSbePlan>(std::move(winningPlan.clonedPlan->first),
                                             std::move(winningPlan.clonedPlan->second.stageData),
                                             winningPlan.solution->hash());
    cachedPlan->indexFilterApplied = winningPlan.solution->indexFilterApplied;

    NumReads nReads =
        visit(OverloadedVisitor{
                  [&](const plan_ranker::StatsDetails& details) -> NumReads { MONGO_UNREACHABLE; },
                  [](const plan_ranker::SBEStatsDetails& details) -> NumReads {
                      return NumReads{calculateNumberOfReads(details.candidatePlanStats[0].get())};
                  }},
              ranking->stats);

    updateSbePlanCache(
        opCtx, collections, query, nReads, winningPlan.solution.get(), std::move(cachedPlan));
}

void updateSbePlanCacheWithNumReads(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const CanonicalQuery& query,
    NumReads nReads,
    const std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>& sbePlanAndData,
    const QuerySolution* winningSolution) {
    if (!shouldCacheBasedOnQueryAndPlan(query, winningSolution)) {
        return;
    }

    // Clone the winning SBE plan and its auxiliary data.
    auto cachedPlan = std::make_unique<sbe::CachedSbePlan>(
        sbePlanAndData.first->clone(), sbePlanAndData.second, winningSolution->hash());
    cachedPlan->indexFilterApplied = winningSolution->indexFilterApplied;

    updateSbePlanCache(opCtx, collections, query, nReads, winningSolution, std::move(cachedPlan));
}

void updateSbePlanCacheWithPinnedEntry(OperationContext* opCtx,
                                       const MultipleCollectionAccessor& collections,
                                       const CanonicalQuery& query,
                                       const QuerySolution& solution,
                                       const sbe::PlanStage& root,
                                       stage_builder::PlanStageData stageData) {
    const CollectionPtr& collection = collections.getMainCollection();
    if (collection && !query.isUncacheableSbe() && shouldCacheQuery(query) &&
        solution.isEligibleForPlanCache()) {
        sbe::PlanCacheKey key = plan_cache_key_factory::make(
            query, collections, canonical_query_encoder::Optimizer::kSbeStageBuilders);
        // Store a copy of the root and corresponding data, as well as the hash of the QuerySolution
        // that led to this cache entry.
        auto plan = std::make_unique<sbe::CachedSbePlan>(
            root.clone(), std::move(stageData), solution.hash());
        plan->indexFilterApplied = solution.indexFilterApplied;

        bool shouldOmitDiagnosticInformation =
            CurOp::get(opCtx)->getShouldOmitDiagnosticInformation();
        sbe::getPlanCache(opCtx).setPinned(
            key,
            canonical_query_encoder::computeHash(
                canonical_query_encoder::encodeForPlanCacheCommand(query)),
            std::move(plan),
            opCtx->getServiceContext()->getPreciseClockSource()->now(),
            buildDebugInfo(&solution),
            shouldOmitDiagnosticInformation);
    }
}

plan_cache_debug_info::DebugInfo buildDebugInfo(
    const CanonicalQuery& query, std::unique_ptr<const plan_ranker::PlanRankingDecision> decision) {
    // Strip projections on $-prefixed fields, as these are added by internal callers of the
    // system and are not considered part of the user projection.
    const FindCommandRequest& findCommand = query.getFindCommandRequest();
    BSONObjBuilder projBuilder;
    for (auto elem : findCommand.getProjection()) {
        if (elem.fieldName()[0] == '$') {
            continue;
        }
        projBuilder.append(elem);
    }

    plan_cache_debug_info::CreatedFromQuery createdFromQuery =
        plan_cache_debug_info::CreatedFromQuery{
            findCommand.getFilter().getOwned(),
            findCommand.getSort().getOwned(),
            projBuilder.obj().getOwned(),
            query.getCollator() ? query.getCollator()->getSpec().toBSON() : BSONObj()};

    return {std::move(createdFromQuery), std::move(decision)};
}

plan_cache_debug_info::DebugInfoSBE buildDebugInfo(const QuerySolution* solution) {
    plan_cache_debug_info::DebugInfoSBE debugInfo;

    if (!solution || !solution->root())
        return debugInfo;

    std::queue<const QuerySolutionNode*> queue;
    queue.push(solution->root());

    // Look through the QuerySolution to collect some static stat details.
    while (!queue.empty()) {
        auto node = queue.front();
        queue.pop();
        invariant(node);

        switch (node->getType()) {
            case STAGE_COUNT_SCAN: {
                auto csn = static_cast<const CountScanNode*>(node);
                debugInfo.mainStats.indexesUsed.push_back(csn->index.identifier.catalogName);
                break;
            }
            case STAGE_DISTINCT_SCAN: {
                auto dn = static_cast<const DistinctNode*>(node);
                debugInfo.mainStats.indexesUsed.push_back(dn->index.identifier.catalogName);
                break;
            }
            case STAGE_GEO_NEAR_2D: {
                auto geo2d = static_cast<const GeoNear2DNode*>(node);
                debugInfo.mainStats.indexesUsed.push_back(geo2d->index.identifier.catalogName);
                break;
            }
            case STAGE_GEO_NEAR_2DSPHERE: {
                auto geo2dsphere = static_cast<const GeoNear2DSphereNode*>(node);
                debugInfo.mainStats.indexesUsed.push_back(
                    geo2dsphere->index.identifier.catalogName);
                break;
            }
            case STAGE_IXSCAN: {
                auto ixn = static_cast<const IndexScanNode*>(node);
                debugInfo.mainStats.indexesUsed.push_back(ixn->index.identifier.catalogName);
                break;
            }
            case STAGE_COLUMN_SCAN: {
                auto cisn = static_cast<const ColumnIndexScanNode*>(node);
                debugInfo.mainStats.indexesUsed.push_back(cisn->indexEntry.identifier.catalogName);
                break;
            }
            case STAGE_TEXT_MATCH: {
                auto tn = static_cast<const TextMatchNode*>(node);
                debugInfo.mainStats.indexesUsed.push_back(tn->index.identifier.catalogName);
                break;
            }
            case STAGE_COLLSCAN: {
                debugInfo.mainStats.collectionScans++;
                auto csn = static_cast<const CollectionScanNode*>(node);
                if (!csn->tailable) {
                    debugInfo.mainStats.collectionScansNonTailable++;
                }
                break;
            }
            case STAGE_EQ_LOOKUP: {
                auto eln = static_cast<const EqLookupNode*>(node);
                auto& secondaryStats = debugInfo.secondaryStats[eln->foreignCollection];
                if (eln->lookupStrategy == EqLookupNode::LookupStrategy::kIndexedLoopJoin) {
                    tassert(6466200, "Index join lookup should have an index entry", eln->idxEntry);
                    secondaryStats.indexesUsed.push_back(eln->idxEntry->identifier.catalogName);
                } else {
                    secondaryStats.collectionScans++;
                }
                [[fallthrough]];
            }
            default:
                break;
        }

        for (auto&& child : node->children) {
            queue.push(child.get());
        }
    }

    debugInfo.planSummary = solution->summaryString();

    return debugInfo;
}

void ClassicPlanCacheWriter::operator()(const CanonicalQuery& cq,
                                        MultiPlanStage& mps,
                                        std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
                                        std::vector<plan_ranker::CandidatePlan>& candidates) const {
    // Note this function is also called by ConditionalClassicPlanCacheWriter.

    if (_executeInSbe) {
        auto stats = mps.getStats();
        auto nReads = computeNumReadsFromStats(*stats, *ranking);

        updateClassicPlanCacheFromClassicCandidatesForSbeExecution(
            _opCtx, _collection.getCollectionPtr(), cq, nReads, std::move(ranking), candidates);
    } else {
        // We've been asked to write a works value, for classic execution.
        updateClassicPlanCacheFromClassicCandidatesForClassicExecution(
            _opCtx, _collection.getCollectionPtr(), cq, std::move(ranking), candidates);
    }
}

void ConditionalClassicPlanCacheWriter::operator()(
    const CanonicalQuery& cq,
    MultiPlanStage& mps,
    std::unique_ptr<plan_ranker::PlanRankingDecision> ranking,
    std::vector<plan_ranker::CandidatePlan>& candidates) const {
    if (shouldCacheBasedOnCachingMode(cq, *ranking, candidates)) {
        ClassicPlanCacheWriter::operator()(cq, mps, std::move(ranking), candidates);
    }
}

bool ConditionalClassicPlanCacheWriter::shouldCacheBasedOnCachingMode(
    const CanonicalQuery& cq,
    const plan_ranker::PlanRankingDecision& ranking,
    const std::vector<plan_ranker::CandidatePlan>& candidates) const {
    switch (_planCachingMode) {
        case Mode::AlwaysCache: {
            return true;
        }
        case Mode::NeverCache: {
            return false;
        }
        case Mode::SometimesCache: {
            auto winnerIdx = ranking.candidateOrder[0];
            invariant(winnerIdx >= 0 && winnerIdx < candidates.size());
            auto& winningPlan = candidates[winnerIdx];

            auto&& [winnerExplainer, runnerUpExplainer] =
                getClassicPlanExplainers(winningPlan, ranking, candidates);

            if (ranking.tieForBest()) {
                // These arrays having two or more entries is implied by 'tieForBest'.
                invariant(ranking.scores.size() > 1U);
                invariant(runnerUpExplainer);
                logTieForBest(cq.toStringShort(),
                              ranking.scores[0],
                              ranking.scores[1],
                              winnerExplainer->getPlanSummary(),
                              runnerUpExplainer->getPlanSummary());

                // The winning plan tied with the runner-up and we're using "sometimes cache" mode.
                // We will not write a plan cache entry.
                return false;
            }

            auto numResults =
                visit(OverloadedVisitor{[](const plan_ranker::StatsDetails& details) {
                                            return details.candidatePlanStats[0]->common.advanced;
                                        },
                                        [](const plan_ranker::SBEStatsDetails& details) {
                                            return details.candidatePlanStats[0]->common.advances;
                                        }},
                      ranking.stats);

            if (numResults == 0) {
                // We're using the "sometimes cache" mode, and the winning plan produced no results
                // during the plan ranking trial period. We will not write a plan cache entry.
                logNotCachingZeroResults(
                    cq.toStringShort(), ranking.scores[0], winnerExplainer->getPlanSummary());
                return false;
            }

            // The winning plan produced more than zero results and there was no tie between the
            // winner and runner up. In this case, we allow the winning plan to be cached.
            return true;
        }
    }

    MONGO_UNREACHABLE;
}

NumReads computeNumReadsFromStats(const PlanStageStats& stats,
                                  const plan_ranker::PlanRankingDecision& ranking) {
    auto winnerIdx = ranking.candidateOrder[0];
    auto summary = collectExecutionStatsSummary(&stats, winnerIdx);
    tassert(8523807,
            "Expected StatsDetails in classic runtime planner ranking decision.",
            std::holds_alternative<plan_ranker::StatsDetails>(ranking.stats));

    // The original "all classic" multiplanner uses the "works" stat as its unit of measure for
    // tracking how much work a plan has done, while this multiplanner (the "CRP SBE" multiplanner)
    // uses the "reads" metric (totalKeysExamined + totalDocsExamined) as its unit of measure.
    //
    // The "works" stat is always greater than zero. To play it safe and make it easier for the "all
    // classic" and "CRP SBE" multiplanners to coexist, this function makes sure to always return
    // a positive "reads" value.
    return NumReads{std::max<size_t>(summary.totalKeysExamined + summary.totalDocsExamined, 1)};
}

}  // namespace plan_cache_util
}  // namespace mongo
