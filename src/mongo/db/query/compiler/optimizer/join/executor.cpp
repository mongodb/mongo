// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/executor.h"

#include "mongo/base/status_with.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
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
#include "mongo/db/query/compiler/optimizer/join/hint.h"
#include "mongo/db/query/compiler/optimizer/join/index_fingerprint.h"
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/reorder_joins.h"
#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"
#include "mongo/db/query/plan_cache/join_plan_cache.h"
#include "mongo/db/query/plan_cache/join_plan_cache_key.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer_sbe.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/db/query/stage_builder/stage_builder_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <algorithm>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::join_ordering {

MONGO_FAIL_POINT_DEFINE(sleepWhileJoinOptimizing);
MONGO_FAIL_POINT_DEFINE(hangAfterJoinModelConstruction);

// These sleep for {ms: <millis>} inside the phase they name, so that tests can verify that
// 'sbeLoweringTimeMicros' and 'planEnumerationTimeMicros' measure those phases.
MONGO_FAIL_POINT_DEFINE(sleepWhileLoweringJoinPlanToSbe);
MONGO_FAIL_POINT_DEFINE(sleepWhileEnumeratingJoinPlans);

namespace {
/**
 * Number of times a usable plan was found in the join plan cache. Exposed in serverStatus as
 * 'metrics.query.planCache.join.hits'.
 */
auto& joinPlanCacheHits = *MetricBuilder<Counter64>{"query.planCache.join.hits"};

/**
 * Number of times no usable plan was found in the join plan cache (either absent or stale).
 * Exposed in serverStatus as 'metrics.query.planCache.join.misses'.
 */
auto& joinPlanCacheMisses = *MetricBuilder<Counter64>{"query.planCache.join.misses"};

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

bool isCollPtrEligibleForJoinOpt(const CollectionPtr& cptr) {
    return !cptr->isCapped() && !cptr->isClustered() &&
        CollatorInterface::isSimpleCollator(cptr->getDefaultCollator());
}

bool isCollectionEligibleForJoinOpt(const CollectionAcquisition& coll) {
    return coll.exists() && !coll.getShardingDescription().isSharded() &&
        isCollPtrEligibleForJoinOpt(coll.getCollectionPtr());
}

bool isCollectionOrViewEligibleForJoinOpt(const CollectionOrViewAcquisition& coll) {
    // TODO SERVER-112239: permit foreign collection views/ resolve them.
    // Note: timeseries views should automatically be excluded if they are resolved.
    return coll.isCollection() && isCollectionEligibleForJoinOpt(coll.getCollection());
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
        // TODO SERVER-132003: Record reason as JoinFallbackReason::kUserHintPresent.
        return false;
    }

    if (!mca.hasMainCollection()) {
        // We can't determine if the base collection is sharded.
        // TODO SERVER-132003: Record reason as JoinFallbackReason::kNoMainCollection.
        return false;
    }

    // Ensure that the base collection is eligible. If not, this aggregation can't participate in
    // join optimization.
    // TODO SERVER-132003: Record reason.
    if (!isCollectionEligibleForJoinOpt(mca.getMainCollectionAcquisition())) {
        return false;
    }

    // Check that all foreign collections are eligible.
    // TODO SERVER-125401: instead of falling back, shorten the prefix.
    for (const auto& [_, collAcq] : mca.getSecondaryCollectionAcquisitions()) {
        if (!isCollectionOrViewEligibleForJoinOpt(collAcq)) {
            // TODO SERVER-132003: Record reason.
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
        // TODO SERVER-132003: Record reason as JoinFallbackReason::kCrossDbLookup.
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
AvailableIndexes extractINLJEligibleIndexes(const QuerySolutionMap& cbrCqQsns,
                                            const MultipleCollectionAccessor& mca) {
    AvailableIndexes perCollIdxs;
    for (const auto& [cq, _] : cbrCqQsns) {
        const auto& ns = cq->nss();
        if (perCollIdxs.contains(ns)) {
            // We've already pre-processed this collection's indexes.
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

AvailableIndexes extractINLJEligibleIndexesFromGraph(const JoinGraph& graph,
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
                          CollectionStats{static_cast<double>(recordStore->dataSize()),
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
    if (prepare) {
        // We don't need to prepare plans if we're not planning to execute them.
        stage_builder::prepareSlotBasedExecutableTree(opCtx,
                                                      planStagesAndData.first.get(),
                                                      &planStagesAndData.second,
                                                      baseCQ,
                                                      mca,
                                                      sbeYieldPolicy.get(),
                                                      false /*preparingFromCache*/,
                                                      nullptr /*remoteCursors*/);
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
 * Returns true if the cached entry 'hit' can still be used against the current catalog, and false
 * if it is stale and the query must be replanned.
 *
 * A bumped collection version alone is not enough to reject the entry: the DDL responsible may
 * have touched an index that is irrelevant to the cached plan. In that case we use relevant-index
 * fingerprints, which only cover indexes usable for the fields each join graph node actually
 * references.
 */
bool validateCacheEntry(JoinPlanCacheEntry& hit,
                        const MultipleCollectionAccessor& mca,
                        const AggJoinModel& model,
                        const AvailableIndexes& perCollIdxs) {
    auto cachedTags = hit.getCollectionTags();
    switch (classifyCollectionTags(cachedTags, mca)) {
        case CollectionTagStatus::kCurrent:
            // Nothing has changed since the entry was cached, so no fingerprinting is needed.
            return true;
        case CollectionTagStatus::kStale:
            // A stale tag can mean a dropped collection or a sample refresh.
            return false;
        case CollectionTagStatus::kNeedsIndexRevalidation:
            break;
    }

    tassert(13036801,
            "Cached join plan must have one index fingerprint per join graph node",
            hit.nodeFingerprints.size() == model.getGraph().numNodes());

    auto currentFingerprints =
        makeNodeFingerprints(model.getGraph(), model.getResolvedPaths(), perCollIdxs);
    if (hit.nodeFingerprints != currentFingerprints) {
        LOGV2_DEBUG(
            13036802, 5, "Join plan cache entry invalidated by a change to a relevant index");
        return false;
    }

    // The catalog change left every relevant index untouched, so this entry is valid against the
    // current collection state. Adopt that state's version tags so subsequent lookups take the
    // fast path above instead of re-fingerprinting on every query.
    auto currentTags = makeCollectionTags(mca);
    hit.refreshCollectionTags(currentTags);

    LOGV2_DEBUG(13036803,
                5,
                "Join plan cache entry revalidated: the catalog change did not affect any relevant "
                "index",
                "cachedVersions"_attr = collectionVersionsForLog(cachedTags),
                "currentVersions"_attr = collectionVersionsForLog(currentTags));
    return true;
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> checkPlanCacheForPlan(
    OperationContext* opCtx,
    const JoinPlanCacheKey& cacheKey,
    const MultipleCollectionAccessor& mca,
    const AggJoinModel& model,
    const AvailableIndexes& perCollIdxs,
    PlanYieldPolicy::YieldPolicy yieldPolicy,
    OpDebug::JoinOptimizationMetrics& metrics) {
    auto& cache = JoinPlanCache::get(opCtx->getServiceContext());
    auto hit = cache.lookup(cacheKey);
    if (!hit) {
        return nullptr;
    }

    // TODO (SERVER-129268): Evict stale entries.
    if (validateCacheEntry(*hit, mca, model, perCollIdxs)) {
        LOGV2_DEBUG(11083906, 5, "Join plan cache hit, skipping join optimization");
        auto qsn = fromCachedJoinPlan(opCtx, model.getGraph(), mca, perCollIdxs, *hit->joinTree);
        auto winnerSoln = std::make_unique<QuerySolution>();
        winnerSoln->setRoot(std::move(qsn));

        // TODO SERVER-130469: Pushdown SBE eligible suffix.
        recordSuffixLoweringMetrics(model.getSuffix(), 0 /* numPushedToSbe */, metrics);
        auto [planStagesAndData, sbeYieldPolicy] = lowerToSbePlanStageTree(opCtx,
                                                                           model.getGraph(),
                                                                           yieldPolicy,
                                                                           mca,
                                                                           hit->baseNode,
                                                                           *winnerSoln,
                                                                           nullptr /*estimates*/,
                                                                           true /*prepare*/,
                                                                           metrics);

        size_t plannerOptions = QueryPlannerParams::DEFAULT;
        if (model.getSuffix() && model.getSuffix()->peekFront()) {
            plannerOptions |= QueryPlannerParams::RETURN_OWNED_DATA;
        }
        cost_based_ranker::EstimateMap emptyEstimates;
        auto exec = plan_executor_factory::make(opCtx,
                                                nullptr /* cq */,
                                                std::move(winnerSoln),
                                                std::move(planStagesAndData),
                                                mca,
                                                plannerOptions,
                                                mca.getMainCollection()->ns(),
                                                std::move(sbeYieldPolicy),
                                                true /* isFromPlanCache */,
                                                false /* cachedPlanHash */,
                                                true /*usedJoinOpt*/,
                                                std::move(emptyEstimates),
                                                {} /* rejectedPlans */,
                                                nullptr /* remoteCursors */,
                                                nullptr /* remoteExplains */,
                                                nullptr /* classicRuntimePlannerStage */,
                                                boost::none /* maybeExplainData */);
        return exec;
    }
    return nullptr;
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

    auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    // Retrieve a copy of the hint if present.
    boost::optional<EnumerationStrategy> hintedStrat;
    if (auto hintStage =
            dynamic_cast<DocumentSourceInternalJoinHint*>(model.getPrefix()->peekFront());
        hintStage) {
        hintedStrat = hintStage->getStrategy();
    }

    const auto qkc = expCtx->getQueryKnobConfiguration();

    // Consult the join plan cache before running single-table access planning.
    const bool useJoinPlanCache = !hintedStrat &&
        qkc.getJoinReorderMode() != JoinReorderModeEnum::kRandom && qkc.getEnableJoinPlanCache() &&
        !expCtx->getExplain().has_value();

    // 'cacheKey' and 'cacheEligibleIdxs' are set iff 'useJoinPlanCache' is true.
    boost::optional<JoinPlanCacheKey> cacheKey;
    boost::optional<AvailableIndexes> cacheEligibleIdxs;
    if (useJoinPlanCache) {
        cacheKey = makeJoinPlanCacheKey(model.getGraph(), model.getResolvedPaths(), mca);
        cacheEligibleIdxs = extractINLJEligibleIndexesFromGraph(model.getGraph(), mca);
        auto exec = checkPlanCacheForPlan(
            opCtx, *cacheKey, mca, model, *cacheEligibleIdxs, yieldPolicy, metrics);
        if (exec) {
            joinPlanCacheHits.increment(1);
            return JoinReorderedExecutorResult{.executor = std::move(exec),
                                               .model = std::move(model)};
        }
        joinPlanCacheMisses.increment(1);
        LOGV2_DEBUG(11083907, 5, "Join plan cache miss, running optimization");
    }

    // Initialize enumeration metrics if we're here- we don't want to do this on cache hits/
    // whenever we don't plan.
    metrics.planEnumerationMetrics.emplace();
    auto& peMetrics = *metrics.planEnumerationMetrics;

    // Acquire the samples that CE (both here and in CBR below) will consult.
    SamplingEstimatorMap samplingEstimators = makeSamplingEstimators(
        mca, model.getGraph(), yieldPolicy, model.getJoinExpCtx(), peMetrics);

    // Select access plans for each table in the join.
    auto swAccessPlans =
        singleTableAccessPlans(opCtx, mca, model.getGraph(), samplingEstimators, peMetrics);
    if (!swAccessPlans.isOK()) {
        metrics.fallbackReason = JoinFallbackReason::kCBRFailedToGetSingleTableAccess;
        return swAccessPlans.getStatus();
    }
    auto singleTableAccess = std::move(swAccessPlans.getValue());

    // A trivially false predicate yields an EOF access plan, which carries no 'SolutionCacheData'
    // and therefore cannot be serialized into the join plan cache.
    const bool cacheWinningPlan = useJoinPlanCache &&
        std::all_of(singleTableAccess.cbrCqQsns.cbegin(),
                    singleTableAccess.cbrCqQsns.cend(),
                    [](const auto& cqQsn) { return cqQsn.second->cacheData != nullptr; });

    // Pre-process indexes per collection to facilitate INLJ enumeration.
    auto indexesPerColl = extractINLJEligibleIndexes(singleTableAccess.cbrCqQsns, mca);
    PerCollUniqueFieldInfo uniqueFieldInfo;
    if (qkc.getEnableJoinOptimizationUseIndexUniqueness()) {
        uniqueFieldInfo = buildUniqueFieldInfo(indexesPerColl);
    }

    JoinReorderingContext ctx{.joinGraph = model.getGraph(),
                              .resolvedPaths = model.getResolvedPaths(),
                              .singleTableAccess = std::move(singleTableAccess),
                              .perCollIdxs = std::move(indexesPerColl),
                              .catStats = createCatalogStats(opCtx, mca),
                              .uniqueFieldInfo = std::move(uniqueFieldInfo),
                              .samplingEstimators = &samplingEstimators,
                              .explain = expCtx->getExplain().has_value()};

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
                                             cacheWinningPlan /* populateCachedJoinPlan */,
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
                                                false /* enableHJOrderPruning */,
                                                3 /* maxRandomHintRetries */,
                                                *metrics.planEnumerationMetrics);
    }();
    uassertStatusOK(swReordered.getStatus());
    auto reordered = std::move(swReordered.getValue());

    // Store the winning plan in the join plan cache for future queries with the same shape.
    if (cacheWinningPlan && reordered.cachedJoinPlan) {
        tassert(13036804,
                "Join plan cache key must be set when the join plan cache is in use",
                cacheKey.has_value());
        tassert(13036805,
                "Join plan cache eligible indexes must be set when the join plan cache is in use",
                cacheEligibleIdxs.has_value());

        auto entry = std::make_unique<JoinPlanCacheEntry>(
            std::move(reordered.cachedJoinPlan),
            reordered.baseNode,
            makeCollectionTags(mca),
            makeNodeFingerprints(model.getGraph(), model.getResolvedPaths(), *cacheEligibleIdxs));
        JoinPlanCache::get(opCtx->getServiceContext()).put(std::move(*cacheKey), std::move(entry));
    }

    // Identify suffix stages that are eligible for SBE pushdown & consequently lower them to the
    // SBE executor with the join-reordered prefix.
    if (auto* suffix = model.getSuffix(); suffix && suffix->peekFront()) {

        auto& prefix = *model.getGraph().getNode(reordered.baseNode).accessPath;
        // This helper identifies the stages in the 'suffix' pipeline that are eligible for running
        // in SBE and prepend them to prefix.cqPipeline.

        // When we call extendWithAggPipeline() below, it will mutate the existing join reordered
        // query solution such that the stages in prefix.cqPipeline are grafted above the join
        // optimized stages. Any stages not eligible for join reordering or SBE pushdown remain
        // in the `suffix` pipeline.
        attachPipelineStages(
            mca,
            suffix,
            false /* needsMerge */,
            &prefix,
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForPushDownStagesDecision{
                .opCtx = opCtx,
                .canonicalQuery = prefix,
                .collections = mca,
                .plannerOptions = QueryPlannerParams::DEFAULT,
            }));

        // 'attachPipelineStages()' prepends the SBE-eligible suffix stages onto
        // 'prefix.cqPipeline()', so its size is exactly the number of stages we lowered.
        const size_t numPushed = prefix.cqPipeline().size();

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
            reordered.soln =
                QueryPlanner::extendWithAggPipeline(prefix,
                                                    std::move(reordered.soln),
                                                    plannerParams.secondaryCollectionsInfo,
                                                    false /* skipOptimization */);
        }
        // Remove any of the stages pushed down via attachPipelineStages, from the pipeline 'suffix'
        // so they are not executed twice (once in SBE and again in classic). Only the remaining
        // stages in suffix will be executed in classic.
        finalizePipelineStages(suffix, &prefix);

        recordSuffixLoweringMetrics(suffix, numPushed, metrics);
    } else {
        recordSuffixLoweringMetrics(suffix, 0 /* numPushedToSbe */, metrics);
    }

    // Test hook: all sampling and join reordering is complete at this point.
    hangAfterJoinModelConstruction.pauseWhileSet(opCtx);

    auto& baseNodeCQ = *model.getGraph().accessPathAt(reordered.baseNode);

    // Merge join-field non-array path learnings into the chosen base node's expCtx so the
    // PathArraynessChecker monitors them during execution yields.
    baseNodeCQ.getExpCtx()->mergeNonArrayPathsForNss(
        model.getJoinExpCtx()->getNonArrayPathsForNss());

    // Lower to SBE.
    auto [planStagesAndData, sbeYieldPolicy] = lowerToSbePlanStageTree(opCtx,
                                                                       model.getGraph(),
                                                                       yieldPolicy,
                                                                       mca,
                                                                       reordered.baseNode,
                                                                       *reordered.soln,
                                                                       &reordered.estimates,
                                                                       true /* prepare */,
                                                                       metrics);

    sbe::DebugPrintInfo debugPrintInfo{};
    LOGV2_DEBUG(11083905,
                5,
                "SBE plan for join-reordered query",
                "sbePlan"_attr =
                    sbe::DebugPrinter{}.print(planStagesAndData.first->debugPrint(debugPrintInfo)),
                "sbePlanStageData"_attr = planStagesAndData.second.debugString());

    // If there is a pipeline suffix, then that suffix will execute inside a
    // PlanExecutorPipeline, which expects to received owned BSON objects from the inner
    // PlanExecutor.
    size_t plannerOptions = QueryPlannerParams::DEFAULT;
    if (model.getSuffix() && model.getSuffix()->peekFront()) {
        plannerOptions |= QueryPlannerParams::RETURN_OWNED_DATA;
    }

    // Prepare rejected plans if any.
    std::vector<JoinOptPlan> rejectedPlans;
    if (ctx.explain) {
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
                                                              false /* prepare */,
                                                              rejectedMetrics);
            rejectedPlans.push_back(JoinOptPlan{.soln = std::move(soln),
                                                .stage = std::move(stagesAndData.first),
                                                .data = std::move(stagesAndData.second)});
        }
    }

    // Collect per-namespace sampling metadata for explain output.
    boost::optional<PlanExplainerData> maybeExplainData;
    if (ctx.explain) {
        PlanExplainerData explainData;
        for (const auto& [nss, estimator] : samplingEstimators) {
            explainData.ceSamplingMetadata.emplace(
                NamespaceStringUtil::serialize(nss, expCtx->getSerializationContext()),
                estimator->getSamplingMetadata());
        }
        // Compute the join plan cache key hash for explain regardless of whether the join plan
        // cache is enabled.
        if (!cacheKey.has_value()) {
            cacheKey = makeJoinPlanCacheKey(model.getGraph(), model.getResolvedPaths(), mca);
        }
        explainData.joinPlanCacheKeyHash = canonical_query_encoder::computeHash(*cacheKey);
        maybeExplainData = std::move(explainData);
    }

    // TODO SERVER-111913: Once we are no-longer cloning QSN for single-table plans, the
    // estimate map from join-reordering 'reordered.estimates' can be combined with the estimate
    // map from CBR 'ctx.singleTableAccess.estimate' before creating the executor below. We
    // actually have several canonical queries, so we don't try to pass one in.
    auto exec = plan_executor_factory::make(opCtx,
                                            nullptr /* cq */,
                                            std::move(reordered.soln),
                                            std::move(planStagesAndData),
                                            mca,
                                            plannerOptions,
                                            mca.getMainCollection()->ns(),
                                            std::move(sbeYieldPolicy),
                                            false /* isFromPlanCache */,
                                            false /* cachedPlanHash */,
                                            true /*usedJoinOpt*/,
                                            std::move(reordered.estimates),
                                            std::move(rejectedPlans),
                                            nullptr /* remoteCursors */,
                                            nullptr /* remoteExplains */,
                                            nullptr /* classicRuntimePlannerStage */,
                                            std::move(maybeExplainData));

    return JoinReorderedExecutorResult{.executor = std::move(exec), .model = std::move(model)};
}
}  // namespace mongo::join_ordering
