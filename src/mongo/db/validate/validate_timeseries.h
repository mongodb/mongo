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

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"

#include <string>
#include <string_view>

namespace mongo {

class CollatorInterface;
class CollectionPtr;
class OperationContext;
class TimeseriesOptions;
class ValidateResults;

namespace timeseries::bucket_catalog {
class MinMax;
}  // namespace timeseries::bucket_catalog

namespace collection_validation {

class ValidateState;

inline constexpr char kTimeseriesFixedBucketingInconsistencyReason[] =
    "A time series bucketing parameter was changed in this collection but fixedBucketing is true. "
    "For more info, see logs with log id 9175400.";
inline constexpr char kMalformedMinMaxTimeseriesBucket[] =
    "Detected a time-series bucket with malformed min/max values";
inline constexpr char kExpectedMixedSchemaTimeseriesWarning[] =
    "Detected a time-series bucket with mixed schema data";
inline constexpr char kUnexpectedMixedSchemaTimeseriesError[] =
    "Detected a time-series bucket with mixed schema data when "
    "timeseriesBucketsMayHaveMixedSchemaData is false. You can run the collMod command to set this "
    "flag";

/**
 * Enum returned by validateTimeseries* functions. Classifies timeseries-specific validation
 * results.
 */
enum class TimeseriesValidationResult {
    kValid,
    kIdMismatch,
    kBadVersion,
    kSpanViolation,
    kBadFieldCount,
    kTypeMismatch,
    kBadTimeType,
    kTimeIndexNotIncreasing,
    kTimeNotIncreasingForV2,
    kMissingTime,
    kV3WithOrderedTime,
    kInvalidBSONInTimeField,
    kMinMaxInconsistent,
    kBadCount,
    kWrongCount,
    kIndexNotIncreasing,
    kIndexOutOfRange,
    kIndexBadValue,
    kInvalidBSONInDataField,
    kExtendedRangeMismatch,
};

struct TimeseriesValidationStatus {
    TimeseriesValidationResult result;
    std::string reason;
};

/**
 * Returns a human-readable description for a given timeseries validation result.
 */
const char* describeTimeseriesValidationResult(TimeseriesValidationResult result);

/**
 * Checks that 'control.count' matches the actual number of measurements in a closed bucket.
 */
TimeseriesValidationStatus validateTimeseriesCount(const BSONObj& control,
                                                   int bucketCount,
                                                   int version);

/**
 * Checks if the embedded timestamp in the bucket id field matches that in the 'control.min' field.
 */
TimeseriesValidationStatus validateTimeSeriesIdTimestamp(OperationContext* opCtx,
                                                         const TimeseriesOptions& timeseriesOptions,
                                                         const BSONObj& recordBson);

/**
 * Checks the bucket's 'control' field to make sure the version is valid and the min max timestamps
 * respect the bucket max span.
 */
TimeseriesValidationStatus validateTimeseriesControlField(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& controlField);

/**
 * Checks if the bucket's version matches the types of 'data' fields.
 */
TimeseriesValidationStatus validateTimeseriesDataFieldTypes(const BSONElement& dataField,
                                                            int bucketVersion);

/**
 * Checks that the bucket's timestamps are consistent with the current bucketing parameters when
 * the collection has fixedBucketing=true (i.e., no bucketing parameter change has ever occurred).
 */
TimeseriesValidationStatus validateTimeseriesFixedBucketingConsistency(
    const CollectionPtr& coll,
    timeseries::bucket_catalog::MinMax& minmax,
    const BSONElement& controlMin,
    std::string_view fieldName,
    ValidateResults& results);

/**
 * Checks whether the min and max values between 'control' and 'data' match, taking timestamp
 * granularity into account.
 */
TimeseriesValidationStatus validateTimeSeriesMinMax(const TimeseriesOptions& timeseriesOptions,
                                                    timeseries::bucket_catalog::MinMax& minmax,
                                                    const BSONElement& controlMin,
                                                    const BSONElement& controlMax,
                                                    std::string_view fieldName,
                                                    int version,
                                                    const CollatorInterface* collator);

/**
 * Validates the indexes of the time field in the data field of a bucket. Checks the min and max
 * values match the ones in 'control' field. Counts the number of measurements.
 */
TimeseriesValidationStatus validateTimeSeriesDataTimeField(const CollectionPtr& coll,
                                                           const BSONElement& timeField,
                                                           const BSONElement& controlMin,
                                                           const BSONElement& controlMax,
                                                           std::string_view fieldName,
                                                           ValidateResults& results,
                                                           int version,
                                                           int* bucketCount);

/**
 * Validates the indexes of the data measurement fields of a bucket. Checks the min and max values
 * match the ones in 'control' field.
 */
TimeseriesValidationStatus validateTimeSeriesDataField(const CollectionPtr& coll,
                                                       const BSONElement& dataField,
                                                       const BSONElement& controlMin,
                                                       const BSONElement& controlMax,
                                                       std::string_view fieldName,
                                                       ValidateResults& results,
                                                       int version,
                                                       int bucketCount);

/**
 * Validates all 'data' fields of a bucket against the corresponding 'control' min and max fields.
 */
TimeseriesValidationStatus validateTimeSeriesDataFields(const CollectionPtr& coll,
                                                        const BSONObj& recordBson,
                                                        ValidateResults& results,
                                                        int bucketVersion);

/**
 * Checks for mismatches in the collection where an extended range timestamp exits in the bucket
 * control or _id block. This function should be called after schema validation as field presense is
 * not checked before access and exceptions will be thrown if the BSONObj does not contain the
 * expected field.
 */
TimeseriesValidationStatus validateTimeseriesExtendedRangeTimestamps(
    bool requiresExtendedRangeSupport, std::string_view timeFieldName, const BSONObj& recordBson);

/**
 * Validates the consistency of a time-series bucket.
 */
TimeseriesValidationStatus validateTimeSeriesBucketRecord(OperationContext* opCtx,
                                                          const ValidateState& validateState,
                                                          const CollectionPtr& coll,
                                                          const BSONObj& recordBson,
                                                          ValidateResults& results);

}  // namespace collection_validation
}  // namespace mongo
