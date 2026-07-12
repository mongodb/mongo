// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/explain.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_common.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_enumerator/plan_enumerator_explain_info.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer_impl.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/hex.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

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
                         ExplainOptions::Verbosity verbosity,
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
        const auto planningTimeOpt =
            CurOp::get(exec->getOpCtx())->debug().getAdditiveMetrics().planningTime;

        const Microseconds planningTime =
            planningTimeOpt ? planningTimeOpt.value() : Microseconds{0};

        plannerBob.appendNumber("optimizationTimeMillis",
                                durationCount<Milliseconds>(planningTime));
        plannerBob.appendNumber("optimizationTimeMicros",
                                durationCount<Microseconds>(planningTime));
    }

    if (!extraInfo.isEmpty()) {
        plannerBob.appendElements(extraInfo);
    }

    auto&& explainer = exec->getPlanExplainer();

    if (const auto ceSamplingMeta = explainer.getCeSamplingMetadata(); ceSamplingMeta.has_value()) {
        BSONObjBuilder ceSamplingMetaBob(plannerBob.subobjStart("ceSamplingMetadata"));
        for (const auto& [ns, meta] : ceSamplingMeta.value()) {
            BSONObjBuilder nsMetaBob(ceSamplingMetaBob.subobjStart(ns));
            nsMetaBob.append("sampleSource", meta.isPersisted ? "persisted" : "onTheFly");
            nsMetaBob.append("sampleTechnique", idlSerialize(meta.technique));
            if (meta.technique == ce::SamplingTechniqueEnum::kChunk) {
                tassert(12871302,
                        "numChunks must have a value when technique is chunk",
                        meta.numChunks);
                nsMetaBob.appendNumber("sampleNumChunks", *meta.numChunks);
            }
            nsMetaBob.appendNumber("sampleRequestedDocCount",
                                   static_cast<long long>(meta.requestedDocCount));
            nsMetaBob.appendNumber("sampleDocCount", static_cast<long long>(meta.docCount));
            nsMetaBob.appendNumber("sampleMemorySizeBytes",
                                   static_cast<long long>(meta.memorySizeBytes));
            tassert(12433203,
                    "SamplingMetadata::createdAt must be set before explain is generated",
                    meta.createdAt.has_value());
            nsMetaBob.appendDate("sampleCreatedAt", meta.createdAt.value());
        }
    }
    auto&& enumeratorInfo = explainer.getEnumeratorInfo();
    plannerBob.append("maxIndexedOrSolutionsReached", enumeratorInfo.hitIndexedOrLimit);
    plannerBob.append("maxIndexedAndSolutionsReached", enumeratorInfo.hitIndexedAndLimit);
    plannerBob.append("maxScansToExplodeReached", enumeratorInfo.hitScanLimit);
    plannerBob.append("prunedSimilarIndexes", enumeratorInfo.prunedAnyIndexes);

    auto&& [winningStats, _] = explainer.getWinningPlanStatsQueryPlanner(
        explainPolicyFor(verbosity).hasByteCode() /*printBytecode*/);
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
                                     boost::optional<long long> totalTimeMicros,
                                     BSONObjBuilder* out,
                                     bool isTrialPeriodInfo) {
    auto&& [stats, summary] = details;
    tassert(11320910, "summary must not be null", summary);

    out->appendNumber("nReturned", static_cast<long long>(summary->nReturned));

    // Time elapsed could might be either precise or approximate.
    if (totalTimeMicros) {
        out->appendNumber("executionTimeMillis", *totalTimeMicros / 1000);
        out->appendNumber("executionTimeMicros", *totalTimeMicros);
    } else {
        appendExecutionTimeFields(*out, summary->executionTime);
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
 * to the point of reaching EOF. Also assumes the verbosity's policy has exec stats
 * (ExplainPolicy::hasExecStats()).
 *
 * If the policy also has all-plans stats (ExplainPolicy::hasAllPlansStats()), it will include the
 * "allPlansExecution" array.
 *
 * - 'execPlanStatus' is OK if the query was exected successfully, or a non-OK status if there
 *   was a runtime error.
 */
void generateExecutionInfo(PlanExecutor* exec,
                           ExplainOptions::Verbosity verbosity,
                           Status executePlanStatus,
                           boost::optional<PlanExplainer::PlanStatsDetails> winningPlanTrialStats,
                           BSONObjBuilder* out) {
    const ExplainPolicy explainPolicy = explainPolicyFor(verbosity);
    tassert(11320911,
            fmt::format("The explain verbosity's policy must be 'executionStats' when generating "
                        "execution info, but found verbosity {}",
                        idl::serialize(verbosity)),
            explainPolicy.hasExecStats());

    auto&& explainer = exec->getPlanExplainer();

    if (explainPolicy.hasAllPlansStats() && explainer.areThereRejectedPlansToExplain()) {
        tassert(11320912,
                "winningPlanTrialStats must be present when requesting all execution stats",
                winningPlanTrialStats);
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
    auto elapsed = CurOp::get(opCtx)->elapsedTimeTotal();
    auto totalTimeMicros = durationCount<Microseconds>(elapsed);
    generateSinglePlanExecutionInfo(explainer.getWinningPlanStats(verbosity),
                                    totalTimeMicros,
                                    &execBob,
                                    false /* isTrialPeriodInfo */);

    // Also generate exec stats for all plans, if the verbosity level is high enough. These stats
    // reflect what happened during the trial period that ranked the plans.
    if (explainPolicy.hasAllPlansStats()) {
        // If we ranked multiple plans against each other, then add stats collected from the trial
        // period of the winning plan. The "allPlansExecution" section will contain an
        // apples-to-apples comparison of the winning plan's stats against all rejected plans' stats
        // collected during the trial period.
        BSONArrayBuilder allPlansBob(execBob.subarrayStart("allPlansExecution"));

        // If the winning plan was uncontested, leave the `allPlansExecution` array empty.
        if (explainer.areThereRejectedPlansToExplain()) {
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

/**
 * The V3 analogue of generatePlannerInfo(): the hook where the new (version 3) "queryPlanner"
 * output will be produced. It is invoked in place of generatePlannerInfo() when a V3 verbosity is
 * requested.
 *
 * TODO SERVER-130529 Implement the V3 queryPlanner output format here. Until then this skeleton
 * delegates to the legacy generatePlannerInfo() so the V3 modes produce meaningful (legacy-shaped)
 * output while reporting "explainVersion: '3'".
 * The legacy verbosity supplied by the caller is temporary until we reuse the legacy
 * implementation.
 */
void generatePlannerInfoV3(PlanExecutor* exec,
                           ExplainOptions::Verbosity legacyVerbosity,
                           const BSONObj& cmd,
                           const Explain::PlannerContext& plannerContext,
                           BSONObj extraInfo,
                           const SerializationContext& serializationContext,
                           BSONObjBuilder* out) {
    generatePlannerInfo(
        exec, legacyVerbosity, cmd, plannerContext, extraInfo, serializationContext, out);
}

/**
 * The V3 analogue of generateExecutionInfo(): the hook where the new (version 3) "executionStats"
 * output will be produced. It is invoked in place of generateExecutionInfo() when a V3 verbosity
 * that warrants execution statistics is requested.
 *
 * TODO SERVER-130529 Implement the V3 execution output format here. Until then this skeleton
 * delegates to the legacy generateExecutionInfo().
 * The legacy verbosity supplied by the caller is temporary until we reuse the legacy
 * implementation.
 */
void generateExecutionInfoV3(PlanExecutor* exec,
                             ExplainOptions::Verbosity legacyVerbosity,
                             Status executePlanStatus,
                             boost::optional<PlanExplainer::PlanStatsDetails> winningPlanTrialStats,
                             BSONObjBuilder* out) {
    generateExecutionInfo(exec, legacyVerbosity, executePlanStatus, winningPlanTrialStats, out);
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
                static_cast<long long>(entry.planCacheDecisionMetrics
                                           ? entry.planCacheDecisionMetrics->works.value
                                           : 0));
    out->append("reads",
                static_cast<long long>(entry.planCacheDecisionMetrics
                                           ? entry.planCacheDecisionMetrics->reads.value
                                           : 0));
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
    out->appendElements(explainVersionToBson(explainer.getVerbosityVersion(verbosity)));

    // Dispatch on the verbosity. Each V3 verbosity is routed to the V3 section generators, which
    // are the hooks for the future V3 output format; for now they reuse the legacy generators at
    // the nearest legacy verbosity (passed explicitly here). The legacy verbosities keep the
    // existing threshold-based behavior. This switch is intentionally exhaustive (no 'default') so
    // that any future verbosity must be classified here.
    // TODO SERVER-130529 Replace the legacy delegation in generatePlannerInfoV3()/
    // generateExecutionInfoV3() with the real V3 output format.
    // TODO SERVER-130529 Rewrite the switch as
    // if (isV3(verbosity)) {
    //     if (explainPolicy(verbosity).hasExecStats()) {
    //         generateV3exec();
    //     else ...
    //  else {
    //      if (explainPolicy(verbosity).hasExecStats()) {
    //          generateExecStats();
    //      else ...
    // }
    switch (verbosity) {
        case ExplainOptions::Verbosity::kPlanSummary:
        case ExplainOptions::Verbosity::kPlannerChoice:
            generatePlannerInfoV3(exec,
                                  ExplainOptions::Verbosity::kQueryPlanner,
                                  command,
                                  plannerContext,
                                  extraInfo,
                                  serializationContext,
                                  out);
            break;
        case ExplainOptions::Verbosity::kPlannerStats:
            // plannerStats includes the per-plan trial statistics that V1/V2 report via the
            // "allPlansExecution" section, so it reuses the legacy kExecAllPlans output.
            generatePlannerInfoV3(exec,
                                  ExplainOptions::Verbosity::kExecAllPlans,
                                  command,
                                  plannerContext,
                                  extraInfo,
                                  serializationContext,
                                  out);
            generateExecutionInfoV3(exec,
                                    ExplainOptions::Verbosity::kExecAllPlans,
                                    executePlanStatus,
                                    winningPlanTrialStats,
                                    out);
            break;
        case ExplainOptions::Verbosity::kExecStatsV3:
            // execStats has the same meaning as the legacy "executionStats" verbosity, so it reuses
            // the legacy kExecStats output.
            generatePlannerInfoV3(exec,
                                  ExplainOptions::Verbosity::kExecStats,
                                  command,
                                  plannerContext,
                                  extraInfo,
                                  serializationContext,
                                  out);
            generateExecutionInfoV3(exec,
                                    ExplainOptions::Verbosity::kExecStats,
                                    executePlanStatus,
                                    winningPlanTrialStats,
                                    out);
            break;
        case ExplainOptions::Verbosity::kQueryPlanner:
        case ExplainOptions::Verbosity::kExecStats:
        case ExplainOptions::Verbosity::kExecAllPlans:
        case ExplainOptions::Verbosity::kInternal:
            if (explainPolicyFor(verbosity).hasPlannerInfo()) {
                generatePlannerInfo(
                    exec, verbosity, command, plannerContext, extraInfo, serializationContext, out);
            }
            if (explainPolicyFor(verbosity).hasExecStats()) {
                generateExecutionInfo(
                    exec, verbosity, executePlanStatus, winningPlanTrialStats, out);
            }
            break;
    }

    explain_common::generateQueryShapeHash(exec->getOpCtx(), out);
    // Report peak tracked memory only at executionStats verbosity or higher, matching how execution
    // stats are reported. Memory consumed during planning/optimization is still counted in the
    // operation-wide total, but must not surface in a queryPlanner-verbosity explain (which does no
    // execution).
    if (explainPolicyFor(verbosity).hasExecStats()) {
        explain_common::generatePeakTrackedMemBytes(exec->getOpCtx(), out);
    }
    explain_common::appendIfRoom(command, "command", out);
}

void Explain::explainPipeline(PlanExecutor* exec,
                              bool executePipeline,
                              ExplainOptions::Verbosity verbosity,
                              const BSONObj& command,
                              BSONObjBuilder* out) {
    tassert(11320913, "exec must not be null", exec);
    tassert(11320914, "out must not be null", out);

    auto pipelineExec = dynamic_cast<PlanExecutorPipeline*>(exec);
    tassert(11320915, "expected 'exec' to be a PlanExecutorPipeline", pipelineExec);

    auto&& explainer = pipelineExec->getPlanExplainer();
    out->appendElements(explainVersionToBson(explainer.getVerbosityVersion(verbosity)));

    // Dispatch on the verbosity. Each V3 verbosity is routed to writeExplainOpsV3() (the hook for
    // the future V3 pipeline format), which for now reuses the legacy writeExplainOps() at the
    // nearest legacy verbosity passed explicitly here. Handing the stages a legacy verbosity keeps
    // every DocumentSource's threshold checks (and DocumentSourceCursor's cross-check) consistent.
    // The legacy verbosities keep the existing behavior. Exhaustive switch (no 'default') so any
    // future verbosity must be classified here.
    // TODO SERVER-130810 Replace the legacy delegation in writeExplainOpsV3() with the real V3
    // pipeline format. (TODO SERVER-32732: an execution error should be reported in explain rather
    // than failing the explain itself.)
    switch (verbosity) {
        case ExplainOptions::Verbosity::kPlanSummary:
        case ExplainOptions::Verbosity::kPlannerChoice:
            // Planner-only: do not execute the pipeline.
            *out << "stages"
                 << Value(
                        pipelineExec->writeExplainOpsV3(ExplainOptions::Verbosity::kQueryPlanner));
            break;
        case ExplainOptions::Verbosity::kPlannerStats:
            // plannerStats reuses the legacy kExecAllPlans output (see the find-path dispatch).
            if (executePipeline) {
                executePlan(pipelineExec);
            }
            *out << "stages"
                 << Value(
                        pipelineExec->writeExplainOpsV3(ExplainOptions::Verbosity::kExecAllPlans));
            break;
        case ExplainOptions::Verbosity::kExecStatsV3:
            // execStats has the same meaning as the legacy "executionStats" verbosity.
            if (executePipeline) {
                executePlan(pipelineExec);
            }
            *out << "stages"
                 << Value(pipelineExec->writeExplainOpsV3(ExplainOptions::Verbosity::kExecStats));
            break;
        case ExplainOptions::Verbosity::kQueryPlanner:
        case ExplainOptions::Verbosity::kExecStats:
        case ExplainOptions::Verbosity::kExecAllPlans:
        case ExplainOptions::Verbosity::kInternal:
            if (explainPolicyFor(verbosity).hasExecStats() && executePipeline) {
                executePlan(pipelineExec);
            }
            *out << "stages" << Value(pipelineExec->writeExplainOps(verbosity));
            break;
    }

    explain_common::generateQueryShapeHash(exec->getOpCtx(), out);
    // Report peak tracked memory only at executionStats verbosity or higher, matching how execution
    // stats are reported. Memory consumed during planning/optimization is still counted in the
    // operation-wide total, but must not surface in a queryPlanner-verbosity explain (which does no
    // execution).
    if (explainPolicyFor(verbosity).hasExecStats()) {
        explain_common::generatePeakTrackedMemBytes(exec->getOpCtx(), out);
    }
    explain_common::generateServerInfo(out);

    const auto& expCtx = pipelineExec->getPipeline()->getContext();
    explain_common::generateServerParameters(expCtx, out);
    explain_common::generateQueryKnobs(expCtx, out);
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

    // Whether the plan must be executed to gather execution statistics. This mirrors the verbosity
    // dispatch in the sibling explainStages() overload: the planner-only modes (legacy queryPlanner
    // and the planner-only V3 modes) do not execute; everything else does. Exhaustive switch (no
    // 'default') so a future verbosity must be classified here too.
    const bool requiresExecution = [&] {
        switch (verbosity) {
            case ExplainOptions::Verbosity::kQueryPlanner:
            case ExplainOptions::Verbosity::kPlanSummary:
            case ExplainOptions::Verbosity::kPlannerChoice:
                return false;
            case ExplainOptions::Verbosity::kExecStats:
            case ExplainOptions::Verbosity::kExecAllPlans:
            case ExplainOptions::Verbosity::kInternal:
            case ExplainOptions::Verbosity::kPlannerStats:
            case ExplainOptions::Verbosity::kExecStatsV3:
                return true;
        }
        MONGO_UNREACHABLE_TASSERT(10905002);
    }();

    // If we need execution stats, then run the plan in order to gather the stats.
    const MultipleCollectionAccessor emptyCollections;
    if (requiresExecution) {
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
    explain_common::generateQueryKnobs(expCtx, out);
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
        tassert(11320916, "decision must not be null", debugInfo.decision);

        // Add the 'createdFromQuery' object.
        {
            const auto& createdFromQuery = entry.debugInfo->createdFromQuery;
            BSONObjBuilder shapeBuilder{};
            shapeBuilder.append("query", createdFromQuery.filter);
            shapeBuilder.append("sort", createdFromQuery.sort);
            shapeBuilder.append("projection", createdFromQuery.projection);
            if (!createdFromQuery.collation.isEmpty()) {
                shapeBuilder.append("collation", createdFromQuery.collation);
            }
            if (!createdFromQuery.distinct.isEmpty()) {
                shapeBuilder.append("distinct", createdFromQuery.distinct);
            }
            explain_common::appendIfRoom(shapeBuilder.obj(), "createdFromQuery", out);
        }

        auto plannerStats = getCachedPlanStats(debugInfo, ExplainOptions::Verbosity::kQueryPlanner);
        auto execStats = getCachedPlanStats(debugInfo, ExplainOptions::Verbosity::kExecStats);

        tassert(11320917, "plannerStats must not be empty", plannerStats.size() > 0);
        explain_common::appendIfRoom(plannerStats[0].first, "cachedPlan", out);

        BSONArrayBuilder creationBuilder{};
        for (auto&& stats : execStats) {
            BSONObjBuilder planBob(creationBuilder.subobjStart());
            generateSinglePlanExecutionInfo(
                stats, boost::none, &planBob, false /* isTrialPeriodInfo */);
            planBob.doneFast();
        }
        explain_common::appendIfRoom(creationBuilder.arr(), "creationExecStats", out);

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

    sbe::DebugPrintInfo debugPrintInfo{};
    explain_common::appendIfRoom(
        BSON("slots" << entry.cachedPlan->planStageData.debugString() << "stages"
                     << sbe::DebugPrinter().print(*entry.cachedPlan->root, debugPrintInfo)),
        "cachedPlan",
        out);

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
        const auto planCacheKeyInfo = plan_cache_key_factory::make<PlanCacheKey>(
            *exec.getCanonicalQuery(), collections.getMainCollectionAcquisition());
        planCacheKeyHash = planCacheKeyInfo.planCacheKeyHash();
        planCacheShapeHash = planCacheKeyInfo.planCacheShapeHash();
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
