// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * EXPAND table of QueryKnob<T>s mirroring the server parameters in
 * query_join_optimization_knobs.idl.
 */

#pragma once

#include "mongo/db/query/query_join_optimization_knobs_gen.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/util/modules.h"

// clang-format off
#define MONGO_EXPAND_QUERY_KNOBS_JOIN_OPTIMIZATION(KNOB)                                  \
    KNOB(kEnableJoinOptimization,                                                         \
         kInternalEnableJoinOptimizationName,                                             \
         internalEnableJoinOptimization,                                                  \
         isJoinOrderingEnabled)                                                           \
    KNOB(kExplainJoinCostComponents,                                                      \
         kInternalQueryExplainJoinCostComponentsName,                                     \
         internalQueryExplainJoinCostComponents,                                          \
         getExplainJoinCostComponents)                                                    \
    KNOB(kJoinReorderMode,                                                                \
         kInternalJoinReorderModeName,                                                    \
         JoinReorderMode,                                                                 \
         getJoinReorderMode)                                                              \
    KNOB(kJoinPlanTreeShape,                                                              \
         kInternalJoinPlanTreeShapeName,                                                  \
         JoinPlanTreeShape,                                                               \
         getJoinPlanTreeShape)                                                            \
    KNOB(kMaxNodesInJoinGraph,                                                            \
         kInternalMaxNodesInJoinGraphName,                                                \
         internalMaxNodesInJoinGraph,                                                     \
         getMaxNodesInJoinGraph)                                                          \
    KNOB(kMaxEdgesInJoinGraph,                                                            \
         kInternalMaxEdgesInJoinGraphName,                                                \
         internalMaxEdgesInJoinGraph,                                                     \
         getMaxEdgesInJoinGraph)                                                          \
    KNOB(kMaxNumberNodesConsideredForImplicitEdges,                                       \
         kInternalMaxNumberNodesConsideredForImplicitEdgesName,                           \
         internalMaxNumberNodesConsideredForImplicitEdges,                                \
         getMaxNumberNodesConsideredForImplicitEdges)                                     \
    KNOB(kRandomJoinOrderSeed,                                                            \
         kInternalRandomJoinOrderSeedName,                                                \
         internalRandomJoinOrderSeed,                                                     \
         getRandomJoinOrderSeed)                                                          \
    KNOB(kEnableJoinEnumerationHJOrderPruning,                                            \
         kInternalEnableJoinEnumerationHJOrderPruningName,                                \
         internalEnableJoinEnumerationHJOrderPruning,                                     \
         getEnableJoinEnumerationHJOrderPruning)                                          \
    KNOB(kEnableJoinOptimizationUseIndexUniqueness,                                       \
         kInternalEnableJoinOptimizationUseIndexUniquenessName,                           \
         internalEnableJoinOptimizationUseIndexUniqueness,                                \
         getEnableJoinOptimizationUseIndexUniqueness)                                     \
    KNOB(kJoinMethod,                                                                     \
         kInternalJoinMethodName,                                                         \
         ForcedJoinMethod,                                                                \
         getJoinMethod)                                                                   \
    KNOB(kJoinSamplingCEMethod,                                                           \
         kInternalJoinOptimizationSamplingCEMethodName,                                   \
         JoinSamplingCEMethod,                                                            \
         getInternalJoinOptimizationSamplingCEMethod)                                     \
    KNOB(kJoinPlanSamplingSize,                                                           \
         kInternalJoinPlanSamplingSizeName,                                               \
         internalJoinPlanSamplingSize,                                                    \
         getInternalJoinPlanSamplingSize)                                                 \
    KNOB(kJoinEnumerateCollScanPlans,                                                     \
         kInternalJoinEnumerateCollScanPlansName,                                         \
         internalJoinEnumerateCollScanPlans,                                              \
         getInternalJoinEnumerateCollScanPlans)                                           \
    KNOB(kMinAllPlansEnumerationSubsetLevel,                                              \
         kInternalMinAllPlansEnumerationSubsetLevelName,                                  \
         internalMinAllPlansEnumerationSubsetLevel,                                       \
         getInternalMinAllPlansEnumerationSubsetLevel)                                    \
    KNOB(kMaxAllPlansEnumerationSubsetLevel,                                              \
         kInternalMaxAllPlansEnumerationSubsetLevelName,                                  \
         internalMaxAllPlansEnumerationSubsetLevel,                                       \
         getInternalMaxAllPlansEnumerationSubsetLevel)                                    \
    KNOB(kEnableJoinPlanCache,                                                            \
         kInternalEnableJoinPlanCacheName,                                                \
         internalEnableJoinPlanCache,                                                     \
         getEnableJoinPlanCache)                                                          \
    KNOB(kJoinPlanCacheTrialWindow,                                                       \
         kInternalQueryJoinPlanCacheTrialWindowName,                                      \
         internalQueryJoinPlanCacheTrialWindow,                                           \
         getJoinPlanCacheTrialWindow)                                                     \
    KNOB(kJoinPlanCacheMaxReadsAndIntermediateRows,                                       \
         kInternalQueryJoinPlanCacheMaxReadsAndIntermediateRowsName,                      \
         internalQueryJoinPlanCacheMaxReadsAndIntermediateRows,                           \
         getJoinPlanCacheMaxReadsAndIntermediateRows)                                     \
    /* End MONGO_EXPAND_QUERY_KNOBS_JOIN_OPTIMIZATION */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryJoinOptimizationKnobs, MONGO_EXPAND_QUERY_KNOBS_JOIN_OPTIMIZATION)
}  // namespace mongo::query_knobs
