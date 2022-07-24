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


#include "mongo/platform/basic.h"

#include "mongo/db/exec/plan_cache_util.h"

#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
namespace plan_cache_util {
namespace log_detail {
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
}  // namespace log_detail

void updatePlanCache(OperationContext* opCtx,
                     const MultipleCollectionAccessor& collections,
                     const CanonicalQuery& query,
                     const QuerySolution& solution,
                     const sbe::PlanStage& root,
                     const stage_builder::PlanStageData& data) {
    // TODO SERVER-67576: re-enable caching of "explode for sort" plans in the SBE cache.
    if (shouldCacheQuery(query) && collections.getMainCollection() &&
        !solution.hasExplodedForSort &&
        feature_flags::gFeatureFlagSbeFull.isEnabledAndIgnoreFCV()) {
        auto key = plan_cache_key_factory::make(query, collections);
        auto plan = std::make_unique<sbe::CachedSbePlan>(root.clone(), data);
        plan->indexFilterApplied = solution.indexFilterApplied;
        sbe::getPlanCache(opCtx).setPinned(
            std::move(key),
            canonical_query_encoder::computeHash(
                canonical_query_encoder::encodeForPlanCacheCommand(query)),
            std::move(plan),
            opCtx->getServiceContext()->getPreciseClockSource()->now(),
            buildDebugInfo(&solution));
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
                auto& secondaryStats = debugInfo.secondaryStats[eln->foreignCollection.toString()];
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
}  // namespace plan_cache_util
}  // namespace mongo
