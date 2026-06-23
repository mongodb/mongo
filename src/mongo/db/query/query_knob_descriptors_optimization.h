/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

/**
 * EXPAND table of QueryKnob<T>s mirroring the server parameters in query_optimization_knobs.idl.
 */

#pragma once

#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"

// clang-format off
#define MONGO_EXPAND_QUERY_KNOBS_OPTIMIZATION(KNOB)                                      \
    /* Multi-plan ranking */                                                              \
    KNOB(kPlanEvaluationMaxResults,                                                       \
         kInternalQueryPlanEvaluationMaxResultsName,                                      \
         internalQueryPlanEvaluationMaxResults,                                           \
         getPlanEvaluationMaxResultsForOp)                                                \
    KNOB(kPlanEvaluationCollFraction,                                                     \
         kInternalQueryPlanEvaluationCollFractionName,                                    \
         internalQueryPlanEvaluationCollFraction,                                         \
         getPlanEvaluationCollFraction)                                                   \
    KNOB(kPlanTotalEvaluationCollFraction,                                                \
         kInternalQueryPlanTotalEvaluationCollFractionName,                               \
         internalQueryPlanTotalEvaluationCollFraction,                                    \
         getPlanTotalEvaluationCollFraction)                                              \
    /* Planning and enumeration */                                                        \
    KNOB(kPlannerMaxIndexedSolutions,                                                     \
         kInternalQueryPlannerMaxIndexedSolutionsName,                                    \
         internalQueryPlannerMaxIndexedSolutions,                                         \
         getPlannerMaxIndexedSolutions)                                                   \
    KNOB(kMaxScansToExplode,                                                              \
         kInternalQueryMaxScansToExplodeName,                                             \
         internalQueryMaxScansToExplode,                                                  \
         getMaxScansToExplodeForOp)                                                       \
    KNOB(kPlannerUseMultiplannerForSingleSolutions,                                       \
         kInternalQueryPlannerUseMultiplannerForSingleSolutionsName,                      \
         internalQueryPlannerUseMultiplannerForSingleSolutions,                           \
         getUseMultiplannerForSingleSolutions)                                            \
    KNOB(kMinAllPlansEnumerationSubsetLevel,                                              \
         kInternalMinAllPlansEnumerationSubsetLevelName,                                  \
         internalMinAllPlansEnumerationSubsetLevel,                                       \
         getInternalMinAllPlansEnumerationSubsetLevel)                                    \
    KNOB(kMaxAllPlansEnumerationSubsetLevel,                                              \
         kInternalMaxAllPlansEnumerationSubsetLevelName,                                  \
         internalMaxAllPlansEnumerationSubsetLevel,                                       \
         getInternalMaxAllPlansEnumerationSubsetLevel)                                    \
    /* Join optimization */                                                               \
    KNOB(kEnableJoinOptimization,                                                         \
         kInternalEnableJoinOptimizationName,                                             \
         internalEnableJoinOptimization,                                                  \
         isJoinOrderingEnabled)                                                           \
    KNOB(kRandomJoinOrderSeed,                                                            \
         kInternalRandomJoinOrderSeedName,                                                \
         internalRandomJoinOrderSeed,                                                     \
         getRandomJoinOrderSeed)                                                          \
    KNOB(kMaxNodesInJoinGraph,                                                            \
         kInternalMaxNodesInJoinGraphName,                                                \
         internalMaxNodesInJoinGraph,                                                     \
         getMaxNodesInJoinGraph)                                                          \
    KNOB(kMaxEdgesInJoinGraph,                                                            \
         kInternalMaxEdgesInJoinGraphName,                                                \
         internalMaxEdgesInJoinGraph,                                                     \
         getMaxEdgesInJoinGraph)                                                          \
    KNOB(kInferSingleTablePredicates,                                                     \
         kInternalInferSingleTablePredicatesName,                                         \
         internalInferSingleTablePredicates,                                              \
         getInferSingleTablePredicates)                                                   \
    KNOB(kMaxNumberNodesConsideredForImplicitEdges,                                       \
         kInternalMaxNumberNodesConsideredForImplicitEdgesName,                           \
         internalMaxNumberNodesConsideredForImplicitEdges,                                \
         getMaxNumberNodesConsideredForImplicitEdges)                                     \
    KNOB(kEnableJoinEnumerationHJOrderPruning,                                            \
         kInternalEnableJoinEnumerationHJOrderPruningName,                                \
         internalEnableJoinEnumerationHJOrderPruning,                                     \
         getEnableJoinEnumerationHJOrderPruning)                                          \
    KNOB(kEnableJoinOptimizationUseIndexUniqueness,                                       \
         kInternalEnableJoinOptimizationUseIndexUniquenessName,                           \
         internalEnableJoinOptimizationUseIndexUniqueness,                                \
         getEnableJoinOptimizationUseIndexUniqueness)                                     \
    KNOB(kJoinPlanSamplingSize,                                                           \
         kInternalJoinPlanSamplingSizeName,                                               \
         internalJoinPlanSamplingSize,                                                    \
         getInternalJoinPlanSamplingSize)                                                 \
    KNOB(kJoinEnumerateCollScanPlans,                                                     \
         kInternalJoinEnumerateCollScanPlansName,                                         \
         internalJoinEnumerateCollScanPlans,                                              \
         getInternalJoinEnumerateCollScanPlans)                                           \
    /* Sampling / cardinality estimation */                                               \
    KNOB(kSamplingMarginOfError,                                                          \
         kSamplingMarginOfErrorName,                                                      \
         samplingMarginOfError,                                                           \
         getSamplingMarginOfError)                                                        \
    KNOB(kNumChunksForChunkBasedSampling,                                                 \
         kInternalQueryNumChunksForChunkBasedSamplingName,                                \
         internalQueryNumChunksForChunkBasedSampling,                                     \
         getNumChunksForChunkBasedSampling)                                               \
    /* Pipeline rewrites */                                                               \
    KNOB(kEnablePathArrayness,                                                            \
         kInternalEnablePathArraynessName,                                                \
         internalEnablePathArrayness,                                                     \
         getEnablePathArrayness)                                                          \
    KNOB(kEnablePipelineOptimizationAdditionalTestingRules,                               \
         kInternalEnablePipelineOptimizationAdditionalTestingRulesName,                   \
         internalEnablePipelineOptimizationAdditionalTestingRules,                        \
         getEnablePipelineOptimizationAdditionalTestingRules)                             \
    /* cpp_class enum knobs */                                                            \
    KNOB(kPlanRankerMode,                                                                 \
         kInternalQueryCBRCEModeName,                                                     \
         QueryPlanRankerMode,                                                             \
         getPlanRankerMode)                                                               \
    KNOB(kPlanRankingStrategyForAutomaticQueryPlanRankerMode,                             \
         kAutomaticCEPlanRankingStrategyName,                                             \
         QueryPlanRankingStrategyForAutomaticQueryPlanRankerMode,                         \
         getPlanRankingStrategyForAutomaticQueryPlanRankerMode)                           \
    KNOB(kSamplingConfidenceInterval,                                                     \
         kSamplingConfidenceIntervalName,                                                 \
         SamplingConfidenceInterval,                                                      \
         getConfidenceInterval)                                                           \
    KNOB(kSamplingCEMethod,                                                               \
         kInternalQuerySamplingCEMethodName,                                              \
         CBRSamplingCEMethod,                                                             \
         getInternalQuerySamplingCEMethod)                                                \
    KNOB(kJoinReorderMode,                                                                \
         kInternalJoinReorderModeName,                                                    \
         JoinReorderMode,                                                                 \
         getJoinReorderMode)                                                              \
    KNOB(kJoinPlanTreeShape,                                                              \
         kInternalJoinPlanTreeShapeName,                                                  \
         JoinPlanTreeShape,                                                               \
         getJoinPlanTreeShape)                                                            \
    KNOB(kJoinMethod,                                                                     \
         kInternalJoinMethodName,                                                         \
         ForcedJoinMethod,                                                                \
         getJoinMethod)                                                                   \
    KNOB(kJoinSamplingCEMethod,                                                           \
         kInternalJoinOptimizationSamplingCEMethodName,                                   \
         JoinSamplingCEMethod,                                                            \
         getInternalJoinOptimizationSamplingCEMethod)                                     \
    /* End MONGO_EXPAND_QUERY_KNOBS_OPTIMIZATION */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryOptimizationKnobs, MONGO_EXPAND_QUERY_KNOBS_OPTIMIZATION)
}  // namespace mongo::query_knobs
