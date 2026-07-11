// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <utility>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

/**
 * Evaluates whether fixed-bucket query optimizations can be used.
 *
 * Returns true if fixed-bucket query optimizations are enabled (via
 * featureFlagFixedBucketingOptimizations) and applicable to the specified timeseries options.
 */
bool canUseFixedBucketOptimizations(const TimeseriesOptions& options);

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
 * Defaults the 'fixedBucketing' option to true for a NEW viewless time-series collection when the
 * user omitted it. No-op when 'fixedBucketingEnabled' (featureFlagFixedBucketingCatalog) is false,
 * or when the field was set explicitly (an explicit false is preserved). Only call for viewless
 * collections; the caller is responsible for the viewless check.
 */
void setFixedBucketingDefaultForNewCollection(TimeseriesOptions& timeseriesOptions,
                                              bool fixedBucketingEnabled);

/**
 * Inherits the 'fixedBucketing' value from 'existing' into 'requested' if the field is absent
 * in 'requested'.
 */
void inheritFixedBucketingIfOmitted(TimeseriesOptions& requested,
                                    const TimeseriesOptions& existing);

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
}  // namespace mongo::timeseries
