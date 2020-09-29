/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/query/explain.h"

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/count_scan.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/near.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/text.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_executor_sbe.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/server_options.h"
#include "mongo/util/hex.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"
#include "mongo/util/visit_helper.h"

namespace mongo {
namespace {
/**
 * Adds the 'queryPlanner' explain section to the BSON object being built by 'out'.
 *
 * This is a helper for generating explain BSON. It is used by explainStages(...).
 *
 * - 'exec' is a PlanExecutor which executes the plan for the operation being explained.
 * - 'collection' is the collection used in the operation. The caller should hold an IS lock on the
 *    collection which the query is for, even if 'collection' is nullptr.
 * - 'extraInfo' specifies additional information to include into the output.
 * - 'out' is a builder for the explain output.
 */
void generatePlannerInfo(PlanExecutor* exec,
                         const CollectionPtr& collection,
                         BSONObj extraInfo,
                         BSONObjBuilder* out) {
    BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));

    plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);
    plannerBob.append("namespace", exec->nss().ns());

    // Find whether there is an index filter set for the query shape. The 'indexFilterSet' field
    // will always be false in the case of EOF or idhack plans.
    bool indexFilterSet = false;
    boost::optional<uint32_t> queryHash;
    boost::optional<uint32_t> planCacheKeyHash;
    if (collection && exec->getCanonicalQuery()) {
        const QuerySettings* querySettings =
            QuerySettingsDecoration::get(collection->getSharedDecorations());
        PlanCacheKey planCacheKey = CollectionQueryInfo::get(collection)
                                        .getPlanCache()
                                        ->computeKey(*exec->getCanonicalQuery());
        planCacheKeyHash = canonical_query_encoder::computeHash(planCacheKey.toString());
        queryHash = canonical_query_encoder::computeHash(planCacheKey.getStableKeyStringData());

        if (auto allowedIndicesFilter =
                querySettings->getAllowedIndicesFilter(planCacheKey.getStableKey())) {
            // Found an index filter set on the query shape.
            indexFilterSet = true;
        }
    }
    plannerBob.append("indexFilterSet", indexFilterSet);

    // In general we should have a canonical query, but sometimes we may avoid creating a canonical
    // query as an optimization (specifically, the update system does not canonicalize for idhack
    // updates). In these cases, 'query' is NULL.
    auto query = exec->getCanonicalQuery();
    if (nullptr != query) {
        BSONObjBuilder parsedQueryBob(plannerBob.subobjStart("parsedQuery"));
        query->root()->serialize(&parsedQueryBob);
        parsedQueryBob.doneFast();

        if (query->getCollator()) {
            plannerBob.append("collation", query->getCollator()->getSpec().toBSON());
        }
    }

    if (queryHash) {
        plannerBob.append("queryHash", zeroPaddedHex(*queryHash));
    }

    if (planCacheKeyHash) {
        plannerBob.append("planCacheKey", zeroPaddedHex(*planCacheKeyHash));
    }

    if (!extraInfo.isEmpty()) {
        plannerBob.appendElements(extraInfo);
    }

    auto&& explainer = exec->getPlanExplainer();
    auto&& [winningStats, _] =
        explainer.getWinningPlanStats(ExplainOptions::Verbosity::kQueryPlanner);
    plannerBob.append("winningPlan", winningStats);

    BSONArrayBuilder bab{plannerBob.subarrayStart("rejectedPlans")};
    for (auto&& [rejectedStats, _] :
         explainer.getRejectedPlansStats(ExplainOptions::Verbosity::kQueryPlanner)) {
        bab.append(rejectedStats);
    }
    bab.doneFast();
    plannerBob.doneFast();
}

/**
 * Generates the execution stats section from the given 'PlanStatsDetails', adding the resulting
 * BSON document and specific execution metrics to 'out'.
 *
 * The 'totalTimeMillis' value passed here will be added to the top level of the execution stats
 * section, but will not affect the reporting of timing for individual stages. If 'totalTimeMillis'
 * is not set, we use the approximate timing information collected by the stages.
 *
 * Stats are generated at the verbosity specified by 'verbosity'.
 */
void generateSinglePlanExecutionInfo(const PlanExplainer::PlanStatsDetails& details,
                                     boost::optional<long long> totalTimeMillis,
                                     BSONObjBuilder* out) {
    auto&& [stats, summary] = details;
    invariant(summary);

    out->appendNumber("nReturned", summary->nReturned);

    // Time elapsed could might be either precise or approximate.
    if (totalTimeMillis) {
        out->appendNumber("executionTimeMillis", *totalTimeMillis);
    } else {
        out->appendNumber("executionTimeMillisEstimate", summary->executionTimeMillisEstimate);
    }

    out->appendNumber("totalKeysExamined", summary->totalKeysExamined);
    out->appendNumber("totalDocsExamined", summary->totalDocsExamined);

    if (summary->planFailed) {
        out->appendBool("failed", true);
    }

    // Add the tree of stages, with individual execution stats for each stage.
    out->append("executionStages", stats);
}

/**
 * Adds the "executionStats" field to out. Assumes that the PlanExecutor has already been executed
 * to the point of reaching EOF. Also assumes that verbosity >= kExecStats.
 *
 * If verbosity >= kExecAllPlans, it will include the "allPlansExecution" array.
 *
 * - 'execPlanStatus' is OK if the query was exected successfully, or a non-OK status if there
 *   was a runtime error.
 */
void generateExecutionInfo(PlanExecutor* exec,
                           ExplainOptions::Verbosity verbosity,
                           Status executePlanStatus,
                           boost::optional<PlanExplainer::PlanStatsDetails> winningPlanTrialStats,
                           BSONObjBuilder* out) {
    invariant(verbosity >= ExplainOptions::Verbosity::kExecStats);

    auto&& explainer = exec->getPlanExplainer();

    if (verbosity >= ExplainOptions::Verbosity::kExecAllPlans && explainer.isMultiPlan()) {
        invariant(winningPlanTrialStats,
                  "winningPlanTrialStats must be present when requesting all execution stats");
    }
    BSONObjBuilder execBob(out->subobjStart("executionStats"));

    // If there is an execution error while running the query, the error is reported under the
    // "executionStats" section and the explain as a whole succeeds.
    execBob.append("executionSuccess", executePlanStatus.isOK());
    if (!executePlanStatus.isOK()) {
        execBob.append("errorMessage", executePlanStatus.reason());
        execBob.append("errorCode", executePlanStatus.code());
    }

    // Generate exec stats BSON for the winning plan.
    auto opCtx = exec->getOpCtx();
    auto totalTimeMillis = durationCount<Milliseconds>(CurOp::get(opCtx)->elapsedTimeTotal());
    generateSinglePlanExecutionInfo(
        explainer.getWinningPlanStats(verbosity), totalTimeMillis, &execBob);

    // Also generate exec stats for all plans, if the verbosity level is high enough. These stats
    // reflect what happened during the trial period that ranked the plans.
    if (verbosity >= ExplainOptions::Verbosity::kExecAllPlans) {
        // If we ranked multiple plans against each other, then add stats collected from the trial
        // period of the winning plan. The "allPlansExecution" section will contain an
        // apples-to-apples comparison of the winning plan's stats against all rejected plans' stats
        // collected during the trial period.
        BSONArrayBuilder allPlansBob(execBob.subarrayStart("allPlansExecution"));

        if (winningPlanTrialStats) {
            BSONObjBuilder planBob(allPlansBob.subobjStart());
            generateSinglePlanExecutionInfo(*winningPlanTrialStats, boost::none, &planBob);
            planBob.doneFast();
        }

        for (auto&& stats : explainer.getRejectedPlansStats(verbosity)) {
            BSONObjBuilder planBob(allPlansBob.subobjStart());
            generateSinglePlanExecutionInfo(stats, boost::none, &planBob);
            planBob.doneFast();
        }

        allPlansBob.doneFast();
    }

    execBob.doneFast();
}

/**
 * Executes the given plan executor, discarding the resulting documents, until it reaches EOF. If a
 * runtime error occur or execution is killed, throws a DBException.
 *
 * If 'exec' is configured for yielding, then a call to this helper could result in a yield.
 */
void executePlan(PlanExecutor* exec) {
    BSONObj obj;
    while (exec->getNext(&obj, nullptr) == PlanExecutor::ADVANCED) {
        // Discard the resulting documents.
    }
}
}  // namespace

void Explain::explainStages(PlanExecutor* exec,
                            const CollectionPtr& collection,
                            ExplainOptions::Verbosity verbosity,
                            Status executePlanStatus,
                            boost::optional<PlanExplainer::PlanStatsDetails> winningPlanTrialStats,
                            BSONObj extraInfo,
                            BSONObjBuilder* out) {
    //
    // Use the stats trees to produce explain BSON.
    //

    if (verbosity >= ExplainOptions::Verbosity::kQueryPlanner) {
        generatePlannerInfo(exec, collection, extraInfo, out);
    }

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        generateExecutionInfo(exec, verbosity, executePlanStatus, winningPlanTrialStats, out);
    }
}

void Explain::explainPipeline(PlanExecutor* exec,
                              bool executePipeline,
                              ExplainOptions::Verbosity verbosity,
                              BSONObjBuilder* out) {
    invariant(exec);
    invariant(out);

    auto pipelineExec = dynamic_cast<PlanExecutorPipeline*>(exec);
    invariant(pipelineExec);

    // If we need execution stats, this runs the plan in order to gather the stats.
    if (verbosity >= ExplainOptions::Verbosity::kExecStats && executePipeline) {
        // TODO SERVER-32732: An execution error should be reported in explain, but should not
        // cause the explain itself to fail.
        executePlan(pipelineExec);
    }

    *out << "stages" << Value(pipelineExec->writeExplainOps(verbosity));

    explain_common::generateServerInfo(out);
}

void Explain::explainStages(PlanExecutor* exec,
                            const CollectionPtr& collection,
                            ExplainOptions::Verbosity verbosity,
                            BSONObj extraInfo,
                            BSONObjBuilder* out) {
    auto&& explainer = exec->getPlanExplainer();
    auto winningPlanTrialStats = explainer.getWinningPlanStats(verbosity);
    Status executePlanStatus = Status::OK();
    const CollectionPtr* collectionPtr = &collection;

    // If we need execution stats, then run the plan in order to gather the stats.
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        try {
            executePlan(exec);
        } catch (const DBException&) {
            executePlanStatus = exceptionToStatus();
        }

        // If executing the query failed, for any number of reasons other than a planning failure,
        // then the collection may no longer be valid. We conservatively set our collection pointer
        // to null in case it is invalid.
        if (executePlanStatus != ErrorCodes::NoQueryExecutionPlans) {
            collectionPtr = &CollectionPtr::null;
        }
    }

    explainStages(
        exec, *collectionPtr, verbosity, executePlanStatus, winningPlanTrialStats, extraInfo, out);

    explain_common::generateServerInfo(out);
}


void Explain::planCacheEntryToBSON(const PlanCacheEntry& entry, BSONObjBuilder* out) {
    out->append("queryHash", zeroPaddedHex(entry.queryHash));
    out->append("planCacheKey", zeroPaddedHex(entry.planCacheKey));

    // Append whether or not the entry is active.
    out->append("isActive", entry.isActive);
    out->append("works", static_cast<long long>(entry.works));
    out->append("timeOfCreation", entry.timeOfCreation);

    if (entry.debugInfo) {
        const auto& debugInfo = *entry.debugInfo;
        invariant(debugInfo.decision);

        // Add the 'createdFromQuery' object.
        {
            const auto& createdFromQuery = entry.debugInfo->createdFromQuery;
            BSONObjBuilder shapeBuilder(out->subobjStart("createdFromQuery"));
            shapeBuilder.append("query", createdFromQuery.filter);
            shapeBuilder.append("sort", createdFromQuery.sort);
            shapeBuilder.append("projection", createdFromQuery.projection);
            if (!createdFromQuery.collation.isEmpty()) {
                shapeBuilder.append("collation", createdFromQuery.collation);
            }
        }

        auto explainer = stdx::visit(
            visit_helper::Overloaded{
                [](const std::vector<std::unique_ptr<PlanStageStats>>& stats) {
                    return plan_explainer_factory::make(nullptr);
                },
                [](const std::vector<std::unique_ptr<sbe::PlanStageStats>>& stats) {
                    return plan_explainer_factory::make(nullptr, nullptr);
                }},
            debugInfo.decision->stats);
        auto plannerStats =
            explainer->getCachedPlanStats(debugInfo, ExplainOptions::Verbosity::kQueryPlanner);
        auto execStats =
            explainer->getCachedPlanStats(debugInfo, ExplainOptions::Verbosity::kExecStats);

        invariant(plannerStats.size() > 0);
        out->append("cachedPlan", plannerStats[0].first);

        BSONArrayBuilder creationBuilder(out->subarrayStart("creationExecStats"));
        for (auto&& stats : execStats) {
            BSONObjBuilder planBob(creationBuilder.subobjStart());
            generateSinglePlanExecutionInfo(stats, boost::none, &planBob);
            planBob.doneFast();
        }
        creationBuilder.doneFast();

        BSONArrayBuilder scoresBuilder(out->subarrayStart("candidatePlanScores"));
        for (double score : debugInfo.decision->scores) {
            scoresBuilder.append(score);
        }

        std::for_each(debugInfo.decision->failedCandidates.begin(),
                      debugInfo.decision->failedCandidates.end(),
                      [&scoresBuilder](const auto&) { scoresBuilder.append(0.0); });
        scoresBuilder.doneFast();
    }

    out->append("indexFilterSet", entry.plannerData->indexFilterApplied);

    out->append("estimatedSizeBytes", static_cast<long long>(entry.estimatedEntrySizeBytes));
}
}  // namespace mongo
