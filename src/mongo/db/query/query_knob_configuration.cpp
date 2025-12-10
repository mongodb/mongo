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

namespace mongo {

QueryKnobConfiguration::QueryKnobConfiguration(const query_settings::QuerySettings& querySettings) {
    _sbeDisableGroupPushdownValue =
        internalQuerySlotBasedExecutionDisableGroupPushdown.loadRelaxed();
    _sbeDisableLookupPushdownValue =
        internalQuerySlotBasedExecutionDisableLookupPushdown.loadRelaxed();
    _sbeDisableTimeSeriesValue =
        internalQuerySlotBasedExecutionDisableTimeSeriesPushdown.loadRelaxed();

    _queryFrameworkControlValue = querySettings.getQueryFramework().value_or_eval([]() {
        return ServerParameterSet::getNodeParameterSet()
            ->get<QueryFrameworkControl>("internalQueryFrameworkControl")
            ->_data.get();
    });

    _planRankerMode = ServerParameterSet::getNodeParameterSet()
                          ->get<QueryPlanRankerMode>("planRankerMode")
                          ->_data.get();

    _samplingConfidenceInterval =
        ServerParameterSet::getNodeParameterSet()
            ->get<SamplingConfidenceInterval>("samplingConfidenceInterval")
            ->_data.get();

    _samplingCEMethod = ServerParameterSet::getNodeParameterSet()
                            ->get<SamplingCEMethod>("internalQuerySamplingCEMethod")
                            ->_data.get();

    _numChunksForChunkBasedSampling = internalQueryNumChunksForChunkBasedSampling.load();
    _samplingMarginOfError = samplingMarginOfError.load();

    _sbeHashAggIncreasedSpillingMode =
        ServerParameterSet::getNodeParameterSet()
            ->get<SbeHashAggIncreasedSpillingMode>(
                "internalQuerySlotBasedExecutionHashAggIncreasedSpilling")
            ->_data.get();

    _planEvaluationMaxResults = internalQueryPlanEvaluationMaxResults.loadRelaxed();
    _plannerMaxIndexedSolutions = internalQueryPlannerMaxIndexedSolutions.loadRelaxed();
    _planEvaluationCollFraction = internalQueryPlanEvaluationCollFraction.load();
    _planTotalEvaluationCollFraction = internalQueryPlanTotalEvaluationCollFraction.load();
    _maxScansToExplodeValue = static_cast<size_t>(internalQueryMaxScansToExplode.loadRelaxed());
    _internalQuerySpillingMinAvailableDiskSpaceBytes =
        static_cast<int64_t>(internalQuerySpillingMinAvailableDiskSpaceBytes.loadRelaxed());
    _measureQueryExecutionTimeInNanoseconds =
        internalMeasureQueryExecutionTimeInNanoseconds.loadRelaxed();
    _useMultiplannerForSingleSolutions =
        internalQueryPlannerUseMultiplannerForSingleSolutions.loadRelaxed();

    _isJoinOrderingEnabled = internalEnableJoinOptimization.load();
    _randomJoinReorderDefaultToHashJoin = internalRandomJoinReorderDefaultToHashJoin.load();
    _randomJoinOrderSeed = internalRandomJoinOrderSeed.load();
    _joinReorderMode = ServerParameterSet::getNodeParameterSet()
                           ->get<JoinReorderMode>("internalJoinReorderMode")
                           ->_data.get();
    _joinPlanTreeShape = ServerParameterSet::getNodeParameterSet()
                             ->get<JoinPlanTreeShape>("internalJoinPlanTreeShape")
                             ->_data.get();
}

QueryFrameworkControlEnum QueryKnobConfiguration::getInternalQueryFrameworkControlForOp() const {
    return _queryFrameworkControlValue;
}

QueryPlanRankerModeEnum QueryKnobConfiguration::getPlanRankerMode() const {
    return _planRankerMode;
}

SamplingConfidenceIntervalEnum QueryKnobConfiguration::getConfidenceInterval() const {
    return _samplingConfidenceInterval;
}

SamplingCEMethodEnum QueryKnobConfiguration::getInternalQuerySamplingCEMethod() const {
    return _samplingCEMethod;
}

size_t QueryKnobConfiguration::getRandomJoinOrderSeed() const {
    return _randomJoinOrderSeed;
}

bool QueryKnobConfiguration::isJoinOrderingEnabled() const {
    return _isJoinOrderingEnabled;
}

bool QueryKnobConfiguration::getRandomJoinReorderDefaultToHashJoin() const {
    return _randomJoinReorderDefaultToHashJoin;
}

JoinReorderModeEnum QueryKnobConfiguration::getJoinReorderMode() const {
    return _joinReorderMode;
}

JoinPlanTreeShapeEnum QueryKnobConfiguration::getJoinPlanTreeShape() const {
    return _joinPlanTreeShape;
}

double QueryKnobConfiguration::getSamplingMarginOfError() const {
    return _samplingMarginOfError;
}

int64_t QueryKnobConfiguration::getNumChunksForChunkBasedSampling() const {
    return _numChunksForChunkBasedSampling;
}

SbeHashAggIncreasedSpillingModeEnum QueryKnobConfiguration::getSbeHashAggIncreasedSpillingMode()
    const {
    return _sbeHashAggIncreasedSpillingMode;
}


bool QueryKnobConfiguration::getSbeDisableGroupPushdownForOp() const {
    return _sbeDisableGroupPushdownValue;
}

bool QueryKnobConfiguration::getSbeDisableLookupPushdownForOp() const {
    return _sbeDisableLookupPushdownValue;
}

bool QueryKnobConfiguration::getSbeDisableTimeSeriesForOp() const {
    return _sbeDisableTimeSeriesValue;
}

bool QueryKnobConfiguration::isForceClassicEngineEnabled() const {
    return _queryFrameworkControlValue == QueryFrameworkControlEnum::kForceClassicEngine;
}

size_t QueryKnobConfiguration::getPlanEvaluationMaxResultsForOp() const {
    return _planEvaluationMaxResults;
}

size_t QueryKnobConfiguration::getPlannerMaxIndexedSolutions() const {
    return _plannerMaxIndexedSolutions;
}

double QueryKnobConfiguration::getPlanEvaluationCollFraction() const {
    return _planEvaluationCollFraction;
}

double QueryKnobConfiguration::getPlanTotalEvaluationCollFraction() const {
    return _planTotalEvaluationCollFraction;
}

size_t QueryKnobConfiguration::getMaxScansToExplodeForOp() const {
    return _maxScansToExplodeValue;
}

bool QueryKnobConfiguration::canPushDownFullyCompatibleStages() const {
    switch (_queryFrameworkControlValue) {
        case QueryFrameworkControlEnum::kForceClassicEngine:
        case QueryFrameworkControlEnum::kTrySbeRestricted:
            return false;
        case QueryFrameworkControlEnum::kTrySbeEngine:
            return true;
    }
    MONGO_UNREACHABLE;
}

int64_t QueryKnobConfiguration::getInternalQuerySpillingMinAvailableDiskSpaceBytes() const {
    return _internalQuerySpillingMinAvailableDiskSpaceBytes;
}

bool QueryKnobConfiguration::getMeasureQueryExecutionTimeInNanoseconds() const {
    return _measureQueryExecutionTimeInNanoseconds;
}

bool QueryKnobConfiguration::getUseMultiplannerForSingleSolutions() const {
    return _useMultiplannerForSingleSolutions;
}

}  // namespace mongo
