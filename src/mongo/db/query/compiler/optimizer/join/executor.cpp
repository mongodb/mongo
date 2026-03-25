/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/join/executor.h"

#include "mongo/base/status_with.h"
#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/compiler/optimizer/join/agg_join_model.h"
#include "mongo/db/query/compiler/optimizer/join/cardinality_estimator.h"
#include "mongo/db/query/compiler/optimizer/join/catalog_stats.h"
#include "mongo/db/query/compiler/optimizer/join/hint.h"
#include "mongo/db/query/compiler/optimizer/join/join_cost_estimator_impl.h"
#include "mongo/db/query/compiler/optimizer/join/join_reordering_context.h"
#include "mongo/db/query/compiler/optimizer/join/reorder_joins.h"
#include "mongo/db/query/compiler/optimizer/join/single_table_access.h"
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

#include <algorithm>
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::join_ordering {
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

bool anySecondaryNamespacesDontExist(const MultipleCollectionAccessor& mca) {
    auto colls = mca.getSecondaryCollectionAcquisitions();
    return std::any_of(
        colls.begin(), colls.end(), [](auto&& it) { return !it.second.collectionExists(); });
}

bool isAggEligibleForJoinReordering(const MultipleCollectionAccessor& mca,
                                    const Pipeline& pipeline) {
    const auto& queryKnob = pipeline.getContext()->getQueryKnobConfiguration();

    if (!queryKnob.isJoinOrderingEnabled()) {
        return false;
    }

    if (queryKnob.isForceClassicEngineEnabled()) {
        return false;
    }

    if (!mca.hasMainCollection()) {
        // We can't determine if the base collection is sharded.
        return false;
    }

    if (mca.getMainCollectionAcquisition().getShardingDescription().isSharded()) {
        // We don't permit a sharded base collection.
        return false;
    }

    // Check that no foreign collection is sharded.
    for (const auto& [_, collAcq] : mca.getSecondaryCollectionAcquisitions()) {
        if (collAcq.collectionExists() &&
            collAcq.getCollection().getShardingDescription().isSharded()) {
            // We don't permit sharded foreign collections.
            return false;
        }
    }

    if (mca.isAnySecondaryNamespaceAViewOrNotFullyLocal() || anySecondaryNamespacesDontExist(mca)) {
        // TODO SERVER-112239: Enable support for views, as the above check will prevent views from
        // being used for join ordering.
        return false;
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
        return false;
    }

    return AggJoinModel::pipelineEligibleForJoinReordering(pipeline);
}

bool indexIsValidForINLJ(const std::shared_ptr<const IndexCatalogEntry>& ice) {
    auto desc = ice->descriptor();
    return !desc->isHashedIdIndex() && !desc->hidden() && !desc->isPartial() &&
        !desc->isSetSparseByUser() && desc->collation().isEmpty() &&
        !dynamic_cast<WildcardAccessMethod*>(ice->accessMethod());
}

/**
 * Pre-process indexes to filter out those ineligible for conversion to INLJ, and output a map of
 * collection namespaces to indexes available.
 */
AvailableIndexes extractINLJEligibleIndexes(const QuerySolutionMap& solns,
                                            const MultipleCollectionAccessor& mca) {
    AvailableIndexes perCollIdxs;
    for (const auto& [cq, _] : solns) {
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

CatalogStats createCatalogStats(OperationContext* opCtx, const MultipleCollectionAccessor& mca) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    stdx::unordered_map<NamespaceString, CollectionStats> collStats;
    mca.forEach([&collStats, &ru](const CollectionPtr& coll) {
        auto* recordStore = coll->getRecordStore();
        // TODO SERVER-117620: set .pageSizeBytes.
        collStats.emplace(coll->ns(),
                          CollectionStats{
                              .logicalDataSizeBytes = static_cast<double>(recordStore->dataSize()),
                              .onDiskSizeBytes = static_cast<double>(
                                  recordStore->storageSize(ru) - recordStore->freeStorageSize(ru)),
                          });
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
}  // namespace

/**
 * Attempts to apply join optimization to the given aggregation, but if it fails to extract a join
 * model, falls back to preparing executors for the pipeline in the normal way.
 */
StatusWith<JoinReorderedExecutorResult> getJoinReorderedExecutor(
    const MultipleCollectionAccessor& mca,
    const Pipeline& pipeline,
    OperationContext* opCtx,
    const boost::intrusive_ptr<ExpressionContext> expCtx) {
    // Quick eligibility check.
    if (!isAggEligibleForJoinReordering(mca, pipeline)) {
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "Pipeline or collection ineligible for join-reordering");
    }

    // Try to build JoinGraph.
    const auto& config = pipeline.getContext()->getQueryKnobConfiguration();
    AggModelBuildParams buildParams{
        .joinGraphBuildParams =
            JoinGraphBuildParams(config.getMaxNodesInJoinGraph(), config.getMaxEdgesInJoinGraph()),
        .maxNumberNodesConsideredForImplicitEdges =
            config.getMaxNumberNodesConsideredForImplicitEdges()};
    auto swModel = AggJoinModel::constructJoinModel(pipeline, buildParams);
    if (!swModel.isOK()) {
        // We failed to apply join-reordering, so we take the regular path.
        const auto status = swModel.getStatus();
        LOGV2_DEBUG(11083903, 5, "Unable to construct join model", "status"_attr = status);
        return status;
    }

    // Validate we have all the collection acquisitions we need here.
    bool missingAcquisitions = std::any_of(swModel.getValue().prefix->getSources().begin(),
                                           swModel.getValue().prefix->getSources().end(),
                                           [&](const auto& stage) {
                                               auto* lookup =
                                                   dynamic_cast<DocumentSourceLookUp*>(stage.get());
                                               if (!lookup) {
                                                   return false;
                                               }
                                               return !mca.knowsNamespace(lookup->getFromNs());
                                           });
    if (missingAcquisitions) {
        return Status(
            ErrorCodes::QueryFeatureNotAllowed,
            "Pipeline ineligible for join-reordering due to missing foreign namespace acquisition");
    }

    LOGV2_DEBUG(11083902,
                5,
                "Join model was successfully constructed, reordering joins",
                "graph"_attr = swModel.getValue().toBSON());
    auto model = std::move(swModel.getValue());

    // Select access plans for each table in the join.
    auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;
    SamplingEstimatorMap samplingEstimators = makeSamplingEstimators(mca, model.graph, yieldPolicy);
    auto swAccessPlans = singleTableAccessPlans(opCtx, mca, model.graph, samplingEstimators);
    if (!swAccessPlans.isOK()) {
        return swAccessPlans.getStatus();
    }

    auto& solns = swAccessPlans.getValue().solns;
    const auto qkc = expCtx->getQueryKnobConfiguration();

    // Pre-process indexes per collection to facilitate INLJ enumeration.
    auto indexesPerColl = extractINLJEligibleIndexes(solns, mca);
    PerCollUniqueFieldInfo uniqueFieldInfo;
    if (qkc.getEnableJoinOptimizationUseIndexUniqueness()) {
        uniqueFieldInfo = buildUniqueFieldInfo(indexesPerColl);
    }

    JoinReorderingContext ctx{.joinGraph = model.graph,
                              .resolvedPaths = model.resolvedPaths,
                              .cbrCqQsns = std::move(solns),
                              .perCollIdxs = std::move(indexesPerColl),
                              .catStats = createCatalogStats(opCtx, mca),
                              .uniqueFieldInfo = std::move(uniqueFieldInfo),
                              .explain = expCtx->getExplain().has_value()};

    JoinCardinalityEstimator cardEstimator(
        JoinCardinalityEstimator::make(ctx, swAccessPlans.getValue().estimate, samplingEstimators));
    JoinCostEstimatorImpl costEstimator(ctx, cardEstimator);

    StatusWith<ReorderedJoinSolution> swReordered = [&]() {
        switch (qkc.getJoinReorderMode()) {
            case JoinReorderModeEnum::kBottomUp: {
                // Optimize join order using bottom-up Sellinger-style algorithm.
                return constructSolutionBottomUp(
                    ctx, cardEstimator, costEstimator, getEnumerationStrategy(qkc));
            }
            case JoinReorderModeEnum::kRandom:
                // Randomly reorder joins (while still passing through bottom-up enumerator). NOTE:
                // this currently ignores all query knobs other than the random seed, the plan tree
                // shape and the join method, but could easily be modified to take the values of
                // other query knobs as "overrides".
                return constructSolutionWithRandomOrder(
                    ctx,
                    &cardEstimator,
                    &costEstimator,
                    qkc.getRandomJoinOrderSeed(),
                    getPlanTreeShape(qkc.getJoinPlanTreeShape()),
                    getJoinMethod(qkc.getJoinMethod()));
            default:
                MONGO_UNREACHABLE_TASSERT(11336911);
        }
    }();
    uassertStatusOK(swReordered.getStatus());
    auto reordered = std::move(swReordered.getValue());

    // Lower to SBE.
    // TODO SERVER-112232: Identify SBE suffixes that are eligible for pushdown & push them to the
    // SBE executor.
    auto lower =
        [&model, &opCtx, yieldPolicy, &mca](NodeId baseNode,
                                            const QuerySolution& soln,
                                            const cost_based_ranker::EstimateMap* estimates,
                                            bool prepare) {
            auto& baseCQ = *model.graph.accessPathAt(baseNode);
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
        };

    auto [planStagesAndData, sbeYieldPolicy] =
        lower(reordered.baseNode, *reordered.soln, &reordered.estimates, true /* prepare */);
    sbe::DebugPrintInfo debugPrintInfo{};
    LOGV2_DEBUG(11083905,
                5,
                "SBE plan for join-reordered query",
                "sbePlan"_attr =
                    sbe::DebugPrinter{}.print(planStagesAndData.first->debugPrint(debugPrintInfo)),
                "sbePlanStageData"_attr = planStagesAndData.second.debugString());

    // If there is a pipeline suffix, then that suffix will execute inside a PlanExecutorPipeline,
    // which expects to received owned BSON objects from the inner PlanExecutor.
    size_t plannerOptions = QueryPlannerParams::DEFAULT;
    if (model.suffix && model.suffix->peekFront()) {
        plannerOptions |= QueryPlannerParams::RETURN_OWNED_DATA;
    }

    // Prepare rejected plans if any.
    std::vector<JoinOptPlan> rejectedPlans;
    if (ctx.explain) {
        rejectedPlans.reserve(reordered.rejectedSolns.size());
        for (auto&& rs : reordered.rejectedSolns) {
            auto soln = std::move(rs.first);
            auto baseNode = rs.second;
            auto [stagesAndData, _] =
                lower(baseNode, *soln, &reordered.estimates, false /* prepare */);
            rejectedPlans.push_back(JoinOptPlan{.soln = std::move(soln),
                                                .stage = std::move(stagesAndData.first),
                                                .data = std::move(stagesAndData.second)});
        }
    }

    // TODO SERVER-111913: Once we are no-longer cloning QSN for single-table plans, the estimate
    // map from join-reordering 'reordered.estimates' can be combined with the estimate map from
    // CBR 'swAccessPlans.getValue().estimate' before creating the executor below.
    // We actually have several canonical queries, so we don't try to pass one in.
    auto exec = uassertStatusOK(plan_executor_factory::make(opCtx,
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
                                                            std::move(rejectedPlans)));

    return JoinReorderedExecutorResult{.executor = std::move(exec), .model = std::move(model)};
}
}  // namespace mongo::join_ordering
