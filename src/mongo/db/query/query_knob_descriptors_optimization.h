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
         internalQueryPlanEvaluationMaxResults)                                           \
    KNOB(kPlanEvaluationCollFraction,                                                     \
         kInternalQueryPlanEvaluationCollFractionName,                                    \
         internalQueryPlanEvaluationCollFraction)                                         \
    KNOB(kPlanTotalEvaluationCollFraction,                                                \
         kInternalQueryPlanTotalEvaluationCollFractionName,                               \
         internalQueryPlanTotalEvaluationCollFraction)                                    \
    /* Planning and enumeration */                                                        \
    KNOB(kPlannerMaxIndexedSolutions,                                                     \
         kInternalQueryPlannerMaxIndexedSolutionsName,                                    \
         internalQueryPlannerMaxIndexedSolutions)                                         \
    KNOB(kMaxScansToExplode,                                                              \
         kInternalQueryMaxScansToExplodeName,                                             \
         internalQueryMaxScansToExplode)                                                  \
    KNOB(kPlannerUseMultiplannerForSingleSolutions,                                       \
         kInternalQueryPlannerUseMultiplannerForSingleSolutionsName,                      \
         internalQueryPlannerUseMultiplannerForSingleSolutions)                           \
    KNOB(kMinAllPlansEnumerationSubsetLevel,                                              \
         kInternalMinAllPlansEnumerationSubsetLevelName,                                  \
         internalMinAllPlansEnumerationSubsetLevel)                                       \
    KNOB(kMaxAllPlansEnumerationSubsetLevel,                                              \
         kInternalMaxAllPlansEnumerationSubsetLevelName,                                  \
         internalMaxAllPlansEnumerationSubsetLevel)                                       \
    /* Join optimization */                                                               \
    KNOB(kEnableJoinOptimization,                                                         \
         kInternalEnableJoinOptimizationName,                                             \
         internalEnableJoinOptimization)                                                  \
    KNOB(kRandomJoinOrderSeed,                                                            \
         kInternalRandomJoinOrderSeedName,                                                \
         internalRandomJoinOrderSeed)                                                     \
    KNOB(kMaxNodesInJoinGraph,                                                            \
         kInternalMaxNodesInJoinGraphName,                                                \
         internalMaxNodesInJoinGraph)                                                     \
    KNOB(kMaxEdgesInJoinGraph,                                                            \
         kInternalMaxEdgesInJoinGraphName,                                                \
         internalMaxEdgesInJoinGraph)                                                     \
    KNOB(kInferSingleTablePredicates,                                                     \
         kInternalInferSingleTablePredicatesName,                                         \
         internalInferSingleTablePredicates)                                              \
    KNOB(kMaxNumberNodesConsideredForImplicitEdges,                                       \
         kInternalMaxNumberNodesConsideredForImplicitEdgesName,                           \
         internalMaxNumberNodesConsideredForImplicitEdges)                                \
    KNOB(kEnableJoinEnumerationHJOrderPruning,                                            \
         kInternalEnableJoinEnumerationHJOrderPruningName,                                \
         internalEnableJoinEnumerationHJOrderPruning)                                     \
    KNOB(kEnableJoinOptimizationUseIndexUniqueness,                                       \
         kInternalEnableJoinOptimizationUseIndexUniquenessName,                           \
         internalEnableJoinOptimizationUseIndexUniqueness)                                \
    KNOB(kJoinPlanSamplingSize,                                                           \
         kInternalJoinPlanSamplingSizeName,                                               \
         internalJoinPlanSamplingSize)                                                    \
    KNOB(kJoinEnumerateCollScanPlans,                                                     \
         kInternalJoinEnumerateCollScanPlansName,                                         \
         internalJoinEnumerateCollScanPlans)                                              \
    /* Sampling / cardinality estimation */                                               \
    KNOB(kSamplingMarginOfError,                                                          \
         kSamplingMarginOfErrorName,                                                      \
         samplingMarginOfError)                                                           \
    KNOB(kNumChunksForChunkBasedSampling,                                                 \
         kInternalQueryNumChunksForChunkBasedSamplingName,                                \
         internalQueryNumChunksForChunkBasedSampling)                                     \
    /* Pipeline rewrites */                                                               \
    KNOB(kEnablePathArrayness,                                                            \
         kInternalEnablePathArraynessName,                                                \
         internalEnablePathArrayness)                                                     \
    KNOB(kEnablePipelineOptimizationAdditionalTestingRules,                               \
         kInternalEnablePipelineOptimizationAdditionalTestingRulesName,                   \
         internalEnablePipelineOptimizationAdditionalTestingRules)                        \
    /* cpp_class enum knobs */                                                            \
    KNOB(kPlanRankerMode,                                                                 \
         kInternalQueryCBRCEModeName,                                                     \
         QueryPlanRankerMode)                                                             \
    KNOB(kPlanRankingStrategyForAutomaticQueryPlanRankerMode,                             \
         kAutomaticCEPlanRankingStrategyName,                                             \
         QueryPlanRankingStrategyForAutomaticQueryPlanRankerMode)                         \
    KNOB(kSamplingConfidenceInterval,                                                     \
         kSamplingConfidenceIntervalName,                                                 \
         SamplingConfidenceInterval)                                                      \
    KNOB(kSamplingCEMethod,                                                               \
         kInternalQuerySamplingCEMethodName,                                              \
         CBRSamplingCEMethod)                                                             \
    KNOB(kJoinReorderMode,                                                                \
         kInternalJoinReorderModeName,                                                    \
         JoinReorderMode)                                                                 \
    KNOB(kJoinPlanTreeShape,                                                              \
         kInternalJoinPlanTreeShapeName,                                                  \
         JoinPlanTreeShape)                                                               \
    KNOB(kJoinMethod,                                                                     \
         kInternalJoinMethodName,                                                         \
         ForcedJoinMethod)                                                                \
    KNOB(kJoinSamplingCEMethod,                                                           \
         kInternalJoinOptimizationSamplingCEMethodName,                                   \
         JoinSamplingCEMethod)                                                            \
    /* End MONGO_EXPAND_QUERY_KNOBS_OPTIMIZATION */
// clang-format on

namespace mongo::query_knobs {
DECLARE_QUERY_KNOBS(QueryOptimizationKnobs, MONGO_EXPAND_QUERY_KNOBS_OPTIMIZATION)
}  // namespace mongo::query_knobs
