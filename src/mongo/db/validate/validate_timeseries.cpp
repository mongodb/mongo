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


#include "mongo/db/validate/validate_timeseries.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/validate/validate_results.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/time_support.h"
#include "mongo/util/tracking/context.h"

#include <climits>
#include <string>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace CollectionValidation {

namespace {
/**
 * Attempts to parse the field name to integer.
 */
int idxInt(StringData idx) {
    int value = INT_MIN;
    auto [ptr, ec] = std::from_chars(idx.data(), idx.data() + idx.size(), value);
    // Ensure that the parsing consumes the entire buffer.
    if (ec != std::errc{} || ptr != idx.data() + idx.size()) {
        return INT_MIN;
    }
    return value;
}
}  // namespace

const char* describeTimeseriesValidationResult(TimeseriesValidationResult result) {
    switch (result) {
        case TimeseriesValidationResult::kValid:
            return "Valid";
        case TimeseriesValidationResult::kIdMismatch:
            return "Mismatch between the embedded timestamp in the time-series bucket '_id' field "
                   "and the timestamp in 'control.min' field. For more info, see logs with log id "
                   "6698300.";
        case TimeseriesValidationResult::kBadVersion:
            return "Invalid value for 'control.version'. For more info, see logs with log id "
                   "6698300.";
        case TimeseriesValidationResult::kSpanViolation:
            return "Bucket's timestamps in 'control.min' and 'control.max' fields do not respect "
                   "the bucket max span. For more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kBadFieldCount:
            return "Mismatch between the number of time-series control fields and the number of "
                   "data fields. For more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kTypeMismatch:
            return "Mismatch between time-series schema version and data field type. For more "
                   "info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kBadTimeType:
            return "Time-series bucket field is not a Date. For more info, see logs with log id "
                   "6698300.";
        case TimeseriesValidationResult::kTimeIndexNotIncreasing:
            return "The indexes in time-series bucket data fields are not consecutively increasing "
                   "from '0'. For more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kTimeNotIncreasingForV2:
            return "Time-series time values are not in ascending order. For more info, see logs "
                   "with log id 6698300.";
        case TimeseriesValidationResult::kMissingTime:
            return "Time-series bucket has missing time fields.";
        case TimeseriesValidationResult::kV3WithOrderedTime:
            return "Time-series bucket is v3 but has its measurements in-order on time.";
        case TimeseriesValidationResult::kInvalidBSONInTimeField:
            return "Invalid BSON In Time Field. For more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kMinMaxInconsistent:
            return "Mismatch between time-series control and observed min or max values. For more "
                   "info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kBadCount:
            return "Could not parse count value. For more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kWrongCount:
            return "The 'control.count' field does not match the actual number of "
                   "measurements in the document. For more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kIndexNotIncreasing:
            return "An index in time-series bucket data fields is not in increasing order. For "
                   "more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kIndexOutOfRange:
            return "An index in time-series bucket data fields is out of range. For more info, "
                   "see logs with log id 6698300.";
        case TimeseriesValidationResult::kIndexBadValue:
            return "An index in time-series bucket data fields is negative or non-numerical. For "
                   "more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kInvalidBSONInDataField:
            return "Invalid BSON In Data Field. For more info, see logs with log id 6698300.";
        case TimeseriesValidationResult::kExtendedRangeMismatch:
            return "An extended range timestamp was found in a collection without extended range "
                   "support. For more info, see logs with log id 6698300.";
    }

    MONGO_UNREACHABLE;
}

// Checks that 'control.count' matches the actual number of measurements in a closed bucket.
TimeseriesValidationStatus validateTimeseriesCount(const BSONObj& control,
                                                   int bucketCount,
                                                   int version) {
    if (version == timeseries::kTimeseriesControlUncompressedVersion) {
        return {TimeseriesValidationResult::kValid, ""};
    }
    long long controlCount;
    if (Status status = bsonExtractIntegerField(
            control, timeseries::kBucketControlCountFieldName, &controlCount);
        !status.isOK()) {
        return {TimeseriesValidationResult::kBadCount, status.toString()};
    }
    if (controlCount != bucketCount) {
        return {TimeseriesValidationResult::kWrongCount,
                fmt::format("The 'control.count' field ({}) does not match the actual number "
                            "of measurements in the document ({}).",
                            controlCount,
                            bucketCount)};
    }
    return {TimeseriesValidationResult::kValid, ""};
}

// Checks if the embedded timestamp in the bucket id field matches that in the 'control.min' field.
TimeseriesValidationStatus validateTimeSeriesIdTimestamp(OperationContext* opCtx,
                                                         const TimeseriesOptions& timeseriesOptions,
                                                         const BSONObj& recordBson) {
    // Ensure the time field exists
    const StringData timeField = timeseriesOptions.getTimeField();

    // Compares both timestamps as Dates.
    const auto minTimestamp = recordBson[timeseries::kBucketControlFieldName]
                                        [timeseries::kBucketControlMinFieldName][timeField]
                                            .Date();

    auto oidEmbeddedTimestamp = recordBson[timeseries::kBucketIdFieldName].OID().asDateT();

    // If this collection has extended-range measurements, we cannot assert that the
    // minTimestamp matches the embedded timestamp.
    if (minTimestamp != oidEmbeddedTimestamp &&
        !timeseries::dateOutsideStandardRange(minTimestamp)) {
        return {TimeseriesValidationResult::kIdMismatch,
                fmt::format("Mismatch between the embedded timestamp {} in the time-series "
                            "bucket '_id' field and the timestamp {} in 'control.min' field.",
                            oidEmbeddedTimestamp.toString(),
                            minTimestamp.toString())};
    }
    return {TimeseriesValidationResult::kValid, ""};
}

/**
 * Checks the bucket's 'control' field to make sure the version is valid and the min max timestamps
 * respect the bucket max span.
 */
TimeseriesValidationStatus validateTimeseriesControlField(
    const TimeseriesOptions& timeseriesOptions, const BSONObj& controlField) {
    int bucketVersion = controlField.getIntField(timeseries::kBucketControlVersionFieldName);
    if (bucketVersion != timeseries::kTimeseriesControlUncompressedVersion &&
        bucketVersion != timeseries::kTimeseriesControlCompressedSortedVersion &&
        bucketVersion != timeseries::kTimeseriesControlCompressedUnsortedVersion) {
        return {TimeseriesValidationResult::kBadVersion,
                fmt::format("Invalid value for 'control.version'. Expected 1, 2, or 3, but got {}.",
                            bucketVersion)};
    }

    const auto& minTimestamp = controlField.getField(timeseries::kBucketControlMinFieldName)
                                   .Obj()
                                   .getField(timeseriesOptions.getTimeField())
                                   .Date();
    const auto& maxTimestamp = controlField.getField(timeseries::kBucketControlMaxFieldName)
                                   .Obj()
                                   .getField(timeseriesOptions.getTimeField())
                                   .Date();
    const auto& bucketMaxSpanSeconds = timeseriesOptions.getBucketMaxSpanSeconds();
    if (maxTimestamp - minTimestamp >= Seconds(*bucketMaxSpanSeconds)) {
        return {
            TimeseriesValidationResult::kSpanViolation,
            fmt::format(
                "Bucket's timestamps in 'control.min' and 'control.max' fields do not respect the "
                "bucket max span. Min time: {}. Max time: {}. Bucket max span seconds: {}",
                minTimestamp.toString(),
                maxTimestamp.toString(),
                *bucketMaxSpanSeconds)};
    }

    return {TimeseriesValidationResult::kValid, ""};
}

/**
 * Checks if the bucket's version matches the types of 'data' fields.
 */
TimeseriesValidationStatus validateTimeseriesDataFieldTypes(const BSONElement& dataField,
                                                            int bucketVersion) {
    auto dataType = (bucketVersion == timeseries::kTimeseriesControlUncompressedVersion)
        ? BSONType::object
        : BSONType::binData;
    // Checks that open buckets have 'Object' type and closed buckets have 'BinData Column' type.
    auto isCorrectType = [&](BSONElement el) {
        if (bucketVersion == timeseries::kTimeseriesControlUncompressedVersion) {
            return el.type() == BSONType::object;
        } else {
            return el.type() == BSONType::binData && el.binDataType() == BinDataType::Column;
        }
    };

    if (!isCorrectType(dataField)) {
        return {TimeseriesValidationResult::kTypeMismatch,
                fmt::format("Mismatch between time-series schema version and data field "
                            "type. Expected type {}, but got {}.",
                            mongo::typeName(dataType),
                            mongo::typeName(dataField.type()))};
    }
    return {TimeseriesValidationResult::kValid, ""};
}


/*
 * Checks that only buckets that have timeSeriesBucketingParameters flag set have changed
 * bucket parameters.
 */
TimeseriesValidationStatus validateTimeseriesBucketingParametersChanged(
    const CollectionPtr& coll,
    timeseries::bucket_catalog::MinMax& minmax,
    const BSONElement& controlMin,
    StringData fieldName,
    ValidateResults& results,
    int version) {
    const bool timeseriesBucketingParametersHaveChanged =
        coll->timeseriesBucketingParametersHaveChanged().value_or(true);

    const auto min = minmax.min();

    auto checkTimeSeriesBucketingParametersChanged = [&]() {
        const auto options = coll->getTimeseriesOptions().value();
        auto roundedDownTimeStamp =
            timeseries::roundTimestampToGranularity(min.getField(fieldName).date(), options);
        return roundedDownTimeStamp < controlMin.Date();
    };

    if (checkTimeSeriesBucketingParametersChanged() && !timeseriesBucketingParametersHaveChanged &&
        results.addError(kTimeseriesBucketingParametersChangedInconsistencyReason)) {
        // Get the original timeseries bucketing parameters.
        auto originalTimeseriesOptions = coll->getTimeseriesOptions();
        invariant(originalTimeseriesOptions != boost::none);
        const auto& options = originalTimeseriesOptions.get();
        auto originalBucketMaxSpanSeconds = options.getBucketMaxSpanSeconds();
        auto originalBucketRoundingSeconds = options.getBucketRoundingSeconds();
        auto originalGranularity = options.getGranularity();

        LOGV2_ERROR_OPTIONS(9175400,
                            {logv2::LogTruncation::Disabled},
                            "A time series bucketing parameter was changed",
                            logAttrs(coll->ns()),
                            "currentBucketMaxSpanSeconds"_attr = originalBucketMaxSpanSeconds,
                            "currentBucketRoundingSeconds"_attr = originalBucketRoundingSeconds,
                            "currentGranularity"_attr = originalGranularity);
    }

    return {TimeseriesValidationResult::kValid, ""};
}

/**
 * Checks whether the min and max values between 'control' and 'data' match, taking timestamp
 * granularity into account.
 */
TimeseriesValidationStatus validateTimeSeriesMinMax(const TimeseriesOptions& timeseriesOptions,
                                                    timeseries::bucket_catalog::MinMax& minmax,
                                                    const BSONElement& controlMin,
                                                    const BSONElement& controlMax,
                                                    StringData fieldName,
                                                    int version,
                                                    const CollatorInterface* collator) {
    const auto min = minmax.min();
    const auto max = minmax.max();
    auto checkMinAndMaxMatch = [&]() {
        if (fieldName == timeseriesOptions.getTimeField()) {
            // With measurement-level deletes (deletes with non-metafield filters) it is possible
            // that the earliest measurements got deleted. Since we keep the bucket's minTime
            // unchanged in that case, we cannot rely on the minTime always corresponding with what
            // the actual minimum measurement time is. We can, however, rely on the fact that the
            // rounded time of the earliest measurement is at greater than or equal to the
            // control.min time-field.
            // TODO (SERVER-94872): Reinstate the strict equality check.
            auto minTimestampsMatch = timeseries::roundTimestampToGranularity(
                                          min.getField(fieldName).Date(), timeseriesOptions) >=
                timeseries::roundTimestampToGranularity(controlMin.Date(), timeseriesOptions);
            // For the maximum check, if we had measurements that were pre-1970 (the lower end of
            // the extended range check), it is possible that the control.max value gets rounded up
            // to the epoch and is greater than the observed maximum timestamp. In the case where
            // the control.min is earlier than the epoch, we should relax the check.
            auto maxTimestampsMatch = (min.getField(fieldName).Date() < Date_t())
                ? controlMax.Date() >= max.getField(fieldName).Date()
                : controlMax.Date() == max.getField(fieldName).Date();

            return minTimestampsMatch && maxTimestampsMatch;
        } else {
            // We cannot guarantee that the field order of the BSON objects will be the same.
            // The collation must match what the bucket catalog used when computing the
            // original min/max to avoid false mismatches.
            return controlMin.wrap().woCompare(min,
                                               /*ordering=*/BSONObj(),
                                               BSONObj::ComparisonRules::kConsiderFieldName |
                                                   BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                               collator) == 0 &&
                controlMax.wrap().woCompare(max,
                                            /*ordering=*/BSONObj(),
                                            BSONObj::ComparisonRules::kConsiderFieldName |
                                                BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                            collator) == 0;
        }
    };

    if (!checkMinAndMaxMatch()) {
        return {TimeseriesValidationResult::kMinMaxInconsistent,
                fmt::format(
                    "Mismatch between time-series control and observed min or max for field {}. "
                    "Control had min {} and max {}, but observed data had min {} and max {}.",
                    fieldName,
                    controlMin.toString(),
                    controlMax.toString(),
                    min.toString(),
                    max.toString())};
    }

    return {TimeseriesValidationResult::kValid, ""};
}

/**
 * Validates the indexes of the time field in the data field of a bucket. Checks the min and max
 * values match the ones in 'control' field. Counts the number of measurements.
 */
TimeseriesValidationStatus validateTimeSeriesDataTimeField(const CollectionPtr& coll,
                                                           const BSONElement& timeField,
                                                           const BSONElement& controlMin,
                                                           const BSONElement& controlMax,
                                                           StringData fieldName,
                                                           ValidateResults& results,
                                                           int version,
                                                           int* bucketCount) {
    tracking::Context trackingContext;
    timeseries::bucket_catalog::MinMax minmax{trackingContext};
    if (version == timeseries::kTimeseriesControlUncompressedVersion) {
        for (const auto& metric : timeField.Obj()) {
            if (metric.type() != BSONType::date) {
                return {TimeseriesValidationResult::kBadTimeType,
                        fmt::format("Time-series bucket {} field is not a Date", fieldName)};
            }
            // Checks that indices are consecutively increasing numbers starting from 0.
            if (auto idx = idxInt(metric.fieldNameStringData()); idx != *bucketCount) {
                return {
                    TimeseriesValidationResult::kTimeIndexNotIncreasing,
                    fmt::format("The indexes in time-series bucket data field '{}' is "
                                "not consecutively increasing from '0'. Expected: {}, but got: {}",
                                fieldName,
                                *bucketCount,
                                idx)};
            }
            // Time fields are not compared as strings so skip passing comparator.
            minmax.update(metric.wrap(fieldName), boost::none, /*stringComparator=*/nullptr);
            ++(*bucketCount);
        }
    } else {
        try {
            BSONColumn col{timeField};
            Date_t prevTimestamp = Date_t::min();
            bool detectedOutOfOrder = false;
            for (const auto& metric : col) {
                if (!metric.eoo()) {
                    if (metric.type() != BSONType::date) {
                        return {
                            TimeseriesValidationResult::kBadTimeType,
                            fmt::format("Time-series bucket '{}' field is not a Date", fieldName)};
                    }
                    // Checks the time values are sorted in increasing order for v2 buckets
                    // (compressed, sorted). Skip the check if the bucket is v3 (compressed,
                    // unsorted).
                    Date_t curTimestamp = metric.Date();
                    if (curTimestamp < prevTimestamp) {
                        if (version == timeseries::kTimeseriesControlCompressedSortedVersion) {
                            return {TimeseriesValidationResult::kTimeNotIncreasingForV2,
                                    fmt::format(
                                        "Time-series bucket '{}' field is not in ascending order",
                                        fieldName)};
                        } else if (version ==
                                   timeseries::kTimeseriesControlCompressedUnsortedVersion) {
                            detectedOutOfOrder = true;
                        }
                    }
                    prevTimestamp = curTimestamp;
                    // Time fields are not compared as strings so skip passing comparator.
                    minmax.update(
                        metric.wrap(fieldName), boost::none, /*stringComparator=*/nullptr);
                    ++(*bucketCount);
                } else {
                    return {TimeseriesValidationResult::kMissingTime,
                            "Time-series bucket has missing time fields"};
                }
            }
            if (version == timeseries::kTimeseriesControlCompressedUnsortedVersion &&
                !detectedOutOfOrder) {
                return {TimeseriesValidationResult::kV3WithOrderedTime,
                        "Time-series bucket is v3 but has its measurements in-order on time"};
            }
        } catch (DBException& e) {
            return {TimeseriesValidationResult::kInvalidBSONInTimeField,
                    str::stream() << "Exception occurred while decompressing a BSON column: "
                                  << e.toString()};
        }
    }
    // Time fields are not compared as strings so skip passing comparator.
    if (auto status = validateTimeSeriesMinMax(coll->getTimeseriesOptions().value(),
                                               minmax,
                                               controlMin,
                                               controlMax,
                                               fieldName,
                                               version,
                                               /*collator=*/nullptr);
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }
    if (auto status = validateTimeseriesBucketingParametersChanged(
            coll, minmax, controlMin, fieldName, results, version);
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    return {TimeseriesValidationResult::kValid, ""};
}

/**
 * Validates the indexes of the data measurement fields of a bucket. Checks the min and max values
 * match the ones in 'control' field.
 */
TimeseriesValidationStatus validateTimeSeriesDataField(const CollectionPtr& coll,
                                                       const BSONElement& dataField,
                                                       const BSONElement& controlMin,
                                                       const BSONElement& controlMax,
                                                       StringData fieldName,
                                                       ValidateResults& results,
                                                       int version,
                                                       int bucketCount) {
    tracking::Context trackingContext;
    timeseries::bucket_catalog::MinMax minmax{trackingContext};
    if (version == timeseries::kTimeseriesControlUncompressedVersion) {
        // Checks that indices are in increasing order and within the correct range.
        int prevIdx = INT_MIN;
        for (const auto& metric : dataField.Obj()) {
            auto idx = idxInt(metric.fieldNameStringData());
            if (idx <= prevIdx) {
                return {TimeseriesValidationResult::kIndexNotIncreasing,
                        fmt::format("The index '{}' in time-series bucket data field '{}' is "
                                    "not in increasing order",
                                    metric.fieldNameStringData(),
                                    fieldName)};
            }
            if (idx > bucketCount) {
                return {TimeseriesValidationResult::kIndexOutOfRange,
                        fmt::format("The index '{}' in time-series bucket data field '{}' is "
                                    "out of range",
                                    metric.fieldNameStringData(),
                                    fieldName)};
            }
            if (idx < 0) {
                return {TimeseriesValidationResult::kIndexBadValue,
                        fmt::format("The index '{}' in time-series bucket data field '{}' is "
                                    "negative or non-numerical",
                                    metric.fieldNameStringData(),
                                    fieldName)};
            }
            minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
            prevIdx = idx;
        }
    } else {
        try {
            BSONColumn col{dataField};
            for (const auto& metric : col) {
                if (!metric.eoo()) {
                    minmax.update(metric.wrap(fieldName), boost::none, coll->getDefaultCollator());
                }
            }
        } catch (DBException& e) {
            return {TimeseriesValidationResult::kInvalidBSONInDataField,
                    str::stream() << "Exception occurred while decompressing a BSON column: "
                                  << e.toString()};
        }
    }

    if (auto status = validateTimeSeriesMinMax(coll->getTimeseriesOptions().value(),
                                               minmax,
                                               controlMin,
                                               controlMax,
                                               fieldName,
                                               version,
                                               coll->getDefaultCollator());
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    return {TimeseriesValidationResult::kValid, ""};
}

TimeseriesValidationStatus validateTimeSeriesDataFields(const CollectionPtr& coll,
                                                        const BSONObj& recordBson,
                                                        ValidateResults& results,
                                                        int bucketVersion) {
    BSONObj data = recordBson.getField(timeseries::kBucketDataFieldName).Obj();
    BSONObj control = recordBson.getField(timeseries::kBucketControlFieldName).Obj();
    BSONObj controlMin = control.getField(timeseries::kBucketControlMinFieldName).Obj();
    BSONObj controlMax = control.getField(timeseries::kBucketControlMaxFieldName).Obj();

    // Builds a hash map for the fields to avoid repeated traversals.
    auto buildFieldTable = [&](StringMap<BSONElement>* table, const BSONObj& fields) {
        for (const auto& field : fields) {
            table->insert({std::string{field.fieldNameStringData()}, field});
        }
    };

    StringMap<BSONElement> dataFields;
    StringMap<BSONElement> controlMinFields;
    StringMap<BSONElement> controlMaxFields;
    buildFieldTable(&dataFields, data);
    buildFieldTable(&controlMinFields, controlMin);
    buildFieldTable(&controlMaxFields, controlMax);

    // Checks that the number of 'control.min' and 'control.max' fields agrees with number of 'data'
    // fields.
    if (dataFields.size() != controlMinFields.size() ||
        controlMinFields.size() != controlMaxFields.size()) {
        return {
            TimeseriesValidationResult::kBadFieldCount,
            fmt::format("Mismatch between the number of time-series control fields and the number "
                        "of data fields. Control had {} min fields and {} max fields, but observed "
                        "data had {} fields.",
                        controlMinFields.size(),
                        controlMaxFields.size(),
                        dataFields.size())};
    };

    // Validates the time field.
    int bucketCount = 0;
    const auto timeFieldName = coll->getTimeseriesOptions().value().getTimeField();
    if (auto status = validateTimeseriesDataFieldTypes(dataFields[timeFieldName], bucketVersion);
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    if (auto status = validateTimeSeriesDataTimeField(coll,
                                                      dataFields[timeFieldName],
                                                      controlMinFields[timeFieldName],
                                                      controlMaxFields[timeFieldName],
                                                      timeFieldName,
                                                      results,
                                                      bucketVersion,
                                                      &bucketCount);
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    if (auto status = validateTimeseriesCount(control, bucketCount, bucketVersion);
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    // Validates the other fields.
    for (const auto& [fieldName, dataField] : dataFields) {
        if (fieldName != timeFieldName) {
            if (auto status =
                    validateTimeseriesDataFieldTypes(dataFields[fieldName], bucketVersion);
                status.result != TimeseriesValidationResult::kValid) {
                return status;
            }

            if (auto status = validateTimeSeriesDataField(coll,
                                                          dataFields[fieldName],
                                                          controlMinFields[fieldName],
                                                          controlMaxFields[fieldName],
                                                          fieldName,
                                                          results,
                                                          bucketVersion,
                                                          bucketCount);
                status.result != TimeseriesValidationResult::kValid) {
                return status;
            }
        }
    }

    return {TimeseriesValidationResult::kValid, ""};
}

/**
 * Checks for mismatches in the collection where an extended range timestamp exits in the bucket
 * control or _id block. This function should be called after schema validation as field presense is
 * not checked before access and exceptions will be thrown if the BSONObj does not contain the
 * expected field.
 */
TimeseriesValidationStatus validateTimeseriesExtendedRangeTimestamps(
    bool requiresExtendedRangeSupport, StringData timeFieldName, const BSONObj& recordBson) {
    if (!requiresExtendedRangeSupport) {
        // Check the control block and _id fields for an extended timestamp and flag an error if
        // one exists while the collection metadata is not correctly set.

        std::vector<std::string> extendedRangeTimestampChecks;

        if (const auto oid = recordBson["_id"].OID(); timeseries::oidHasExtendedRangeTime(oid)) {
            extendedRangeTimestampChecks.push_back("_id");
        }
        if (const auto controlMinTimestamp = recordBson["control"]["min"][timeFieldName].Date();
            timeseries::dateOutsideStandardRange(controlMinTimestamp)) {
            extendedRangeTimestampChecks.push_back(fmt::format("control.min.{}", timeFieldName));
        }
        // Extended range support is required to use `control.min` for bucket date comparision
        // instead of `_id` if the date cannot be stored in 32-bits. `control.max` is not used to
        // determine if extended range support is required.

        if (!extendedRangeTimestampChecks.empty()) {
            return {TimeseriesValidationResult::kExtendedRangeMismatch,
                    fmt::format("[ {} ] contain{} extended range timestamps unsupported by the "
                                "collection metadata",
                                fmt::join(extendedRangeTimestampChecks, ", "),
                                extendedRangeTimestampChecks.size() == 1 ? "" : "s")};
        }
    }
    return {TimeseriesValidationResult::kValid, ""};
}

/**
 * Validates the consistency of a time-series bucket.
 */
TimeseriesValidationStatus validateTimeSeriesBucketRecord(OperationContext* opCtx,
                                                          const ValidateState& validateState,
                                                          const CollectionPtr& coll,
                                                          const BSONObj& recordBson,
                                                          ValidateResults& results) {
    const auto& timeseriesOptions = coll->getTimeseriesOptions().value();
    const auto controlElem = recordBson.getField(timeseries::kBucketControlFieldName);

    const int bucketVersion =
        controlElem.Obj().getIntField(timeseries::kBucketControlVersionFieldName);

    if (auto status = validateTimeSeriesIdTimestamp(opCtx, timeseriesOptions, recordBson);
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    if (auto status = validateTimeseriesControlField(timeseriesOptions, controlElem.Obj());
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    if (auto status = validateTimeSeriesDataFields(coll, recordBson, results, bucketVersion);
        status.result != TimeseriesValidationResult::kValid) {
        return status;
    }

    // TODO: SERVER-119598
    // Enable snapshot-based validation check
    if (!validateState.getReadTimestamp()) {
        if (auto status = validateTimeseriesExtendedRangeTimestamps(
                coll->getRequiresTimeseriesExtendedRangeSupport(),
                timeseriesOptions.getTimeField(),
                recordBson);
            status.result != TimeseriesValidationResult::kValid) {
            return status;
        }
    }

    return {TimeseriesValidationResult::kValid, ""};
}

}  // namespace CollectionValidation
}  // namespace mongo
