/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/time_support.h"

#include <utility>

namespace mongo {

/**
 * Namespace for helper functions related to time-series collections.
 */
namespace timeseries {

/**
 * Evaluates whether the timeseries bucket's options are fixed (unchanged).
 *
 * Returns true if `options.bucketRoundingSeconds` and `options.bucketMaxSpanSeconds` are equal and
 * the `parametersChanged` argument is `false`.
 */
bool areTimeseriesBucketsFixed(const TimeseriesOptions& options, bool parametersChanged);

/**
 * Evaluates whether the transition of timeseries granularities is valid (returning Status::OK if
 * the transition is acceptable) and if a pointer is given, it will be modified to reflect if the
 * options have changed.
 */
Status isTimeseriesGranularityValidAndUnchanged(const TimeseriesOptions& currentOptions,
                                                const CollModTimeseries& targetOptions,
                                                bool* shouldUpdateOptions = nullptr);

/**
 * Returns the default bucket timespan associated with the given granularity.
 */
int getMaxSpanSecondsFromGranularity(BucketGranularityEnum granularity);

StatusWith<std::pair<TimeseriesOptions, bool>> applyTimeseriesOptionsModifications(
    const TimeseriesOptions& current, const CollModTimeseries& mod);

BSONObj generateViewPipeline(const TimeseriesOptions& options, bool asArray);

bool optionsAreEqual(const TimeseriesOptions& option1, const TimeseriesOptions& option2);

/**
 * Returns the number of seconds used to round down the bucket ID and control.min timestamp.
 */
int getBucketRoundingSecondsFromGranularity(BucketGranularityEnum granularity);

/**
 * Rounds down timestamp to the specified granularity.
 */
Date_t roundTimestampToGranularity(const Date_t& time, const TimeseriesOptions& options);

/**
 * Rounds down timestamp by the specified seconds.
 */
Date_t roundTimestampBySeconds(const Date_t& time, long long roundingSeconds);
/**
 * Validates the combination of bucketRoundingSeconds, bucketMaxSpanSeconds and granularity in
 * TimeseriesOptions. If the parameters are not valid we return a bad status and if no parameters
 * are passed through we set them to their default values.
 */
Status validateAndSetBucketingParameters(TimeseriesOptions& timeseriesOptions);

/**
 * Validates the combination of bucketRoundingSeconds, bucketMaxSpanSeconds and granularity in
 * TimeseriesOptions. Returns a non-OK status if the options are not valid or if required parameters
 * are missing.
 */
Status validateBucketingParameters(const TimeseriesOptions&);
}  // namespace timeseries
}  // namespace mongo
