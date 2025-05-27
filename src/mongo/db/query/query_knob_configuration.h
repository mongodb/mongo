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

#pragma once

#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

namespace mongo {
/**
 * A class for query-related knobs, it sets all the knob values on the first time a knob is accessed
 * and ensures the values are same though the whole lifetime of a query.
 */
class QueryKnobConfiguration {
public:
    /**
     * NOTE: QueryKnobConfiguration construction requires 'querySettings', because settings may
     * override the query knob values.
     */
    QueryKnobConfiguration(const query_settings::QuerySettings& querySettings);

    QueryFrameworkControlEnum getInternalQueryFrameworkControlForOp() const;
    QueryPlanRankerModeEnum getPlanRankerMode() const;
    SamplingConfidenceIntervalEnum getConfidenceInterval() const;
    SamplingCEMethodEnum getInternalQuerySamplingCEMethod() const;
    SbeHashAggIncreasedSpillingModeEnum getSbeHashAggIncreasedSpillingMode() const;

    bool getSbeDisableGroupPushdownForOp() const;
    bool getSbeDisableLookupPushdownForOp() const;
    bool getSbeDisableTimeSeriesForOp() const;

    /**
     * Returns true if internal query framework control knob is set to 'forceClassicEngine', false
     * otherwise.
     */
    bool isForceClassicEngineEnabled() const;
    size_t getPlanEvaluationMaxResultsForOp() const;
    size_t getPlannerMaxIndexedSolutions() const;
    double getPlanEvaluationCollFraction() const;
    double getPlanTotalEvaluationCollFraction() const;
    size_t getMaxScansToExplodeForOp() const;

    /**
     * Returns whether we can push down fully compatible stages to sbe. This is only true when the
     * query knob is 'trySbeEngine'.
     */
    bool canPushDownFullyCompatibleStages() const;

    int64_t getInternalQuerySpillingMinAvailableDiskSpaceBytes() const;

private:
    QueryFrameworkControlEnum _queryFrameworkControlValue;
    QueryPlanRankerModeEnum _planRankerMode;
    SamplingConfidenceIntervalEnum _samplingConfidenceInterval;
    SamplingCEMethodEnum _samplingCEMethod;
    SbeHashAggIncreasedSpillingModeEnum _sbeHashAggIncreasedSpillingMode;
    size_t _planEvaluationMaxResults;
    size_t _plannerMaxIndexedSolutions;
    double _planEvaluationCollFraction;
    double _planTotalEvaluationCollFraction;
    size_t _maxScansToExplodeValue;
    bool _sbeDisableGroupPushdownValue;
    bool _sbeDisableLookupPushdownValue;
    bool _sbeDisableTimeSeriesValue;
    int64_t _internalQuerySpillingMinAvailableDiskSpaceBytes;
};
}  // namespace mongo
