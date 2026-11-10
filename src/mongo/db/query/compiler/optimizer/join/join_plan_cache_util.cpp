// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_plan_cache_util.h"

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/query/all_indices_required_checker.h"
#include "mongo/db/query/compiler/optimizer/join/index_fingerprint.h"
#include "mongo/db/query/plan_cache/join_plan_cache_key.h"
#include "mongo/db/query/sbe_trial_runtime_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryJoin

namespace mongo::join_ordering {

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

/**
 * Number of times a cached plan was found but had gone stale and was evicted, a subset of
 * 'joinPlanCacheMisses'. Exposed in serverStatus as
 * 'metrics.query.planCache.join.invalidations'.
 */
auto& joinPlanCacheInvalidations = *MetricBuilder<Counter64>{"query.planCache.join.invalidations"};

/** Number of plans not cached because their baseline exceeded the cache-eligibility threshold. */
auto& joinPlanCacheBaselineOverThreshold =
    *MetricBuilder<Counter64>{"query.planCache.join.baselineOverThreshold"};

/** Number of plans not cached because their baseline trial hit the stash byte cap. */
auto& joinPlanCacheBaselineOverStashMemoryLimit =
    *MetricBuilder<Counter64>{"query.planCache.join.baselineOverStashMemoryLimit"};

/**
 * Returns true if the cached entry 'hit' can still be used against the current catalog, and false
 * if it is stale and the query must be replanned.
 *
 * 'planCacheKeyHex' is an output parameter, left empty on the fast 'kCurrent' path and otherwise
 * filled lazily with the formatted plan cache key once a log line fires.
 *
 * A bumped collection version alone is not enough to reject the entry. The DDL responsible may have
 * touched an index this plan could never use, or one it considered but did not choose; neither can
 * change which plan the optimizer would pick now. The per-node index fingerprints tell the two
 * cases apart from an index change that does matter.
 */
bool validateCacheEntry(JoinPlanCacheEntry& hit,
                        const MultipleCollectionAccessor& graphOnlyMca,
                        const AggJoinModel& model,
                        const AvailableIndexes& perCollIdxs,
                        const JoinPlanCacheKey& cacheKey,
                        std::string& planCacheKeyHex) {
    auto cachedTags = hit.getCollectionTags();
    auto validation = classifyCollectionTags(cachedTags, graphOnlyMca);

    // The formatted plan cache key costs a hash pass over the whole key plus an allocation, so only
    // compute it when a log line will actually be emitted; the value lands in 'planCacheKeyHex' so
    // this function's reason logs and the caller's removal log share it instead of hashing twice.
    const auto getPlanCacheKeyHex = [&]() -> const std::string& {
        if (planCacheKeyHex.empty()) {
            planCacheKeyHex = joinPlanCacheKeyForLog(cacheKey);
        }
        return planCacheKeyHex;
    };

    switch (validation.status) {
        case CollectionTagStatus::kCurrent:
            // Nothing has changed since the entry was cached, so no fingerprinting is needed.
            return true;
        case CollectionTagStatus::kStale:
            // A stale tag can mean a dropped collection or a sample refresh.
            if (validation.droppedCollectionUuid) {
                LOGV2(12926600,
                      "Join plan cache entry references a collection which no longer exists",
                      "planCacheKey"_attr = getPlanCacheKeyHex(),
                      "planShape"_attr = hit.joinTree ? hit.joinTree->toBSONForLog() : BSONObj(),
                      "uuid"_attr = *validation.droppedCollectionUuid);
            }
            return false;
        case CollectionTagStatus::kNeedsIndexRevalidation:
            LOGV2_DEBUG(13445403,
                        5,
                        "the entry against the current indexes",
                        "planCacheKey"_attr = getPlanCacheKeyHex());
            break;
    }

    tassert(13036801,
            "Cached join plan must have one index fingerprint per join graph node",
            hit.nodeFingerprints.size() == model.getGraph().numNodes());

    auto currentFingerprints = makeNodeFingerprints(
        model.getGraph(), model.getResolvedPaths(), perCollIdxs, *hit.joinTree);
    for (size_t node = 0; node < hit.nodeFingerprints.size(); ++node) {
        if (!canReuseNodeFingerprint(hit.nodeFingerprints[node], currentFingerprints[node])) {
            LOGV2(13036802,
                  "Join plan cache entry invalidated by a DDL",
                  "planCacheKey"_attr = getPlanCacheKeyHex(),
                  "planShape"_attr = hit.joinTree ? hit.joinTree->toBSONForLog() : BSONObj(),
                  "node"_attr = static_cast<int>(node),
                  "nss"_attr = redact(toStringForLogging(
                      model.getGraph().getNode(static_cast<NodeId>(node)).collectionName)),
                  "cachedUsedFingerprint"_attr =
                      static_cast<unsigned long long>(hit.nodeFingerprints[node].usedFingerprint),
                  "currentUsedFingerprint"_attr =
                      static_cast<unsigned long long>(currentFingerprints[node].usedFingerprint),
                  "cachedRelevantIndexCount"_attr =
                      static_cast<int>(hit.nodeFingerprints[node].relevantIndexHashes.size()),
                  "currentRelevantIndexCount"_attr =
                      static_cast<int>(currentFingerprints[node].relevantIndexHashes.size()));
            return false;
        }
    }

    // The catalog change either left the relevant indexes untouched or only dropped ones this plan
    // does not use, so the entry is valid against the current collection state. Adopt that state's
    // version tags so subsequent lookups take the fast path above instead of re-fingerprinting on
    // every query.
    auto currentTags = makeCollectionTags(graphOnlyMca);
    hit.refreshCollectionTags(currentTags);

    LOGV2_DEBUG(13036803,
                5,
                "Join plan cache entry revalidated: the catalog change did not affect any index "
                "this plan relies on",
                "cachedVersions"_attr = collectionVersionsForLog(cachedTags),
                "currentVersions"_attr = collectionVersionsForLog(currentTags));
    return true;
}

/**
 * Returns the cached entry for 'cacheKey' if it is still usable, removing it if it is stale.
 */
std::shared_ptr<JoinPlanCacheEntry> lookupValidEntry(OperationContext* opCtx,
                                                     const JoinPlanCacheKey& cacheKey,
                                                     const MultipleCollectionAccessor& graphOnlyMca,
                                                     const AggJoinModel& model,
                                                     const AvailableIndexes& perCollIdxs) {
    auto& cache = JoinPlanCache::get(opCtx->getServiceContext());
    auto hit = cache.lookup(cacheKey);
    if (!hit) {
        return nullptr;
    }

    // validateCacheEntry fills 'planCacheKeyHex' lazily while deciding the entry is stale, so the
    // failure path below reuses it instead of hashing the key a second time.
    std::string planCacheKeyHex;
    if (validateCacheEntry(*hit, graphOnlyMca, model, perCollIdxs, cacheKey, planCacheKeyHex)) {
        return hit;
    }

    if (planCacheKeyHex.empty()) {
        planCacheKeyHex = joinPlanCacheKeyForLog(cacheKey);
    }
    if (cache.removeIfMatches(cacheKey, hit)) {
        joinPlanCacheInvalidations.increment(1);
        LOGV2(13445401, "Join plan cache entry removed", "planCacheKey"_attr = planCacheKeyHex);
    } else {
        LOGV2_DEBUG(13445402,
                    2,
                    "Join plan cache entry invalidated but already removed or replaced by "
                    "another operation; no removal performed",
                    "planCacheKey"_attr = planCacheKeyHex);
    }
    return nullptr;
}
}  // namespace

bool canUseJoinPlanCache(const QueryKnobConfiguration& qkc, bool hasHint, bool isExplain) {
    return !hasHint && qkc.getJoinReorderMode() != JoinReorderModeEnum::kRandom &&
        qkc.getEnableJoinPlanCache() && !isExplain;
}

JoinPlanCacheLookupResult lookupInJoinPlanCache(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& graphOnlyMca,
    const AggJoinModel& model,
    const AvailableIndexes& perCollIdxs,
    const query_settings::QuerySettings& querySettings) {
    auto cacheKey = makeJoinPlanCacheKey(
        model.getGraph(), model.getResolvedPaths(), graphOnlyMca, querySettings);
    if (auto entry = lookupValidEntry(opCtx, cacheKey, graphOnlyMca, model, perCollIdxs)) {
        return JoinPlanCacheHit{.key = std::move(cacheKey), .entry = std::move(entry)};
    }
    // Capture the tags before sampling: a yield re-acquires collections from the latest
    // catalog, which would tag the entry with a newer state than the plan is built from.
    auto collectionTags = makeCollectionTags(graphOnlyMca);
    joinPlanCacheMisses.increment(1);
    LOGV2_DEBUG(11083907, 5, "Join plan cache miss, running optimization");
    return JoinPlanCacheMiss{.key = std::move(cacheKey),
                             .collectionTags = std::move(collectionTags)};
}

void recordJoinPlanCacheHit() {
    joinPlanCacheHits.increment(1);
}

bool canCacheAccessPlans(const SingleTableAccessPlansResult& singleTableAccess) {
    return std::all_of(singleTableAccess.cbrCqQsns.cbegin(),
                       singleTableAccess.cbrCqQsns.cend(),
                       [](const auto& cqQsn) { return cqQsn.second->cacheData != nullptr; });
}

std::unique_ptr<TrialRunTracker> makeBaselineTracker() {
    return TrialRunTracker::makeUncapped();
}

JoinPlanBaseline baselineFromTrial(const TrialRunTracker& tracker, size_t numResults) {
    return JoinPlanBaseline{
        .numReads = tracker.getMetric<TrialRunTracker::kNumReads>(),
        .numIntermediateRows = tracker.getMetric<TrialRunTracker::kNumIntermediateRows>(),
        .spilled = tracker.getMetric<TrialRunTracker::kNumSpills>() > 0,
        .numResults = numResults,
    };
}

JoinTrialResult runJoinTrial(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& mca,
    const CanonicalQuery& baseCQ,
    PlanYieldPolicySBE* yieldPolicy,
    std::unique_ptr<QuerySolution> soln,
    std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData> planStagesAndData,
    std::unique_ptr<TrialRunTracker> tracker,
    size_t maxNumResults) {
    sbe::plan_ranker::CandidatePlan candidate{
        .solution = std::move(soln),
        .root = std::move(planStagesAndData.first),
        .data = sbe::plan_ranker::CandidatePlanData{std::move(planStagesAndData.second)},
        .status = Status::OK(),
        .fromPlanCache = false};
    // TrialRuntimeExecutor reads the tracker through the candidate, so the candidate must own it.
    candidate.data.tracker = std::move(tracker);
    candidate.root->attachToTrialRunTracker(candidate.data.tracker.get());
    ON_BLOCK_EXIT([rootPtr = candidate.root.get()] { rootPtr->detachFromTrialRunTracker(); });

    // Never fires here: nothing yields before the check. DDL during the trial is caught on restore.
    const AllIndicesRequiredChecker indexExistenceChecker{mca};
    sbe::TrialRuntimeExecutor trialExecutor{
        opCtx,
        mca,
        baseCQ,
        yieldPolicy,
        indexExistenceChecker,
        static_cast<size_t>(
            baseCQ.getExpCtx()->getQueryKnobConfiguration().getSbePlanEvaluationMaxMemoryBytes())};

    trialExecutor.prepareCandidate(&candidate, /*preparingFromCache=*/false);
    while (trialExecutor.fetchNextDocument(&candidate, maxNumResults)) {
    }

    // The trial executor reports recoverable failures through the status instead of throwing.
    uassertStatusOK(candidate.status);
    auto baseline = baselineFromTrial(*candidate.data.tracker, candidate.results.size());
    return {.candidate = std::move(candidate), .baseline = baseline};
}

JoinPlanCacheInsertResult checkCanInsertIntoJoinPlanCache(bool collectionsUnchanged,
                                                          bool hitStashMemoryLimit,
                                                          const JoinPlanBaseline& baseline,
                                                          size_t maxReadsAndIntermediateRows) {
    if (!collectionsUnchanged) {
        return JoinPlanCacheInsertResult::kSkipCollectionsChanged;
    }
    if (hitStashMemoryLimit) {
        return JoinPlanCacheInsertResult::kSkipOverStashMemoryLimit;
    }
    // Only cache inexpensive plans, where optimization time is a large share of total execution
    // time. An expensive plan is re-optimized on every run instead since the time a cache hit
    // would save is small next to its execution and thus, fresh optimization guards against
    // potentially re-using a bad plan and paying the high price of replanning an expensive
    // query.
    if (baseline.numReads + baseline.numIntermediateRows > maxReadsAndIntermediateRows) {
        return JoinPlanCacheInsertResult::kSkipOverThreshold;
    }
    return JoinPlanCacheInsertResult::kInsert;
}

void tryInsertIntoJoinPlanCache(OperationContext* opCtx,
                                JoinPlanCacheMiss miss,
                                const MultipleCollectionAccessor& graphOnlyMca,
                                const AggJoinModel& model,
                                const AvailableIndexes& perCollIdxs,
                                const Pipeline& pipeline,
                                std::unique_ptr<CachedJoinPlan> cachedJoinPlan,
                                NodeId baseNode,
                                const JoinPlanBaseline& baseline,
                                bool hitStashMemoryLimit,
                                size_t maxReadsAndIntermediateRows) {
    // A yield may have occurred during the trial. Re-check CollectionTags and skip caching if the
    // live CollectionTags have advanced.
    const bool collectionsUnchanged =
        classifyCollectionTags(miss.collectionTags, graphOnlyMca).status ==
        CollectionTagStatus::kCurrent;

    switch (checkCanInsertIntoJoinPlanCache(
        collectionsUnchanged, hitStashMemoryLimit, baseline, maxReadsAndIntermediateRows)) {
        case JoinPlanCacheInsertResult::kSkipCollectionsChanged:
            LOGV2_DEBUG(135049,
                        5,
                        "Skipping join plan cache write: index DDL occurred during planning",
                        "planCacheKey"_attr = joinPlanCacheKeyForLog(miss.key));
            return;
        case JoinPlanCacheInsertResult::kSkipOverStashMemoryLimit:
            // Don't cache a plan whose trial stash hit the byte cap. We assume a plan producing
            // this much output is likely long-running anyway.
            joinPlanCacheBaselineOverStashMemoryLimit.increment(1);
            LOGV2_DEBUG(13673703,
                        5,
                        "Skipping join plan cache write: baseline trial hit the stash byte cap",
                        "planCacheKey"_attr = joinPlanCacheKeyForLog(miss.key),
                        "baseline"_attr = baseline.toBSON());
            return;
        case JoinPlanCacheInsertResult::kSkipOverThreshold:
            joinPlanCacheBaselineOverThreshold.increment(1);
            LOGV2_DEBUG(
                12501501,
                5,
                "Skipping join plan cache write: baseline exceeds the cache-eligibility threshold",
                "planCacheKey"_attr = joinPlanCacheKeyForLog(miss.key),
                "baseline"_attr = baseline.toBSON());
            return;
        case JoinPlanCacheInsertResult::kInsert:
            break;
    }

    auto fingerprints = makeNodeFingerprints(
        model.getGraph(), model.getResolvedPaths(), perCollIdxs, *cachedJoinPlan);
    const auto& currentTags = miss.collectionTags;
    const auto planCacheKeyHex = joinPlanCacheKeyForLog(miss.key);
    // 'serializeForLogging()' handles redaction of user content per the
    // 'redactClientLogData' policy.
    const auto queryShapeForLog = pipeline.serializeForLogging();
    const NamespaceString baseNss = model.getGraph().getNode(baseNode).collectionName;
    const auto logVersions = collectionVersionsForLog(currentTags);

    auto entry = std::make_unique<JoinPlanCacheEntry>(
        std::move(cachedJoinPlan), baseNode, currentTags, std::move(fingerprints), baseline);
    const BSONObj planShapeForLog = entry->joinTree ? entry->joinTree->toBSONForLog() : BSONObj();
    const long long estimatedSizeBytes = static_cast<long long>(entry->estimatedEntrySizeBytes);
    const size_t numEntriesEvicted =
        JoinPlanCache::get(opCtx->getServiceContext()).put(std::move(miss.key), std::move(entry));

    LOGV2(13445400,
          "Join plan cache entry put",
          "planCacheKey"_attr = planCacheKeyHex,
          "queryShape"_attr = queryShapeForLog,
          "planShape"_attr = planShapeForLog,
          "nss"_attr = redact(toStringForLogging(baseNss)),
          "baseNode"_attr = static_cast<int>(baseNode),
          "estimatedSizeBytes"_attr = estimatedSizeBytes,
          "collections"_attr = logVersions,
          "baseline"_attr = baseline.toBSON(),
          "entriesEvicted"_attr = static_cast<long long>(numEntriesEvicted));
}

}  // namespace mongo::join_ordering
