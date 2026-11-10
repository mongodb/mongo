// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/executor.h"

#include "mongo/base/status_with.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_join_hint.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/catalog_stats.h"
#include "mongo/db/query/compiler/optimizer/join/fallback_reason.h"
#include "mongo/db/query/compiler/optimizer/join/hint.h"
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/join/join_plan_cache_util.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/reorder_joins.h"
#include "mongo/db/query/compiler/optimizer/join/server_status_metrics.h"
#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/query/plan_cache/join_plan_cache_key.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/query/plan_ranking/plan_selection_strategy.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_join_optimization_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <algorithm>
#include <variant>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryJoin

namespace mongo::join_ordering {

MONGO_FAIL_POINT_DEFINE(sleepWhileJoinOptimizing);
MONGO_FAIL_POINT_DEFINE(hangAfterJoinModelConstruction);

// These sleep for {ms: <millis>} inside the phase they name, so that tests can verify that
// 'sbeLoweringTimeMicros' and 'planEnumerationTimeMicros' measure those phases.
MONGO_FAIL_POINT_DEFINE(sleepWhileLoweringJoinPlanToSbe);
MONGO_FAIL_POINT_DEFINE(sleepWhileEnumeratingJoinPlans);

namespace {
PlanTreeShape getPlanTreeShape(JoinPlanTreeShapeEnum shape) {
    switch (shape) {
        case JoinPlanTreeShapeEnum::kLeftDeep:
            return PlanTreeShape::LEFT_DEEP;
        case JoinPlanTreeShapeEnum::kRightDeep:
            return PlanTreeShape::RIGHT_DEEP;
        case JoinPlanTreeShapeEnum::kZigZag:
            return PlanTreeShape::ZIG_ZAG;
        default:
            MONGO_UNREACHABLE_TASSERT(11336914);
    }
}

boost::optional<JoinMethod> getJoinMethod(ForcedJoinMethodEnum algorithm) {
    switch (algorithm) {
        case ForcedJoinMethodEnum::kAny:
            return boost::none;
        case ForcedJoinMethodEnum::kHJ:
            return JoinMethod::HJ;
        case ForcedJoinMethodEnum::kINLJ:
            return JoinMethod::INLJ;
        case ForcedJoinMethodEnum::kNLJ:
            return JoinMethod::NLJ;
        default:
            MONGO_UNREACHABLE_TASSERT(12018700);
    }
}

PerSubsetLevelEnumerationMode getMode(size_t minLevel,
                                      size_t maxLevel,
                                      boost::optional<JoinHint> hint = boost::none) {
    // Only try to update the enumeration mode to ALL if the query knobs are set to sane values.
    if (minLevel < maxLevel && minLevel < kHardMaxNodesInJoin) {
        if (minLevel == 0) {
            return {{{0, PlanEnumerationMode::ALL, hint},
                     {maxLevel, PlanEnumerationMode::CHEAPEST, hint}}};
        }
        return {{{0, PlanEnumerationMode::CHEAPEST, hint},
                 {minLevel, PlanEnumerationMode::ALL, hint},
                 {maxLevel, PlanEnumerationMode::CHEAPEST, hint}}};
    }

    return {{{0, PlanEnumerationMode::CHEAPEST, hint}}};
}

EnumerationStrategy getEnumerationStrategy(const QueryKnobConfiguration& qkc) {
    auto minLevel = qkc.getInternalMinAllPlansEnumerationSubsetLevel();
    auto maxLevel = qkc.getInternalMaxAllPlansEnumerationSubsetLevel();
    auto joinMethod = getJoinMethod(qkc.getJoinMethod());

    // Override the join method for all joins if specified by the 'internalJoinMethod' query knob.
    auto methodHint = joinMethod
        ? boost::optional<JoinHint>(JoinHint{boost::none, *joinMethod, boost::none})
        : boost::none;

    return {.planShape = getPlanTreeShape(qkc.getJoinPlanTreeShape()),
            .mode = getMode(minLevel, maxLevel, methodHint),
            .enableHJOrderPruning = qkc.getEnableJoinEnumerationHJOrderPruning()};
}

JoinExplainMode getExplainMode(const ExpressionContext& expCtx, const QueryKnobConfiguration& qkc) {
    if (!expCtx.getExplain()) {
        return JoinExplainMode::kNone;
    }
    return qkc.getEnableDetailedJoinExplain() ? JoinExplainMode::kDetailed
                                              : JoinExplainMode::kDefault;
}

/**
 * These return the reason the collection is ineligible for join optimization, or boost::none if it
 * is eligible.
 */
boost::optional<JoinFallbackReason> isCollPtrEligibleForJoinOpt(const CollectionPtr& coll) {
    if (coll->isCapped()) {
        return JoinFallbackReason::kCollectionCapped;
    }
    if (coll->isClustered()) {
        return JoinFallbackReason::kCollectionClustered;
    }
    if (!CollatorInterface::isSimpleCollator(coll->getDefaultCollator())) {
        return JoinFallbackReason::kCollectionCollation;
    }
    return boost::none;
}

boost::optional<JoinFallbackReason> isCollectionEligibleForJoinOpt(
    const CollectionAcquisition& coll) {
    if (!coll.exists()) {
        return JoinFallbackReason::kCollectionMissing;
    }
    if (coll.getShardingDescription().isSharded()) {
        return JoinFallbackReason::kCollectionSharded;
    }
    return isCollPtrEligibleForJoinOpt(coll.getCollectionPtr());
}

boost::optional<JoinFallbackReason> isCollectionOrViewEligibleForJoinOpt(
    const CollectionOrViewAcquisition& coll) {
    // TODO SERVER-112239: permit foreign collection views/ resolve them.
    // Note: timeseries views should automatically be excluded if they are resolved.
    if (!coll.isCollection()) {
        return JoinFallbackReason::kCollectionIsView;
    }
    return isCollectionEligibleForJoinOpt(coll.getCollection());
}

bool isJoinOrderingEnabled(const ExpressionContext& ctx) {
    // The join optimizer unconditionally uses CBR, if the feature flag is disabled
    // this also disables join ordering.
    // TODO: SERVER-129697 Remove this check when the feature flag is removed.
    if (!feature_flags::gFeatureFlagCostBasedRanker.checkEnabled()) {
        return false;
    }

    const auto& queryKnob = ctx.getQueryKnobConfiguration();
    if (!queryKnob.isJoinOrderingEnabled()) {
        return false;
    }
    if (queryKnob.isForceClassicEngineEnabled()) {
        return false;
    }
    return true;
}

bool isAggEligibleForJoinReordering(const MultipleCollectionAccessor& mca,
                                    const Pipeline& pipeline,
                                    const boost::optional<BSONObj>& queryHint) {
    if (!AggJoinModel::pipelineEligibleForJoinReordering(pipeline)) {
        // Return false- don't want to record any kind of metric for this.
        return false;
    }

    if (queryHint.has_value() && !queryHint->isEmpty()) {
        joinOptMetrics.fallbackReasons.increment(JoinFallbackReason::kUserHintPresent);
        return false;
    }

    if (!mca.hasMainCollection()) {
        // We can't determine if the base collection is sharded.
        joinOptMetrics.fallbackReasons.increment(JoinFallbackReason::kNoMainCollection);
        return false;
    }

    // Ensure that the base collection is eligible. If not, this aggregation can't participate in
    // join optimization.
    if (auto reason = isCollectionEligibleForJoinOpt(mca.getMainCollectionAcquisition())) {
        joinOptMetrics.fallbackReasons.increment(*reason);
        return false;
    }

    // Check that all foreign collections are eligible.
    // TODO SERVER-125401: instead of falling back, shorten the prefix.
    for (const auto& [_, collAcq] : mca.getSecondaryCollectionAcquisitions()) {
        if (auto reason = isCollectionOrViewEligibleForJoinOpt(collAcq)) {
            joinOptMetrics.fallbackReasons.increment(*reason);
            return false;
        }
    }

    // Fallback on cross-DB lookups.
    auto& mainDb = mca.getMainCollection()->ns().dbName();
    bool foundCrossDbLookup = false;
    mca.forEach([&mainDb, &foundCrossDbLookup](const CollectionPtr& collPtr) {
        if (collPtr->ns().dbName() != mainDb) {
            foundCrossDbLookup = true;
        }
    });
    if (foundCrossDbLookup) {
        joinOptMetrics.fallbackReasons.increment(JoinFallbackReason::kCrossDbLookup);
        return false;
    }

    // Yay! Eligible.
    return true;
}

bool indexIsValidForINLJ(const std::shared_ptr<const IndexCatalogEntry>& ice) {
    auto desc = ice->descriptor();
    return desc->getIndexType() == IndexType::INDEX_BTREE && !desc->hidden() &&
        !desc->isPartial() && !desc->behavesAsSparse() && desc->collation().isEmpty();
}

/**
 * Pre-process indexes to filter out those ineligible for conversion to INLJ, and output a map of
 * collection namespaces to indexes available.
 */
AvailableIndexes extractINLJEligibleIndexes(const JoinGraph& graph,
                                            const MultipleCollectionAccessor& mca) {
    AvailableIndexes perCollIdxs;
    for (size_t n = 0; n < graph.numNodes(); ++n) {
        const NamespaceString& ns = graph.getNode(n).collectionName;
        if (perCollIdxs.contains(ns)) {
            continue;
        }
        const auto& indexCatalog = *mca.lookupCollection(ns)->getIndexCatalog();
        std::vector<std::shared_ptr<const IndexCatalogEntry>> entries;
        for (auto&& ice : indexCatalog.getEntriesShared(IndexCatalog::InclusionPolicy::kReady)) {
            if (indexIsValidForINLJ(ice)) {
                entries.emplace_back(ice);
            }
        }
        perCollIdxs.emplace(ns, std::move(entries));
    }
    return perCollIdxs;
}

CatalogStats createCatalogStats(OperationContext* opCtx, const MultipleCollectionAccessor& mca) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    stdx::unordered_map<NamespaceString, CollectionStats> collStats;
    mca.forEach([&collStats, &ru](const CollectionPtr& coll) {
        auto* recordStore = coll->getRecordStore();
        const boost::optional<double> approxNumLeafPages{recordStore->approxNumLeafPages(ru)};
        // TODO SERVER-117620: set .pageSizeBytes.
        collStats.emplace(coll->ns(),
                          CollectionStats{static_cast<double>(recordStore->approximateDataSize()),
                                          static_cast<double>(recordStore->storageSize(ru) -
                                                              recordStore->freeStorageSize(ru)),
                                          kDefaultPageSizeBytes,
                                          approxNumLeafPages});
    });
    auto engine = opCtx->getServiceContext()->getStorageEngine();
    double cacheSizeBytes = engine->getCacheSizeMB() * 1024 * 1024;
    return {
        .collStats = std::move(collStats),
        .bytesInStorageEngineCache = cacheSizeBytes,
    };
}

// Initialize unique field information for all namespaces in the join graph.
PerCollUniqueFieldInfo buildUniqueFieldInfo(const AvailableIndexes& perCollIdxs) {
    PerCollUniqueFieldInfo uniqueFieldInfoMap;
    for (const auto& nssAndIndexes : perCollIdxs) {
        const auto& nss = nssAndIndexes.first;

        // Build the per-collection unique field information iteratively, tracking the unique,
        // indexed fields seen so far ('ftb') and the field combinations known to be unique ('ufs').
        FieldToBit ftb;
        UniqueFieldSets ufs;
        for (const auto& index : nssAndIndexes.second) {
            if (!index->descriptor()->unique()) {
                continue;
            }

            if (auto indexFields =
                    buildUniqueFieldSetForIndex(index->descriptor()->keyPattern(), ftb)) {
                ufs.insert(*indexFields);
            }
        }
        uniqueFieldInfoMap.emplace(
            nss,
            UniqueFieldInformation{.fieldToBit = std::move(ftb), .uniqueFieldSet = std::move(ufs)});
    }
    return uniqueFieldInfoMap;
}

std::pair<std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>,
          std::unique_ptr<PlanYieldPolicySBE>>
lowerToSbePlanStageTree(OperationContext* opCtx,
                        const JoinGraph& graph,
                        PlanYieldPolicy::YieldPolicy yieldPolicy,
                        const MultipleCollectionAccessor& mca,
                        NodeId baseNode,
                        const QuerySolution& soln,
                        const cost_based_ranker::EstimateMap* estimates,
                        bool prepare,
                        OpDebug::JoinOptimizationMetrics& metrics) {
    Timer sbeLoweringTimer;
    ON_BLOCK_EXIT([&]() { metrics.sbeLoweringTimeMicros = sbeLoweringTimer.micros(); });
    sleepWhileLoweringJoinPlanToSbe.execute(
        [](const BSONObj& data) { sleepmillis(data["ms"].numberInt()); });

    auto& baseCQ = *graph.accessPathAt(baseNode);
    auto baseNss = baseCQ.nss();
    auto sbeYieldPolicy = PlanYieldPolicySBE::make(opCtx, yieldPolicy, mca, baseNss);
    auto planStagesAndData = stage_builder::buildSlotBasedExecutableTree(
        opCtx, mca, baseCQ, soln, sbeYieldPolicy.get(), estimates);
    // Skipped for explain's rejected plans and for plans that runJoinTrial() prepares instead.
    if (prepare) {
        stage_builder::prepareSlotBasedExecutableTree(opCtx,
                                                      planStagesAndData.first.get(),
                                                      &planStagesAndData.second,
                                                      baseCQ,
                                                      mca,
                                                      sbeYieldPolicy.get(),
                                                      /*preparingFromSbeCache=*/false,
                                                      /*remoteCursors=*/nullptr);
    }
    return std::make_pair(std::move(planStagesAndData), std::move(sbeYieldPolicy));
}

/**
 * Records how much of the non-join-reorderable pipeline suffix was lowered into SBE versus how much
 * remains to be executed as DocumentSources.
 */
void recordSuffixLoweringMetrics(const Pipeline* suffix,
                                 size_t numPushedToSbe,
                                 OpDebug::JoinOptimizationMetrics& metrics) {
    metrics.numSuffixSourcesPushedToSbe = numPushedToSbe;
    metrics.numResidualClassicSources = suffix ? suffix->getSources().size() : 0;
}

/**
 * Identifies the portion of the pipeline not pushed down into the join optimizer, but eligible to
 * pushdown into SBE. Pushes that portion into SBE, and removes those stages from the pipeline so
 * they are not executed twice. Also records the resulting lowering metrics.
 *
 * Shared by the join plan cache hit path and the full optimization path so that recovering a plan
 * from the cache produces the same executor as the query that populated the cache.
 */
void pushDownSbeEligibleSuffix(OperationContext* opCtx,
                               const MultipleCollectionAccessor& mca,
                               const AggJoinModel& model,
                               NodeId baseNode,
                               std::unique_ptr<QuerySolution>& soln,
                               OpDebug::JoinOptimizationMetrics& metrics) {
    auto* suffix = model.getSuffix();
    size_t numPushedToSbe = 0;
    if (suffix && suffix->peekFront()) {
        auto& prefix = *model.getGraph().getNode(baseNode).accessPath;
        // This helper identifies the stages in the 'suffix' pipeline that are eligible for running
        // in SBE and prepend them to prefix.cqPipeline.

        // When we call extendWithAggPipeline() below, it will mutate the existing join reordered
        // query solution such that the stages in prefix.cqPipeline are placed above the join
        // optimized stages. Any stages not eligible for join reordering or SBE pushdown remain
        // in the `suffix` pipeline.
        attachPipelineStages(
            mca,
            suffix,
            /*needsMerge=*/false,
            &prefix,
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForPushDownStagesDecision{
                .opCtx = opCtx,
                .canonicalQuery = prefix,
                .collections = mca,
                .plannerOptions = QueryPlannerParams::DEFAULT,
            }));

        // 'attachPipelineStages()' prepends the SBE-eligible suffix stages onto
        // 'prefix.cqPipeline()', so its size is exactly the number of stages we lowered.
        numPushedToSbe = prefix.cqPipeline().size();

        if (!prefix.cqPipeline().empty()) {
            QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForSingleCollectionQuery{
                .opCtx = opCtx,
                .canonicalQuery = prefix,
                .collections = mca,
                .plannerOptions = QueryPlannerParams::DEFAULT,
            });

            plannerParams.fillOutSecondaryCollectionsPlannerParams(opCtx, prefix, mca);
            plannerParams.setTargetSbeStageBuilder(prefix, mca);
            // Create the query solution
            soln = QueryPlanner::extendWithAggPipeline(prefix,
                                                       std::move(soln),
                                                       plannerParams.secondaryCollectionsInfo,
                                                       /*skipProjectGroupOptimization=*/false);
        }
        // Remove any of the stages pushed down via attachPipelineStages, from the pipeline 'suffix'
        // so they are not executed twice (once in SBE and again in classic). Only the remaining
        // stages in suffix will be executed in classic.
        finalizePipelineStages(suffix, &prefix);
    }
    recordSuffixLoweringMetrics(suffix, numPushedToSbe, metrics);
}

/**
 * Builds an accessor with only the collections the join graph references. The full 'mca' also
 * carries suffix $lookup namespaces, which are not in the join graph; excluding them keeps an
 * entry's collection tags aligned with its join tree.
 */
MultipleCollectionAccessor makeGraphOnlyCollectionAccessor(const MultipleCollectionAccessor& mca,
                                                           const JoinGraph& graph) {
    auto mainAcq =
        CollectionOrViewAcquisition(CollectionAcquisition(mca.getMainCollectionAcquisition()));
    CollectionOrViewAcquisitionMap secondaries;
    for (size_t i = 0; i < graph.numNodes(); ++i) {
        const auto nss = graph.getNode(static_cast<NodeId>(i)).collectionName;
        if (nss == mca.getMainCollection()->ns()) {
            continue;
        }
        if (auto it = mca.getSecondaryCollectionAcquisitions().find(nss);
            it != mca.getSecondaryCollectionAcquisitions().end()) {
            secondaries.emplace(nss, it->second);
        }
    }
    return MultipleCollectionAccessor(std::move(mainAcq), std::move(secondaries), false);
}

struct LoweredJoinPlan {
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> planStagesAndData;
    std::unique_ptr<PlanYieldPolicySBE> sbeYieldPolicy;
    size_t plannerOptions;
};

/**
 * Lowers a winning join plan rooted at 'baseNode' to SBE, shared by the cache hit and miss paths.
 */
LoweredJoinPlan lowerWinningJoinPlan(OperationContext* opCtx,
                                     const AggJoinModel& model,
                                     const MultipleCollectionAccessor& mca,
                                     PlanYieldPolicy::YieldPolicy yieldPolicy,
                                     NodeId baseNode,
                                     const QuerySolution& soln,
                                     const cost_based_ranker::EstimateMap* estimates,
                                     bool prepare,
                                     OpDebug::JoinOptimizationMetrics& metrics) {
    // Merge join-field non-array path learnings into the chosen base node's expCtx so the
    // PathArraynessChecker monitors them during execution yields.
    auto* baseExpCtx = model.getGraph().accessPathAt(baseNode)->getExpCtx().get();
    baseExpCtx->mergeNonArrayPathsForNss(model.getJoinExpCtx()->getNonArrayPathsForNss());
    baseExpCtx->mergeSchemaTypeInfoUsageFrom(*model.getJoinExpCtx());

    auto [planStagesAndData, sbeYieldPolicy] = lowerToSbePlanStageTree(
        opCtx, model.getGraph(), yieldPolicy, mca, baseNode, soln, estimates, prepare, metrics);

    // If there is a pipeline suffix, then that suffix will execute inside a
    // PlanExecutorPipeline, which expects to received owned BSON objects from the inner
    // PlanExecutor.
    size_t plannerOptions = QueryPlannerParams::DEFAULT;
    if (model.getSuffix() && model.getSuffix()->peekFront()) {
        plannerOptions |= QueryPlannerParams::RETURN_OWNED_DATA;
    }
    return LoweredJoinPlan{.planStagesAndData = std::move(planStagesAndData),
                           .sbeYieldPolicy = std::move(sbeYieldPolicy),
                           .plannerOptions = plannerOptions};
}

/**
 * Builds an executor for a join plan cache hit whose entry was validated against the catalog.
 */
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> buildExecutorFromCachedPlan(
    OperationContext* opCtx,
    const JoinPlanCacheEntry& hit,
    const MultipleCollectionAccessor& mca,
    const MultipleCollectionAccessor& graphOnlyMca,
    const AggJoinModel& model,
    const AvailableIndexes& perCollIdxs,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    OpDebug::JoinOptimizationMetrics& metrics) {
    LOGV2_DEBUG(11083906, 5, "Join plan cache hit, skipping join optimization");
    auto qsn =
        fromCachedJoinPlan(opCtx, model.getGraph(), graphOnlyMca, perCollIdxs, *hit.joinTree);
    auto winnerSoln = std::make_unique<QuerySolution>();
    winnerSoln->setRoot(std::move(qsn));

    // Use 'mca' instead of 'graphOnlyMca' for SBE lowering because non-join eligible stages may
    // reference other collections.
    pushDownSbeEligibleSuffix(opCtx, mca, model, hit.baseNode, winnerSoln, metrics);

    auto [planStagesAndData, sbeYieldPolicy, plannerOptions] =
        lowerWinningJoinPlan(opCtx,
                             model,
                             mca,
                             yieldPolicy,
                             hit.baseNode,
                             *winnerSoln,
                             /*estimates=*/nullptr,
                             /*prepare=*/true,
                             metrics);
    auto exec = plan_executor_factory::make(opCtx,
                                            /*cq=*/nullptr,
                                            std::move(winnerSoln),
                                            std::move(planStagesAndData),
                                            mca,
                                            plannerOptions,
                                            mca.getMainCollection()->ns(),
                                            std::move(sbeYieldPolicy),
                                            /*isFromPlanCache=*/true,
                                            /*cachedPlanHash=*/false,
                                            /*usedJoinOpt=*/true,
                                            /*rejectedJoinPlans=*/{},
                                            /*remoteCursors=*/nullptr,
                                            /*remoteExplains=*/nullptr,
                                            /*classicRuntimePlannerStage=*/nullptr,
                                            /*maybeExplainData=*/boost::none,
                                            PlanSelectionStrategy::kJoinCachedPlan);
    return exec;
}

}  // namespace

/**
 * Attempts to apply join optimization to the given aggregation, but if it fails to extract a join
 * model, falls back to preparing executors for the pipeline in the normal way.
 */
StatusWith<JoinReorderedExecutorResult> getJoinReorderedExecutor(
    const MultipleCollectionAccessor& mca,
    const Pipeline& pipeline,
    const boost::optional<BSONObj>& queryHint,
    OperationContext* opCtx,
    const boost::intrusive_ptr<ExpressionContext> expCtx) {
    // Don't proceed if join optimization is not enabled.
    if (!isJoinOrderingEnabled(*expCtx)) {
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Pipeline or collection ineligible for join-reordering");
    }

    // Quick eligibility check.
    if (!isAggEligibleForJoinReordering(mca, pipeline, queryHint)) {
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Pipeline or collection ineligible for join-reordering");
    }

    // Initialize metrics after we determine that the query shape vaguely looks join-optimizable, as
    // otherwise we would have these metrics for every aggregation. Note that on retry, these
    // metrics will be (intentionally!) reset.
    auto& od = CurOp::get(opCtx)->debug();
    od.joinOptimizationMetrics.emplace();
    auto& metrics = *od.joinOptimizationMetrics;

    // The sampling estimators are created only after a join plan cache miss further down; the
    // recording guard needs them in scope to tally the persisted NDV statistics they served.
    SamplingEstimatorMap samplingEstimators;

    ON_BLOCK_EXIT([&] {
        // Record serverStatus metrics on every exit path, including the ones that throw.
        if (auto& pe = metrics.planEnumerationMetrics) {
            for (const auto& [nss, samplingEstimator] : samplingEstimators) {
                pe->numPersistentNDVStatsUsed +=
                    static_cast<int>(samplingEstimator->getNumPersistedNDVStatsUsed());
            }
        }
        recordJoinOptimizationMetrics(metrics);
    });

    // Try to build JoinGraph.
    const auto& config = pipeline.getContext()->getQueryKnobConfiguration();
    AggModelBuildParams buildParams{
        .joinGraphBuildParams =
            JoinGraphBuildParams(config.getMaxNodesInJoinGraph(), config.getMaxEdgesInJoinGraph()),
        .maxNumberNodesConsideredForImplicitEdges =
            static_cast<size_t>(config.getMaxNumberNodesConsideredForImplicitEdges())};
    auto swModel = AggJoinModel::constructJoinModel(pipeline, buildParams, metrics);
    if (!swModel.isOK()) {
        // We failed to apply join-reordering, so we take the regular path.
        const auto status = swModel.getStatus();
        LOGV2_DEBUG(11083903, 5, "Unable to construct join model", "status"_attr = status);
        return status;
    }

    // Validate we have all the collection acquisitions we need here.
    bool missingAcquisitions = std::any_of(swModel.getValue().getPrefix()->getSources().begin(),
                                           swModel.getValue().getPrefix()->getSources().end(),
                                           [&](const auto& stage) {
                                               auto* lookup =
                                                   dynamic_cast<DocumentSourceLookUp*>(stage.get());
                                               if (!lookup) {
                                                   return false;
                                               }
                                               return !mca.knowsNamespace(lookup->getFromNs());
                                           });
    if (missingAcquisitions) {
        metrics.fallbackReason = JoinFallbackReason::kMissingForeignAcquisition;
        return Status(
            ErrorCodes::QueryFeatureNotAllowed,
            "Pipeline ineligible for join-reordering due to missing foreign namespace acquisition");
    }

    LOGV2_DEBUG(11083902,
                5,
                "Join model was successfully constructed, reordering joins",
                "graph"_attr = swModel.getValue().toBSON());
    auto model = std::move(swModel.getValue());

    // Snapshot only the join-graph collections so the entry's tags match its join tree, and the
    // cache invalidates only on DDL affecting the join graph.
    const auto graphOnlyMca = makeGraphOnlyCollectionAccessor(mca, model.getGraph());

    auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    // Retrieve a copy of the hint if present.
    boost::optional<EnumerationStrategy> hintedStrat;
    if (auto hintStage =
            dynamic_cast<DocumentSourceInternalJoinHint*>(model.getPrefix()->peekFront());
        hintStage) {
        hintedStrat = hintStage->getStrategy();
    }

    const auto qkc = expCtx->getQueryKnobConfiguration();

    const auto eligibleIdxs = extractINLJEligibleIndexes(model.getGraph(), graphOnlyMca);

    // Consult the join plan cache before running single-table access planning. Set iff the cache
    // is in use and the lookup missed.
    boost::optional<JoinPlanCacheMiss> cacheMiss;
    if (canUseJoinPlanCache(qkc, hintedStrat.has_value(), expCtx->getExplain().has_value())) {
        auto lookup = lookupInJoinPlanCache(opCtx, graphOnlyMca, model, eligibleIdxs);
        if (auto* hit = std::get_if<JoinPlanCacheHit>(&lookup)) {
            auto exec = buildExecutorFromCachedPlan(
                opCtx, *hit->entry, mca, graphOnlyMca, model, eligibleIdxs, yieldPolicy, metrics);
            recordJoinPlanCacheHit();
            return JoinReorderedExecutorResult{.executor = std::move(exec),
                                               .model = std::move(model)};
        }
        cacheMiss = std::get<JoinPlanCacheMiss>(std::move(lookup));
    }

    // Initialize enumeration metrics if we're here- we don't want to do this on cache hits/
    // whenever we don't plan.
    metrics.planEnumerationMetrics.emplace();
    auto& peMetrics = *metrics.planEnumerationMetrics;

    // Acquire the samples that CE (both here and in CBR below) will consult.
    samplingEstimators = makeSamplingEstimators(
        graphOnlyMca, model.getGraph(), yieldPolicy, model.getJoinExpCtx(), peMetrics);

    // Select access plans for each table in the join.
    auto swAccessPlans = singleTableAccessPlans(
        opCtx, graphOnlyMca, model.getGraph(), samplingEstimators, peMetrics);
    if (!swAccessPlans.isOK()) {
        metrics.fallbackReason = JoinFallbackReason::kFailedToGetSingleTableAccessViaCBR;
        return swAccessPlans.getStatus();
    }
    auto singleTableAccess = std::move(swAccessPlans.getValue());

    const bool cacheWinningPlan = cacheMiss && canCacheAccessPlans(singleTableAccess);
    PerCollUniqueFieldInfo uniqueFieldInfo;
    if (qkc.getEnableJoinOptimizationUseIndexUniqueness()) {
        uniqueFieldInfo = buildUniqueFieldInfo(eligibleIdxs);
    }

    JoinReorderingContext ctx{.joinGraph = model.getGraph(),
                              .resolvedPaths = model.getResolvedPaths(),
                              .singleTableAccess = std::move(singleTableAccess),
                              .perCollIdxs = eligibleIdxs,
                              .catStats = createCatalogStats(opCtx, graphOnlyMca),
                              .uniqueFieldInfo = std::move(uniqueFieldInfo),
                              .samplingEstimators = &samplingEstimators,
                              .explainMode = getExplainMode(*expCtx, qkc)};

    // Collect the distinct join-graph namespaces whose storage-engine approximate leaf page count
    // is unavailable, forcing cost estimation onto the size-based fallback. 'collStats' covers
    // every collection in 'mca', a superset of the join-graph namespaces; collections referenced
    // only by the unoptimized suffix are never costed, so they are not counted.
    std::vector<NamespaceString> collectionsWithoutPageCount;
    for (const auto& nss : model.getDistinctNamespaces()) {
        if (!ctx.catStats.collStats.at(nss).hasApproxNumLeafPages()) {
            collectionsWithoutPageCount.push_back(nss);
        }
    }
    peMetrics.numApproxLeafPagesUnavailable = static_cast<int>(collectionsWithoutPageCount.size());

    JoinCardinalityEstimator cardEstimator(
        JoinCardinalityEstimator::make(ctx, samplingEstimators, peMetrics));
    JoinCostEstimatorImpl costEstimator(ctx, cardEstimator);

    // Inject delay for testing purposes (allows tests to verify optimizationTimeMillis is
    // measured).
    if (MONGO_unlikely(sleepWhileJoinOptimizing.shouldFail())) {
        sleepWhileJoinOptimizing.execute(
            [](const BSONObj& data) { sleepmillis(data["ms"].numberInt()); });
    }

    StatusWith<ReorderedJoinSolution> swReordered = [&]() {
        // Time required to generate a QSN by any method- even if we fail to generate a QSN.
        Timer planEnumerationTimer;
        ON_BLOCK_EXIT([&] {
            peMetrics.planEnumerationTimeMicros = planEnumerationTimer.micros();
            peMetrics.ceTimeMicros = cardEstimator.getEstimationTimeMicros();
        });
        sleepWhileEnumeratingJoinPlans.execute(
            [](const BSONObj& data) { sleepmillis(data["ms"].numberInt()); });

        if (hintedStrat || qkc.getJoinReorderMode() == JoinReorderModeEnum::kBottomUp) {
            uassert(12016318,
                    "Cannot have hinted & random mode",
                    !hintedStrat || qkc.getJoinReorderMode() != JoinReorderModeEnum::kRandom);
            // Optimize join order using bottom-up Sellinger-style algorithm.
            // Note: a hint will avoid invoking random reordering.
            return constructSolutionBottomUp(ctx,
                                             cardEstimator,
                                             costEstimator,
                                             hintedStrat ? std::move(*hintedStrat)
                                                         : getEnumerationStrategy(qkc),
                                             /*populateCachedPlan=*/cacheWinningPlan,
                                             *metrics.planEnumerationMetrics);
        }

        tassert(12016315,
                "Expected random mode",
                qkc.getJoinReorderMode() == JoinReorderModeEnum::kRandom);
        // Randomly reorder joins (while still passing through bottom-up enumerator). NOTE:
        // this currently ignores all query knobs other than the random seed & plan tree
        // shape, but could easily be modified to take the values of other query knobs as
        // "overrides".
        return constructSolutionWithRandomOrder(ctx,
                                                &cardEstimator,
                                                &costEstimator,
                                                qkc.getRandomJoinOrderSeed(),
                                                getPlanTreeShape(qkc.getJoinPlanTreeShape()),
                                                getJoinMethod(qkc.getJoinMethod()),
                                                /*enableHJOrderPruning=*/false,
                                                /*maxRandomHintRetries=*/3,
                                                *metrics.planEnumerationMetrics);
    }();
    uassertStatusOK(swReordered.getStatus());
    auto reordered = std::move(swReordered.getValue());

    // Identify suffix stages that are eligible for SBE pushdown & consequently lower them to the
    // SBE executor with the join-reordered prefix.
    // Note: this uses the original MCA with all collections, not just the join-graph collections in
    // 'graphOnlyMca' because the SBE executor needs access to all collections pushed down to SBE.
    pushDownSbeEligibleSuffix(opCtx, mca, model, reordered.baseNode, reordered.soln, metrics);

    // Test hook: all sampling and join reordering is complete at this point.
    hangAfterJoinModelConstruction.pauseWhileSet(opCtx);

    // A cacheable plan runs a trial to establish its baseline before the executor is built.
    const bool runBaselineTrial = cacheWinningPlan && reordered.cachedJoinPlan;

    // Lower to SBE. A plan that will run a trial is prepared by the trial instead.
    auto [planStagesAndData, sbeYieldPolicy, plannerOptions] =
        lowerWinningJoinPlan(opCtx,
                             model,
                             mca,
                             yieldPolicy,
                             reordered.baseNode,
                             *reordered.soln,
                             &reordered.estimates,
                             /*prepare=*/!runBaselineTrial,
                             metrics);
    auto& baseNodeCQ = *model.getGraph().accessPathAt(reordered.baseNode);

    sbe::DebugPrintInfo debugPrintInfo{};
    LOGV2_DEBUG(11083905,
                5,
                "SBE plan for join-reordered query",
                "sbePlan"_attr =
                    sbe::DebugPrinter{}.print(planStagesAndData.first->debugPrint(debugPrintInfo)),
                "sbePlanStageData"_attr = planStagesAndData.second.debugString());

    // Prepare rejected plans if any.
    std::vector<JoinOptPlan> rejectedPlans;
    if (ctx.isExplain()) {
        rejectedPlans.reserve(reordered.rejectedSolns.size());
        for (auto&& rs : reordered.rejectedSolns) {
            auto soln = std::move(rs.first);
            auto baseNode = rs.second;
            // We don't count time spent on rejected plan lowering for explain.
            OpDebug::JoinOptimizationMetrics rejectedMetrics;
            auto [stagesAndData, _] = lowerToSbePlanStageTree(opCtx,
                                                              model.getGraph(),
                                                              yieldPolicy,
                                                              mca,
                                                              baseNode,
                                                              *soln,
                                                              &reordered.estimates,
                                                              /*prepare=*/false,
                                                              rejectedMetrics);
            rejectedPlans.push_back(JoinOptPlan{.soln = std::move(soln),
                                                .stage = std::move(stagesAndData.first),
                                                .data = std::move(stagesAndData.second)});
        }
    }

    // Collect per-namespace sampling metadata for explain output.
    boost::optional<PlanExplainerData> maybeExplainData;
    if (ctx.isExplain()) {
        PlanExplainerData explainData;
        for (const auto& [nss, estimator] : samplingEstimators) {
            const std::string serializedNss =
                NamespaceStringUtil::serialize(nss, expCtx->getSerializationContext());
            explainData.ceSamplingMetadata.emplace(serializedNss, estimator->getSamplingMetadata());
            if (auto ndvMetadata = estimator->getPersistedNDVMetadata(); !ndvMetadata.empty()) {
                explainData.fieldStatsMetadata.emplace(serializedNss, std::move(ndvMetadata));
            }
        }
        // Compute the join plan cache key hash for explain regardless of whether the join plan
        // cache is enabled.
        explainData.joinPlanCacheKeyHash = canonical_query_encoder::computeHash(
            cacheMiss
                ? cacheMiss->key
                : makeJoinPlanCacheKey(model.getGraph(), model.getResolvedPaths(), graphOnlyMca));
        // Sort for a deterministic explain output.
        std::ranges::sort(collectionsWithoutPageCount);
        explainData.joinCollectionsWithoutPageCount = std::move(collectionsWithoutPageCount);
        // The join-reordering estimates cover the winning and the rejected plans, and are only
        // serialized by explain. The rejected plans were lowered above, so the map can be moved.
        explainData.estimates = std::move(reordered.estimates);
        maybeExplainData = std::move(explainData);
    }

    if (!runBaselineTrial) {
        // TODO SERVER-111913: Once we are no-longer cloning QSN for single-table plans, the
        // estimate map from CBR 'ctx.singleTableAccess.estimate' can be combined into the explain
        // data's estimates alongside the join-reordering estimates. We actually have several
        // canonical queries, so we don't try to pass one in.
        auto exec = plan_executor_factory::make(opCtx,
                                                /*cq=*/nullptr,
                                                std::move(reordered.soln),
                                                std::move(planStagesAndData),
                                                mca,
                                                plannerOptions,
                                                mca.getMainCollection()->ns(),
                                                std::move(sbeYieldPolicy),
                                                /*isFromPlanCache=*/false,
                                                /*cachedPlanHash=*/false,
                                                /*usedJoinOpt=*/true,
                                                std::move(rejectedPlans),
                                                /*remoteCursors=*/nullptr,
                                                /*remoteExplains=*/nullptr,
                                                /*classicRuntimePlannerStage=*/nullptr,
                                                std::move(maybeExplainData),
                                                PlanSelectionStrategy::kJoinOptimization);
        return JoinReorderedExecutorResult{.executor = std::move(exec), .model = std::move(model)};
    }

    tassert(13036804,
            "Join plan cache key and collection tags must be set when the join plan cache is "
            "in use",
            cacheMiss.has_value());
    // Explain disables the join plan cache, so a trialed plan never carries explain-only data.
    tassert(13673702,
            "A join plan that runs a baseline trial must not carry explain data",
            rejectedPlans.empty() && !maybeExplainData);

    // Establish the winning plan's execution-statistics baseline by running it up to the trial
    // window with an uncapped tracker attached. Cache insertion is deferred until baseline metrics
    // are established at the end of the trial period.
    // TODO(SERVER-136925): consider taking min(limit, joinPlanCacheTrialWindow), as the classic
    // multi-planner does.
    const size_t maxTrialResults = static_cast<size_t>(qkc.getJoinPlanCacheTrialWindow());
    auto [candidate, baseline] = runJoinTrial(opCtx,
                                              mca,
                                              baseNodeCQ,
                                              sbeYieldPolicy.get(),
                                              std::move(reordered.soln),
                                              std::move(planStagesAndData),
                                              makeBaselineTracker(),
                                              maxTrialResults);

    tryInsertIntoJoinPlanCache(
        opCtx,
        std::move(*cacheMiss),
        graphOnlyMca,
        model,
        eligibleIdxs,
        pipeline,
        std::move(reordered.cachedJoinPlan),
        reordered.baseNode,
        baseline,
        candidate.stopCondition == MultiPlannerStopCondition::kStashSizeLimit,
        static_cast<size_t>(qkc.getJoinPlanCacheMaxReadsAndIntermediateRows()));

    // The executor serves the results buffered during the trial before pulling more from the plan.
    auto exec = plan_executor_factory::make(opCtx,
                                            /*cq=*/nullptr,
                                            std::move(candidate),
                                            mca,
                                            plannerOptions,
                                            mca.getMainCollection()->ns(),
                                            std::move(sbeYieldPolicy),
                                            /*remoteCursors=*/nullptr,
                                            /*remoteExplains=*/nullptr,
                                            /*cachedPlanHash=*/boost::none,
                                            PlanSelectionStrategy::kJoinOptimization);
    return JoinReorderedExecutorResult{.executor = std::move(exec), .model = std::move(model)};
}
}  // namespace mongo::join_ordering
