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

#include "mongo/db/query/explain.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/hex.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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
                         const BSONObj& cmd,
                         const Explain::PlannerContext& plannerContext,
                         BSONObj extraInfo,
                         const SerializationContext& serializationContext,
                         BSONObjBuilder* out) {
    BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));

    plannerBob.append("namespace",
                      NamespaceStringUtil::serialize(exec->nss(), serializationContext));

    // In general we should have a canonical query, but sometimes we may avoid creating a canonical
    // query as an optimization (specifically, the update system does not canonicalize for idhack
    // updates). In these cases, 'query' is NULL.
    auto query = exec->getCanonicalQuery();

    if (query) {
        BSONObjBuilder parsedQueryBob(plannerBob.subobjStart("parsedQuery"));
        query->getPrimaryMatchExpression()->serialize(&parsedQueryBob, {});
        parsedQueryBob.doneFast();

        if (query->getCollator()) {
            plannerBob.append("collation", query->getCollator()->getSpec().toBSON());
        }

        // If there exists a matching index filter, set 'indexFilterSet' to false if query settings
        // set, as they have higher priority.
        auto& querySettings = query->getExpCtx()->getQuerySettings();
        if (auto querySettingsBSON = querySettings.toBSON(); !querySettingsBSON.isEmpty()) {
            plannerBob.append("querySettings", querySettingsBSON);
            plannerBob.append("indexFilterSet", false);
        } else {
            plannerBob.append("indexFilterSet", plannerContext.indexFilterSet);
        }
    }

    if (plannerContext.planCacheShapeHash) {
        // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
        std::string planCacheShapeHashStr = zeroPaddedHex(*plannerContext.planCacheShapeHash);
        plannerBob.append("queryHash", planCacheShapeHashStr);
        plannerBob.append("planCacheShapeHash", planCacheShapeHashStr);
    }

    if (plannerContext.planCacheKeyHash) {
        plannerBob.append("planCacheKey", zeroPaddedHex(*plannerContext.planCacheKeyHash));
    }

    if (exec->getOpCtx() != nullptr) {
        plannerBob.appendNumber(
            "optimizationTimeMillis",
            durationCount<Milliseconds>(CurOp::get(exec->getOpCtx())->debug().planningTime));
    }

    if (!extraInfo.isEmpty()) {
        plannerBob.appendElements(extraInfo);
    }

    auto&& explainer = exec->getPlanExplainer();
    auto&& enumeratorInfo = explainer.getEnumeratorInfo();
    plannerBob.append("maxIndexedOrSolutionsReached", enumeratorInfo.hitIndexedOrLimit);
    plannerBob.append("maxIndexedAndSolutionsReached", enumeratorInfo.hitIndexedAndLimit);
    plannerBob.append("maxScansToExplodeReached", enumeratorInfo.hitScanLimit);
    plannerBob.append("prunedSimilarIndexes", enumeratorInfo.prunedAnyIndexes);

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
 * The 'isTrialPeriodInfo' value indicates whether the function was called to generate the
 * stats collected during the trial period of the plan selection phase, i.e is this section being
 * generated for the 'allPlansExecution' field.
 *
 * Stats are generated at the verbosity specified by 'verbosity'.
 */
void generateSinglePlanExecutionInfo(const PlanExplainer::PlanStatsDetails& details,
                                     boost::optional<long long> totalTimeMillis,
                                     BSONObjBuilder* out,
                                     bool isTrialPeriodInfo) {
    auto&& [stats, summary] = details;
    invariant(summary);

    out->appendNumber("nReturned", static_cast<long long>(summary->nReturned));

    // Time elapsed could might be either precise or approximate.
    if (totalTimeMillis) {
        out->appendNumber("executionTimeMillis", *totalTimeMillis);
    } else {
        if (summary->executionTime.precision == QueryExecTimerPrecision::kNanos) {
            out->appendNumber(
                "executionTimeMillisEstimate",
                durationCount<Milliseconds>(summary->executionTime.executionTimeEstimate));
            out->appendNumber(
                "executionTimeMicros",
                durationCount<Microseconds>(summary->executionTime.executionTimeEstimate));
            out->appendNumber(
                "executionTimeNanos",
                durationCount<Nanoseconds>(summary->executionTime.executionTimeEstimate));
        } else if (summary->executionTime.precision == QueryExecTimerPrecision::kMillis) {
            out->appendNumber(
                "executionTimeMillisEstimate",
                durationCount<Milliseconds>(summary->executionTime.executionTimeEstimate));
        }
    }

    out->appendNumber("totalKeysExamined", static_cast<long long>(summary->totalKeysExamined));
    out->appendNumber("totalDocsExamined", static_cast<long long>(summary->totalDocsExamined));

    if (summary->planFailed) {
        out->appendBool("failed", true);
    }

    // Only the scores calculated from the trial period should be outputted alongside each plan
    // in 'allPlansExecution' and not alongside the winning plan stats in 'executionStats'.
    if (isTrialPeriodInfo && summary->score) {
        out->appendNumber("score", *summary->score);
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
    generateSinglePlanExecutionInfo(explainer.getWinningPlanStats(verbosity),
                                    totalTimeMillis,
                                    &execBob,
                                    false /* isTrialPeriodInfo */);

    // Also generate exec stats for all plans, if the verbosity level is high enough. These stats
    // reflect what happened during the trial period that ranked the plans.
    if (verbosity >= ExplainOptions::Verbosity::kExecAllPlans) {
        // If we ranked multiple plans against each other, then add stats collected from the trial
        // period of the winning plan. The "allPlansExecution" section will contain an
        // apples-to-apples comparison of the winning plan's stats against all rejected plans' stats
        // collected during the trial period.
        BSONArrayBuilder allPlansBob(execBob.subarrayStart("allPlansExecution"));

        // If the winning plan was uncontested, leave the `allPlansExecution` array empty.
        if (explainer.isMultiPlan()) {
            BSONObjBuilder planBob(allPlansBob.subobjStart());
            generateSinglePlanExecutionInfo(
                *winningPlanTrialStats, boost::none, &planBob, true /* isTrialPeriodInfo */);
            planBob.doneFast();

            for (auto&& stats : explainer.getRejectedPlansStats(verbosity)) {
                BSONObjBuilder planBob(allPlansBob.subobjStart());
                generateSinglePlanExecutionInfo(
                    stats, boost::none, &planBob, true /* isTrialPeriodInfo */);
                planBob.doneFast();
            }
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
    // Using 'getNextBatch()' rather than 'getNext()' means we iterate the PlanExecutor in a tighter
    // loop. We are passing a null callback function because explain wishes to simply discard the
    // query result set.
    (void)exec->getNextBatch(std::numeric_limits<int64_t>::max(), nullptr);
}

/**
 * Returns a BSON document in the form of {explainVersion: <version>} with the 'version' parameter
 * serialized into the <version> element.
 */
BSONObj explainVersionToBson(const PlanExplainer::ExplainVersion& version) {
    return BSON("explainVersion" << version);
}

template <typename EntryType>
void appendBasicPlanCacheEntryInfoToBSON(const EntryType& entry, BSONObjBuilder* out) {
    // TODO SERVER-93305: Remove deprecated 'queryHash' usages.
    std::string planCacheShapeHashStr = zeroPaddedHex(entry.planCacheShapeHash);
    out->append("queryHash", planCacheShapeHashStr);
    out->append("planCacheShapeHash", planCacheShapeHashStr);
    out->append("planCacheKey", zeroPaddedHex(entry.planCacheKey));
    out->append("isActive", entry.isActive);
    out->append("works",
                static_cast<long long>(entry.readsOrWorks ? entry.readsOrWorks->rawValue() : 0));
    if (entry.readsOrWorks) {
        out->append("worksType", entry.readsOrWorks->type());
    }
    out->append("timeOfCreation", entry.timeOfCreation);

    if (entry.securityLevel == PlanSecurityLevel::kSensitive) {
        out->append("securityLevel", entry.securityLevel);
    }
}
}  // namespace

void Explain::explainStages(PlanExecutor* exec,
                            const MultipleCollectionAccessor& collections,
                            ExplainOptions::Verbosity verbosity,
                            Status executePlanStatus,
                            boost::optional<PlanExplainer::PlanStatsDetails> winningPlanTrialStats,
                            BSONObj extraInfo,
                            const SerializationContext& serializationContext,
                            const BSONObj& command,
                            BSONObjBuilder* out) {
    auto collInfo = Explain::makePlannerContext(*exec, collections);
    Explain::explainStages(exec,
                           collInfo,
                           verbosity,
                           executePlanStatus,
                           winningPlanTrialStats,
                           extraInfo,
                           serializationContext,
                           command,
                           out);
}

void Explain::explainStages(PlanExecutor* exec,
                            const Explain::PlannerContext& plannerContext,
                            ExplainOptions::Verbosity verbosity,
                            Status executePlanStatus,
                            boost::optional<PlanExplainer::PlanStatsDetails> winningPlanTrialStats,
                            BSONObj extraInfo,
                            const SerializationContext& serializationContext,
                            const BSONObj& command,
                            BSONObjBuilder* out) {
    //
    // Use the stats trees to produce explain BSON.
    //

    auto&& explainer = exec->getPlanExplainer();
    out->appendElements(explainVersionToBson(explainer.getVersion()));

    if (verbosity >= ExplainOptions::Verbosity::kQueryPlanner) {
        generatePlannerInfo(exec, command, plannerContext, extraInfo, serializationContext, out);
    }

    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        generateExecutionInfo(exec, verbosity, executePlanStatus, winningPlanTrialStats, out);
    }

    explain_common::generateQueryShapeHash(exec->getOpCtx(), out);
    explain_common::generatePeakTrackedMemBytes(exec->getOpCtx(), out);
    explain_common::appendIfRoom(command, "command", out);
}

void Explain::explainPipeline(PlanExecutor* exec,
                              bool executePipeline,
                              ExplainOptions::Verbosity verbosity,
                              const BSONObj& command,
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

    auto&& explainer = pipelineExec->getPlanExplainer();
    out->appendElements(explainVersionToBson(explainer.getVersion()));
    *out << "stages" << Value(pipelineExec->writeExplainOps(verbosity));

    explain_common::generateQueryShapeHash(exec->getOpCtx(), out);
    explain_common::generatePeakTrackedMemBytes(exec->getOpCtx(), out);
    explain_common::generateServerInfo(out);

    auto* cq = pipelineExec->getCanonicalQuery();
    const auto& expCtx = cq
        ? cq->getExpCtx()
        : makeBlankExpressionContext(pipelineExec->getOpCtx(), pipelineExec->nss());
    explain_common::generateServerParameters(expCtx, out);
    explain_common::appendIfRoom(command, "command", out);
}

void Explain::explainStages(PlanExecutor* exec,
                            const MultipleCollectionAccessor& collections,
                            ExplainOptions::Verbosity verbosity,
                            BSONObj extraInfo,
                            const SerializationContext& serializationContext,
                            const BSONObj& command,
                            BSONObjBuilder* out) {
    auto&& explainer = exec->getPlanExplainer();
    auto winningPlanTrialStats = explainer.getWinningPlanTrialStats();
    Status executePlanStatus = Status::OK();
    const MultipleCollectionAccessor* collectionsPtr = &collections;

    // If we need execution stats, then run the plan in order to gather the stats.
    const MultipleCollectionAccessor emptyCollections;
    if (verbosity >= ExplainOptions::Verbosity::kExecStats) {
        try {
            executePlan(exec);
        } catch (const DBException&) {
            executePlanStatus = exceptionToStatus();
        }

        // If executing the query failed, for any number of reasons other than a planning failure,
        // then the collection may no longer be valid. We conservatively set our collection pointer
        // to null in case it is invalid.
        if (!executePlanStatus.isOK() && executePlanStatus != ErrorCodes::NoQueryExecutionPlans) {
            collectionsPtr = &emptyCollections;
        }
    }

    explainStages(exec,
                  *collectionsPtr,
                  verbosity,
                  executePlanStatus,
                  winningPlanTrialStats,
                  extraInfo,
                  serializationContext,
                  command,
                  out);

    explain_common::generateServerInfo(out);
    auto* cq = exec->getCanonicalQuery();
    const auto& expCtx =
        cq ? cq->getExpCtx() : makeBlankExpressionContext(exec->getOpCtx(), exec->nss());
    explain_common::generateServerParameters(expCtx, out);
}

void Explain::explainStages(PlanExecutor* exec,
                            const CollectionAcquisition& collection,
                            ExplainOptions::Verbosity verbosity,
                            BSONObj extraInfo,
                            const SerializationContext& serializationContext,
                            const BSONObj& command,
                            BSONObjBuilder* out) {
    explainStages(exec,
                  MultipleCollectionAccessor(collection),
                  verbosity,
                  extraInfo,
                  serializationContext,
                  command,
                  out);
}

void Explain::planCacheEntryToBSON(const PlanCacheEntry& entry, BSONObjBuilder* out) {
    out->append("version", "1");

    appendBasicPlanCacheEntryInfoToBSON(entry, out);

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
            if (!createdFromQuery.distinct.isEmpty()) {
                shapeBuilder.append("distinct", createdFromQuery.distinct);
            }
        }

        auto plannerStats = getCachedPlanStats(debugInfo, ExplainOptions::Verbosity::kQueryPlanner);
        auto execStats = getCachedPlanStats(debugInfo, ExplainOptions::Verbosity::kExecStats);

        invariant(plannerStats.size() > 0);
        out->append("cachedPlan", plannerStats[0].first);

        BSONArrayBuilder creationBuilder(out->subarrayStart("creationExecStats"));
        for (auto&& stats : execStats) {
            BSONObjBuilder planBob(creationBuilder.subobjStart());
            generateSinglePlanExecutionInfo(
                stats, boost::none, &planBob, false /* isTrialPeriodInfo */);
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

    out->append("indexFilterSet", entry.cachedPlan->indexFilterApplied);

    out->append("estimatedSizeBytes", static_cast<long long>(entry.estimatedEntrySizeBytes));
    out->append("solutionHash", static_cast<long long>(entry.cachedPlan->solutionHash));
}

void Explain::planCacheEntryToBSON(const sbe::PlanCacheEntry& entry, BSONObjBuilder* out) {
    out->append("version", "2");

    appendBasicPlanCacheEntryInfoToBSON(entry, out);

    out->append("cachedPlan",
                BSON("slots" << entry.cachedPlan->planStageData.debugString() << "stages"
                             << sbe::DebugPrinter().print(*entry.cachedPlan->root)));

    out->append("indexFilterSet", entry.cachedPlan->indexFilterApplied);
    out->append("isPinned", entry.isPinned());
    out->append("estimatedSizeBytes", static_cast<long long>(entry.estimatedEntrySizeBytes));
    out->append("solutionHash", static_cast<long long>(entry.cachedPlan->solutionHash));
}

Explain::PlannerContext Explain::makePlannerContext(const PlanExecutor& exec,
                                                    const MultipleCollectionAccessor& collections) {
    const bool mainCollExists = collections.hasMainCollection();

    boost::optional<uint32_t> planCacheKeyHash;
    boost::optional<uint32_t> planCacheShapeHash;

    if (auto* cq = exec.getCanonicalQuery(); mainCollExists && cq) {
        if (cq->isSbeCompatible() && cq->isUsingSbePlanCache() &&
            feature_flags::gFeatureFlagSbeFull.isEnabled()) {
            const auto planCacheKeyInfo =
                plan_cache_key_factory::make(*exec.getCanonicalQuery(), collections);
            planCacheKeyHash = planCacheKeyInfo.planCacheKeyHash();
            planCacheShapeHash = planCacheKeyInfo.planCacheShapeHash();
        } else {
            const auto planCacheKeyInfo = plan_cache_key_factory::make<PlanCacheKey>(
                *exec.getCanonicalQuery(), collections.getMainCollectionAcquisition());
            planCacheKeyHash = planCacheKeyInfo.planCacheKeyHash();
            planCacheShapeHash = planCacheKeyInfo.planCacheShapeHash();
        }
    }

    // If there exists a matching index filter, set 'indexFilterSet' to false if query settings
    // set, as they have higher priority.
    bool indexFilterSet = false;
    if (auto query = exec.getCanonicalQuery()) {
        auto& querySettings = query->getExpCtx()->getQuerySettings();
        if (auto querySettingsBSON = querySettings.toBSON(); !querySettingsBSON.isEmpty()) {
            indexFilterSet = false;
        } else {
            indexFilterSet = [&]() {
                if (!mainCollExists) {
                    return false;
                }
                const auto* indexFilters = QuerySettingsDecoration::get(
                    collections.getMainCollection()->getSharedDecorations());
                return indexFilters->getAllowedIndicesFilter(*query).has_value();
            }();
        }
    }
    return {mainCollExists, planCacheShapeHash, planCacheKeyHash, indexFilterSet};
}

}  // namespace mongo
