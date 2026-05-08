/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_knob_configuration.h"

#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_knob_descriptors_optimization.h"
#include "mongo/db/query/query_knob_registry.h"
#include "mongo/db/query/query_knob_snapshot.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

namespace mongo {

namespace {

/**
 * Returns a new snapshot initialized from the current global knob values, with any supported
 * per-query QuerySettings overrides applied on top.
 */
QueryKnobSnapshot makeQueryKnobSnapshot(const query_settings::QuerySettings& querySettings) {
    auto&& registry = QueryKnobRegistry::instance();
    QueryKnobSnapshotBuilder builder(registry.knobCount());

    // Load the global knob values.
    for (auto&& entry : registry.entries()) {
        const auto& knob = entry.knob;
        builder.set(knob.id, knob.readGlobal(), KnobSource::kDefault);
    }

    // Apply query settings overrides if needed.
    if (auto queryFramework = querySettings.getQueryFramework()) {
        QueryKnobValue queryFrameworkValue(static_cast<int>(*queryFramework));
        builder.set(query_knobs::kQueryFrameworkControl.id,
                    std::move(queryFrameworkValue),
                    KnobSource::kQuerySettings);
    }
    return std::move(builder).build();
}
}  // namespace

QueryKnobConfiguration::QueryKnobConfiguration(const query_settings::QuerySettings& querySettings)
    : _snapshot(makeQueryKnobSnapshot(querySettings)) {}

QueryFrameworkControlEnum QueryKnobConfiguration::getInternalQueryFrameworkControlForOp() const {
    return get(query_knobs::kQueryFrameworkControl);
}

QueryPlanRankerModeEnum QueryKnobConfiguration::getPlanRankerMode() const {
    return get(query_knobs::kPlanRankerMode);
}

QueryPlanRankingStrategyForAutomaticQueryPlanRankerModeEnum
QueryKnobConfiguration::getPlanRankingStrategyForAutomaticQueryPlanRankerMode() const {
    return get(query_knobs::kPlanRankingStrategyForAutomaticQueryPlanRankerMode);
}

SamplingConfidenceIntervalEnum QueryKnobConfiguration::getConfidenceInterval() const {
    return get(query_knobs::kSamplingConfidenceInterval);
}

SamplingCEMethodEnum QueryKnobConfiguration::getInternalQuerySamplingCEMethod() const {
    return get(query_knobs::kSamplingCEMethod);
}

size_t QueryKnobConfiguration::getRandomJoinOrderSeed() const {
    return static_cast<size_t>(get(query_knobs::kRandomJoinOrderSeed));
}

bool QueryKnobConfiguration::isJoinOrderingEnabled() const {
    return get(query_knobs::kEnableJoinOptimization);
}

JoinReorderModeEnum QueryKnobConfiguration::getJoinReorderMode() const {
    return get(query_knobs::kJoinReorderMode);
}

JoinPlanTreeShapeEnum QueryKnobConfiguration::getJoinPlanTreeShape() const {
    return get(query_knobs::kJoinPlanTreeShape);
}

size_t QueryKnobConfiguration::getMaxNodesInJoinGraph() const {
    return static_cast<size_t>(get(query_knobs::kMaxNodesInJoinGraph));
}

size_t QueryKnobConfiguration::getMaxEdgesInJoinGraph() const {
    return static_cast<size_t>(get(query_knobs::kMaxEdgesInJoinGraph));
}

size_t QueryKnobConfiguration::getMaxNumberNodesConsideredForImplicitEdges() const {
    return static_cast<size_t>(get(query_knobs::kMaxNumberNodesConsideredForImplicitEdges));
}

bool QueryKnobConfiguration::getEnableJoinEnumerationHJOrderPruning() const {
    return get(query_knobs::kEnableJoinEnumerationHJOrderPruning);
}

ForcedJoinMethodEnum QueryKnobConfiguration::getJoinMethod() const {
    return get(query_knobs::kJoinMethod);
}

size_t QueryKnobConfiguration::getInternalJoinPlanSamplingSize() const {
    return static_cast<size_t>(get(query_knobs::kJoinPlanSamplingSize));
}

bool QueryKnobConfiguration::getInternalJoinEnumerateCollScanPlans() const {
    return get(query_knobs::kJoinEnumerateCollScanPlans);
}

size_t QueryKnobConfiguration::getInternalMinAllPlansEnumerationSubsetLevel() const {
    return static_cast<size_t>(get(query_knobs::kMinAllPlansEnumerationSubsetLevel));
}

size_t QueryKnobConfiguration::getInternalMaxAllPlansEnumerationSubsetLevel() const {
    return static_cast<size_t>(get(query_knobs::kMaxAllPlansEnumerationSubsetLevel));
}

bool QueryKnobConfiguration::getEnableJoinOptimizationUseIndexUniqueness() const {
    return get(query_knobs::kEnableJoinOptimizationUseIndexUniqueness);
}

double QueryKnobConfiguration::getSamplingMarginOfError() const {
    return get(query_knobs::kSamplingMarginOfError);
}

int64_t QueryKnobConfiguration::getNumChunksForChunkBasedSampling() const {
    return get(query_knobs::kNumChunksForChunkBasedSampling);
}

SbeHashAggIncreasedSpillingModeEnum QueryKnobConfiguration::getSbeHashAggIncreasedSpillingMode()
    const {
    return get(query_knobs::kSbeHashAggIncreasedSpillingMode);
}


bool QueryKnobConfiguration::getSbeDisableGroupPushdownForOp() const {
    return get(query_knobs::kSbeDisableGroupPushdown);
}

bool QueryKnobConfiguration::getSbeDisableLookupPushdownForOp() const {
    return get(query_knobs::kSbeDisableLookupPushdown);
}

bool QueryKnobConfiguration::getSbeDisableTimeSeriesForOp() const {
    return get(query_knobs::kSbeDisableTimeSeriesPushdown);
}

bool QueryKnobConfiguration::isForceClassicEngineEnabled() const {
    return get(query_knobs::kQueryFrameworkControl) ==
        QueryFrameworkControlEnum::kForceClassicEngine;
}

size_t QueryKnobConfiguration::getPlanEvaluationMaxResultsForOp() const {
    return static_cast<size_t>(get(query_knobs::kPlanEvaluationMaxResults));
}

size_t QueryKnobConfiguration::getPlannerMaxIndexedSolutions() const {
    return static_cast<size_t>(get(query_knobs::kPlannerMaxIndexedSolutions));
}

double QueryKnobConfiguration::getPlanEvaluationCollFraction() const {
    return get(query_knobs::kPlanEvaluationCollFraction);
}

double QueryKnobConfiguration::getPlanTotalEvaluationCollFraction() const {
    return get(query_knobs::kPlanTotalEvaluationCollFraction);
}

size_t QueryKnobConfiguration::getMaxScansToExplodeForOp() const {
    return static_cast<size_t>(get(query_knobs::kMaxScansToExplode));
}

bool QueryKnobConfiguration::canPushDownFullyCompatibleStages() const {
    switch (get(query_knobs::kQueryFrameworkControl)) {
        case QueryFrameworkControlEnum::kForceClassicEngine:
        case QueryFrameworkControlEnum::kTrySbeRestricted:
            return false;
        case QueryFrameworkControlEnum::kTrySbeEngine:
            return true;
    }
    MONGO_UNREACHABLE;
}

int64_t QueryKnobConfiguration::getInternalQuerySpillingMinAvailableDiskSpaceBytes() const {
    return get(query_knobs::kSpillingMinAvailableDiskSpaceBytes);
}

bool QueryKnobConfiguration::getMeasureQueryExecutionTimeInNanoseconds() const {
    return get(query_knobs::kMeasureQueryExecutionTimeInNanoseconds);
}

bool QueryKnobConfiguration::getUseMultiplannerForSingleSolutions() const {
    return get(query_knobs::kPlannerUseMultiplannerForSingleSolutions);
}

int64_t QueryKnobConfiguration::getMaxGroupAccumulatorsInSbe() const {
    return get(query_knobs::kMaxGroupAccumulatorsInSbe);
}

bool QueryKnobConfiguration::getEnablePathArrayness() const {
    return get(query_knobs::kEnablePathArrayness);
}

bool QueryKnobConfiguration::getEnablePipelineOptimizationAdditionalTestingRules() const {
    return get(query_knobs::kEnablePipelineOptimizationAdditionalTestingRules);
}

SamplingCEMethodEnum QueryKnobConfiguration::getInternalJoinOptimizationSamplingCEMethod() const {
    return get(query_knobs::kJoinSamplingCEMethod);
}

}  // namespace mongo
