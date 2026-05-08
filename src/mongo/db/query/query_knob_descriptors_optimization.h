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
 * QueryKnob<T> descriptors for the plain knobs in query_optimization_knobs.idl.
 * Each self-registers into QueryKnobDescriptorSet at static init.
 */

#pragma once

#include "mongo/db/query/query_knob.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"

namespace mongo::query_knobs {

// Multi-plan ranking ─────────────────────────────────────────────────────────

inline QueryKnob<int> kPlanEvaluationMaxResults{
    kInternalQueryPlanEvaluationMaxResultsName,
    &readGlobalValue<internalQueryPlanEvaluationMaxResults>};

inline QueryKnob<double> kPlanEvaluationCollFraction{
    kInternalQueryPlanEvaluationCollFractionName,
    &readGlobalValue<internalQueryPlanEvaluationCollFraction>};

inline QueryKnob<double> kPlanTotalEvaluationCollFraction{
    kInternalQueryPlanTotalEvaluationCollFractionName,
    &readGlobalValue<internalQueryPlanTotalEvaluationCollFraction>};

// Planning and enumeration ───────────────────────────────────────────────────

inline QueryKnob<int> kPlannerMaxIndexedSolutions{
    kInternalQueryPlannerMaxIndexedSolutionsName,
    &readGlobalValue<internalQueryPlannerMaxIndexedSolutions>};

inline QueryKnob<int> kMaxScansToExplode{kInternalQueryMaxScansToExplodeName,
                                         &readGlobalValue<internalQueryMaxScansToExplode>};

inline QueryKnob<bool> kPlannerUseMultiplannerForSingleSolutions{
    kInternalQueryPlannerUseMultiplannerForSingleSolutionsName,
    &readGlobalValue<internalQueryPlannerUseMultiplannerForSingleSolutions>};

inline QueryKnob<int> kMinAllPlansEnumerationSubsetLevel{
    kInternalMinAllPlansEnumerationSubsetLevelName,
    &readGlobalValue<internalMinAllPlansEnumerationSubsetLevel>};

inline QueryKnob<int> kMaxAllPlansEnumerationSubsetLevel{
    kInternalMaxAllPlansEnumerationSubsetLevelName,
    &readGlobalValue<internalMaxAllPlansEnumerationSubsetLevel>};

// Join optimization ──────────────────────────────────────────────────────────

inline QueryKnob<bool> kEnableJoinOptimization{kInternalEnableJoinOptimizationName,
                                               &readGlobalValue<internalEnableJoinOptimization>};

inline QueryKnob<int> kRandomJoinOrderSeed{kInternalRandomJoinOrderSeedName,
                                           &readGlobalValue<internalRandomJoinOrderSeed>};

inline QueryKnob<int> kMaxNodesInJoinGraph{kInternalMaxNodesInJoinGraphName,
                                           &readGlobalValue<internalMaxNodesInJoinGraph>};

inline QueryKnob<int> kMaxEdgesInJoinGraph{kInternalMaxEdgesInJoinGraphName,
                                           &readGlobalValue<internalMaxEdgesInJoinGraph>};

inline QueryKnob<int> kMaxNumberNodesConsideredForImplicitEdges{
    kInternalMaxNumberNodesConsideredForImplicitEdgesName,
    &readGlobalValue<internalMaxNumberNodesConsideredForImplicitEdges>};

inline QueryKnob<bool> kEnableJoinEnumerationHJOrderPruning{
    kInternalEnableJoinEnumerationHJOrderPruningName,
    &readGlobalValue<internalEnableJoinEnumerationHJOrderPruning>};

inline QueryKnob<bool> kEnableJoinOptimizationUseIndexUniqueness{
    kInternalEnableJoinOptimizationUseIndexUniquenessName,
    &readGlobalValue<internalEnableJoinOptimizationUseIndexUniqueness>};

inline QueryKnob<int> kJoinPlanSamplingSize{kInternalJoinPlanSamplingSizeName,
                                            &readGlobalValue<internalJoinPlanSamplingSize>};

inline QueryKnob<bool> kJoinEnumerateCollScanPlans{
    kInternalJoinEnumerateCollScanPlansName, &readGlobalValue<internalJoinEnumerateCollScanPlans>};

// Sampling / cardinality estimation ──────────────────────────────────────────

inline QueryKnob<double> kSamplingMarginOfError{kSamplingMarginOfErrorName,
                                                &readGlobalValue<samplingMarginOfError>};

inline QueryKnob<int> kNumChunksForChunkBasedSampling{
    kInternalQueryNumChunksForChunkBasedSamplingName,
    &readGlobalValue<internalQueryNumChunksForChunkBasedSampling>};

// Pipeline rewrites ──────────────────────────────────────────────────────────

inline QueryKnob<bool> kEnablePathArrayness{kInternalEnablePathArraynessName,
                                            &readGlobalValue<internalEnablePathArrayness>};

inline QueryKnob<bool> kEnablePipelineOptimizationAdditionalTestingRules{
    kInternalEnablePipelineOptimizationAdditionalTestingRulesName,
    &readGlobalValue<internalEnablePipelineOptimizationAdditionalTestingRules>};

// cpp_class enum knobs ───────────────────────────────────────────────────────

inline QueryKnob<QueryPlanRankerModeEnum> kPlanRankerMode{kInternalQueryCBRCEModeName,
                                                          &readGlobalValue<QueryPlanRankerMode>};

inline QueryKnob<QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum>
    kPlanRankingStrategyForAutomaticQueryPlanRankerMode{
        kAutomaticCEPlanRankingStrategyName,
        &readGlobalValue<QueryPlanRankingStrategyForAutomaticQueryPlanRankerMode>};

inline QueryKnob<SamplingConfidenceIntervalEnum> kSamplingConfidenceInterval{
    kSamplingConfidenceIntervalName, &readGlobalValue<SamplingConfidenceInterval>};

inline QueryKnob<SamplingCEMethodEnum> kSamplingCEMethod{kInternalQuerySamplingCEMethodName,
                                                         &readGlobalValue<CBRSamplingCEMethod>};

inline QueryKnob<JoinReorderModeEnum> kJoinReorderMode{kInternalJoinReorderModeName,
                                                       &readGlobalValue<JoinReorderMode>};

inline QueryKnob<JoinPlanTreeShapeEnum> kJoinPlanTreeShape{kInternalJoinPlanTreeShapeName,
                                                           &readGlobalValue<JoinPlanTreeShape>};

inline QueryKnob<ForcedJoinMethodEnum> kJoinMethod{kInternalJoinMethodName,
                                                   &readGlobalValue<ForcedJoinMethod>};

inline QueryKnob<SamplingCEMethodEnum> kJoinSamplingCEMethod{
    kInternalJoinOptimizationSamplingCEMethodName, &readGlobalValue<JoinSamplingCEMethod>};

}  // namespace mongo::query_knobs
