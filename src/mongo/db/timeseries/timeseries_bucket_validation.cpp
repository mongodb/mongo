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

#include "mongo/db/timeseries/timeseries_bucket_validation.h"

#include "mongo/bson/column/bsoncolumn_expressions.h"
#include "mongo/bson/column/bsoncolumn_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"

#include <charconv>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::timeseries {
namespace {
/**
 * Performs a rate limited log of an exception. Maximum 10 logs every 10 seconds are allowed.
 */
void logExceptionRateLimited(const DBException& ex) {
    // Atomics to implement lockless log rate limiting
    static AtomicWord<int64_t> lastLogTime{std::numeric_limits<int64_t>::min()};
    static AtomicWord<int64_t> numErrorsSinceAdvanceLogTime{0};
    static AtomicWord<int64_t> numErrorsTotal{0};

    // Keep track on how many times we've hit this in total.
    auto total = numErrorsTotal.addAndFetch(1);

    // Perform a logging every 10 seconds
    auto now = Date_t::now();
    if (now > Date_t::fromMillisSinceEpoch(lastLogTime.load()) + Seconds(10)) {
        // Update number of logs we've done and check if we should advance the time
        int64_t numErrorsSinceTimeUpdate = numErrorsSinceAdvanceLogTime.addAndFetch(1);
        if (numErrorsSinceTimeUpdate > 10) {
            lastLogTime.store(now.toMillisSinceEpoch());
        }

        // Perform log. This is internally serialized so important that we have a
        // backoff.
        LOGV2_WARNING(11898500,
                      "Strict timeseries bucket validation failed",
                      "total"_attr = total,
                      "error"_attr = ex);
    }
}

/**
 * Attempts to parse the field name to integer.
 */
int _idxInt(StringData idx) {
    int value = INT_MIN;
    auto [ptr, ec] = std::from_chars(idx.data(), idx.data() + idx.size(), value);
    // Ensure that the parsing consume the entire buffer
    if (ec != std::errc{} || ptr != idx.data() + idx.size()) {
        return INT_MIN;
    }
    return value;
}

/**
 * Validates an uncompressed column against expected min/max.
 */
void _validateUncompressedMinMax(StringData fieldName,
                                 BSONElement data,
                                 BSONElement min,
                                 BSONElement max,
                                 int maxCount,
                                 const CollatorInterface* collator) {
    // The MinMax type calculates min/max for both scalars and nested objects where the results
    // needs to be a merged element-wise min/max.
    tracking::Context trackingContext;
    timeseries::bucket_catalog::MinMax minmax{trackingContext};

    // Checks that indices are in increasing order and within the correct range.
    int prevIdx = INT_MIN;
    for (const auto& metric : data.Obj()) {
        auto idx = _idxInt(metric.fieldNameStringData());

        uassert(ErrorCodes::BadValue,
                fmt::format("The index '{}' in time-series bucket data field '{}' is "
                            "not in increasing order",
                            metric.fieldNameStringData(),
                            fieldName),
                idx > prevIdx);

        uassert(ErrorCodes::BadValue,
                fmt::format("The index '{}' in time-series bucket data field '{}' is "
                            "out of range",
                            metric.fieldNameStringData(),
                            fieldName),
                idx <= maxCount);

        uassert(ErrorCodes::BadValue,
                fmt::format("The index '{}' in time-series bucket data field '{}' is "
                            "negative or non-numerical",
                            metric.fieldNameStringData(),
                            fieldName),
                idx >= 0);

        minmax.update(metric.wrap(fieldName), boost::none, collator);
        prevIdx = idx;
    }

    uassert(ErrorCodes::BadValue,
            fmt::format("Incorrect column data min for field '{}'. Data min '{}' is different "
                        "than control.min '{}'.",
                        fieldName,
                        minmax.min().toString(),
                        min.wrap().toString()),
            minmax.min().woCompare(min.wrap(),
                                   /*ordering=*/BSONObj(),
                                   BSONObj::ComparisonRules::kConsiderFieldName |
                                       BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                   collator) == 0);

    uassert(ErrorCodes::BadValue,
            fmt::format("Incorrect column data max for field '{}'. Data min '{}' is different "
                        "than control.max '{}'.",
                        fieldName,
                        minmax.max().toString(),
                        max.wrap().toString()),
            minmax.max().woCompare(max.wrap(),
                                   /*ordering=*/BSONObj(),
                                   BSONObj::ComparisonRules::kConsiderFieldName |
                                       BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                   collator) == 0);
}

/**
 * Validates a compressed column against expected count and min/max.
 */
void _validateCompressedMinMax(boost::intrusive_ptr<BSONElementStorage>& allocator,
                               StringData fieldName,
                               BSONElement data,
                               BSONElement min,
                               BSONElement max,
                               int expectedCount,
                               const CollatorInterface* collator,
                               bool criticalValidationOnly) {
    uassert(ErrorCodes::BadValue,
            fmt::format("Invalid bucket data type. Expected binData, but got {}.", data.type()),
            data.type() == BSONType::binData);

    int len = 0;
    const char* binary = data.binData(len);
    BinDataType type = data.binDataType();
    uassert(ErrorCodes::BadValue,
            fmt::format("Invalid bucket data binData subtype. Expected 7, but got {}.", type),
            type == BinDataType::Column);

    // Disable remaining validation if critical-only is set.
    if (criticalValidationOnly) {
        return;
    }

    // All columns should have the count as stored in the control object.
    size_t cnt = bsoncolumn::count(binary, len);
    uassert(ErrorCodes::BadValue,
            fmt::format("Incorrect column data count for field '{}'. Expected {}, but found {}.",
                        fieldName,
                        expectedCount,
                        cnt),
            expectedCount == static_cast<int>(cnt));

    // Scalar types can use a basic BSON ordering to calculate min/max. However objects/arrays
    // stores element-wise min/max in the control block where the data is merged from the entire
    // column content.
    if (min.type() == BSONType::object || min.type() == BSONType::array ||
        max.type() == BSONType::object || max.type() == BSONType::array) {
        tracking::Context trackingContext;
        timeseries::bucket_catalog::MinMax minmax{trackingContext};

        // Decompress the column and calculate element-wise merged min/max for this column.
        for (auto&& elem : BSONColumn(binary, len)) {
            if (!elem.eoo()) {
                minmax.update(elem.wrap(fieldName), boost::none, collator);
            }
        }

        uassert(ErrorCodes::BadValue,
                fmt::format("Incorrect column data min for field '{}'. Data min '{}' is different "
                            "than control.min '{}'.",
                            fieldName,
                            minmax.min().toString(),
                            min.wrap().toString()),
                minmax.min().woCompare(min.wrap(),
                                       /*ordering=*/BSONObj(),
                                       BSONObj::ComparisonRules::kConsiderFieldName |
                                           BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                       collator) == 0);

        uassert(ErrorCodes::BadValue,
                fmt::format("Incorrect column data max for field '{}'. Data min '{}' is different "
                            "than control.max '{}'.",
                            fieldName,
                            minmax.max().toString(),
                            max.wrap().toString()),
                minmax.max().woCompare(max.wrap(),
                                       /*ordering=*/BSONObj(),
                                       BSONObj::ComparisonRules::kConsiderFieldName |
                                           BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                       collator) == 0);

    } else {
        // Scalar types can use a fast-path to calculate min/max from the compressed column directly
        // without materializing the entire content.
        auto minmaxElems = bsoncolumn::minmax<bsoncolumn::BSONElementMaterializer>(
            binary, len, allocator, collator);

        uassert(ErrorCodes::BadValue,
                fmt::format("Incorrect column data min for field '{}'. Data min '{}' is different "
                            "than control.min '{}'.",
                            fieldName,
                            minmaxElems.first.toString(),
                            min.toString()),
                minmaxElems.first.woCompare(
                    min, BSONObj::ComparisonRules::kIgnoreFieldOrder, collator) == 0);

        uassert(ErrorCodes::BadValue,
                fmt::format("Incorrect column data max for field '{}'. Data min '{}' is different "
                            "than control.max '{}'.",
                            fieldName,
                            minmaxElems.second.toString(),
                            max.toString()),
                minmaxElems.second.woCompare(
                    max, BSONObj::ComparisonRules::kIgnoreFieldOrder, collator) == 0);
    }
}

/**
 * Validates an uncompressed time column against bucket _id, bucket time span and expected min/max.
 * Returns the element count.
 */
int _validateUncompressedTimeField(const TimeseriesOptions& timeseriesOptions,
                                   BSONElement data,
                                   BSONElement min,
                                   BSONElement max,
                                   const CollatorInterface* collator) {
    tracking::Context trackingContext;
    timeseries::bucket_catalog::MinMax minmax{trackingContext};

    int cnt = 0;
    for (const auto& metric : data.Obj()) {
        // Checks that indices are consecutively increasing numbers starting from 0.
        auto idx = _idxInt(metric.fieldNameStringData());

        uassert(ErrorCodes::BadValue,
                fmt::format("Time-series time field '{}' is not in consecutively increasing order. "
                            "Got index '{}' when '{}' is expected.",
                            metric.fieldNameStringData(),
                            idx,
                            cnt),
                idx == cnt);

        minmax.update(metric.wrap(timeseriesOptions.getTimeField()), boost::none, collator);
        ++cnt;
    }

    // With measurement-level deletes (deletes with non-metafield filters) it is possible
    // that the earliest measurements got deleted. Since we keep the bucket's minTime
    // unchanged in that case, we cannot rely on the minTime always corresponding with what
    // the actual minimum measurement time is. We can, however, rely on the fact that the
    // rounded time of the earliest measurement is at greater than or equal to the
    // control.min time-field.
    auto minTimestampsMatch =
        timeseries::roundTimestampToGranularity(
            minmax.min().getField(timeseriesOptions.getTimeField()).Date(), timeseriesOptions) >=
        timeseries::roundTimestampToGranularity(min.Date(), timeseriesOptions);
    // For the maximum check, if we had measurements that were pre-1970 (the lower end of
    // the extended range check), it is possible that the control.max value gets rounded up
    // to the epoch and is greater than the observed maximum timestamp. In the case where
    // the control.min is earlier than the epoch, we should relax the check.
    auto maxTimestampsMatch =
        (minmax.min().getField(timeseriesOptions.getTimeField()).Date() < Date_t())
        ? max.Date() >= minmax.max().getField(timeseriesOptions.getTimeField()).Date()
        : max.Date() == minmax.max().getField(timeseriesOptions.getTimeField()).Date();

    uassert(
        ErrorCodes::BadValue,
        fmt::format("Mismatch between time-series control and observed min or max for field {}. "
                    "Control had min {} and max {}, but observed data had min {} and max {}.",
                    timeseriesOptions.getTimeField(),
                    min.toString(),
                    max.toString(),
                    minmax.min().toString(),
                    minmax.max().toString()),
        minTimestampsMatch && maxTimestampsMatch);

    return cnt;
}

/**
 * Validates a compressed time column against bucket _id, bucket time span, expected count and
 * min/max.
 */
void _validateCompressedTimeField(boost::intrusive_ptr<BSONElementStorage>& allocator,
                                  const TimeseriesOptions& timeseriesOptions,
                                  BSONElement data,
                                  BSONElement min,
                                  BSONElement max,
                                  int expectedCount,
                                  const CollatorInterface* collator,
                                  bool criticalValidationOnly) {
    int len = 0;
    const char* binary = data.binData(len);
    BinDataType type = data.binDataType();
    uassert(ErrorCodes::BadValue,
            fmt::format("Invalid bucket data binData subtype. Expected 7, but got {}.", type),
            type == BinDataType::Column);

    // Disable remaining validation if critical-only is set.
    if (criticalValidationOnly) {
        return;
    }

    size_t cnt = bsoncolumn::count(binary, len);
    uassert(ErrorCodes::BadValue,
            fmt::format("Incorrect column data count for field '{}'. Expected {}, but found {}.",
                        timeseriesOptions.getTimeField(),
                        expectedCount,
                        cnt),
            expectedCount == static_cast<int>(cnt));

    // Time field is always a scalar so we can use fast BSON comparison to calculate min/max.
    auto minmaxElems =
        bsoncolumn::minmax<bsoncolumn::BSONElementMaterializer>(binary, len, allocator, collator);

    // With measurement-level deletes (deletes with non-metafield filters) it is possible
    // that the earliest measurements got deleted. Since we keep the bucket's minTime
    // unchanged in that case, we cannot rely on the minTime always corresponding with what
    // the actual minimum measurement time is. We can, however, rely on the fact that the
    // rounded time of the earliest measurement is at greater than or equal to the
    // control.min time-field.
    auto minTimestampsMatch =
        timeseries::roundTimestampToGranularity(minmaxElems.first.Date(), timeseriesOptions) >=
        timeseries::roundTimestampToGranularity(min.Date(), timeseriesOptions);
    // For the maximum check, if we had measurements that were pre-1970 (the lower end of
    // the extended range check), it is possible that the control.max value gets rounded up
    // to the epoch and is greater than the observed maximum timestamp. In the case where
    // the control.min is earlier than the epoch, we should relax the check.
    auto maxTimestampsMatch = (minmaxElems.first.Date() < Date_t())
        ? max.Date() >= minmaxElems.second.Date()
        : max.Date() == minmaxElems.second.Date();

    uassert(
        ErrorCodes::BadValue,
        fmt::format("Mismatch between time-series control and observed min or max for field {}. "
                    "Control had min {} and max {}, but observed data had min {} and max {}.",
                    timeseriesOptions.getTimeField(),
                    min.toString(),
                    max.toString(),
                    minmaxElems.first.toString(),
                    minmaxElems.second.toString()),
        minTimestampsMatch && maxTimestampsMatch);
}

/**
 * Validates an uncompressed bucket data object.
 */
void _validateUncompressedBucketData(const TimeseriesOptions& timeseriesOptions,
                                     const CollatorInterface* collator,
                                     const StringDataMap<BSONElement>& dataFields,
                                     const StringDataMap<BSONElement>& controlMinFields,
                                     const StringDataMap<BSONElement>& controlMaxFields,
                                     bool criticalValidationOnly) {
    auto it = dataFields.find(timeseriesOptions.getTimeField());
    uassert(ErrorCodes::BadValue,
            fmt::format("Field '{}' is missing from control.max", timeseriesOptions.getTimeField()),
            it != dataFields.end());
    BSONElement time = it->second;

    it = controlMinFields.find(timeseriesOptions.getTimeField());
    uassert(ErrorCodes::BadValue,
            fmt::format("Field '{}' is missing from control.min", timeseriesOptions.getTimeField()),
            it != controlMinFields.end());
    BSONElement min = it->second;

    it = controlMaxFields.find(timeseriesOptions.getTimeField());
    uassert(ErrorCodes::BadValue,
            fmt::format("Field '{}' is missing from control.max", timeseriesOptions.getTimeField()),
            it != controlMaxFields.end());
    BSONElement max = it->second;

    // Disable remaining validation if critical-only is set.
    if (criticalValidationOnly) {
        return;
    }

    // Validate the time column first, we use this to discover the count to validate the other
    // columns with.
    int count = _validateUncompressedTimeField(timeseriesOptions, time, min, max, collator);

    for (auto&& data : dataFields) {
        // Time field is already validated.
        if (data.first == timeseriesOptions.getTimeField()) {
            continue;
        }

        it = controlMinFields.find(data.first);
        uassert(ErrorCodes::BadValue,
                fmt::format("Field '{}' is missing from control.min", data.first),
                it != controlMinFields.end());
        BSONElement min = it->second;

        it = controlMaxFields.find(data.first);
        uassert(ErrorCodes::BadValue,
                fmt::format("Field '{}' is missing from control.max", data.first),
                it != controlMaxFields.end());
        BSONElement max = it->second;

        // Validate this data field.
        _validateUncompressedMinMax(data.first, data.second, min, max, count, collator);
    }
}

/**
 * Validates an uncompressed bucket data object.
 */
void _validateCompressedBucketData(const TimeseriesOptions& timeseriesOptions,
                                   const CollatorInterface* collator,
                                   const int bucketVersion,
                                   const int bucketCount,
                                   const StringDataMap<BSONElement>& dataFields,
                                   const StringDataMap<BSONElement>& controlMinFields,
                                   const StringDataMap<BSONElement>& controlMaxFields,
                                   bool criticalValidationOnly) {
    boost::intrusive_ptr allocator{new BSONElementStorage()};
    for (auto&& data : dataFields) {
        auto it = controlMinFields.find(data.first);
        uassert(ErrorCodes::BadValue,
                fmt::format("Field '{}' is missing from control.min", data.first),
                it != controlMinFields.end());
        BSONElement min = it->second;

        it = controlMaxFields.find(data.first);
        uassert(ErrorCodes::BadValue,
                fmt::format("Field '{}' is missing from control.max", data.first),
                it != controlMaxFields.end());
        BSONElement max = it->second;

        if (data.first == timeseriesOptions.getTimeField()) {
            _validateCompressedTimeField(allocator,
                                         timeseriesOptions,
                                         data.second,
                                         min,
                                         max,
                                         bucketCount,
                                         collator,
                                         criticalValidationOnly);
        } else {
            _validateCompressedMinMax(allocator,
                                      data.first,
                                      data.second,
                                      min,
                                      max,
                                      bucketCount,
                                      collator,
                                      criticalValidationOnly);
        }
    }
}

}  // namespace


void validateBucketConsistency(const Collection* collection, const BSONObj& bucketDoc) {
    OID bucketId;
    try {
        bool criticalValidationOnly = gTimeseriesLessStrictBucketValidator.load();
        // First perform some basic schema validation and extract elements to validate more
        // thoroughly.
        bucketId = bucketDoc[timeseries::kBucketIdFieldName].OID();

        const auto& timeseriesOptions = collection->getTimeseriesOptions().value();

        BSONObj control = bucketDoc[timeseries::kBucketControlFieldName].Obj();
        BSONObj data = bucketDoc[timeseries::kBucketDataFieldName].Obj();

        const int version = control.getIntField(timeseries::kBucketControlVersionFieldName);
        BSONObj min = control[timeseries::kBucketControlMinFieldName].Obj();
        BSONObj max = control[timeseries::kBucketControlMaxFieldName].Obj();

        if (version != timeseries::kTimeseriesControlUncompressedVersion &&
            version != timeseries::kTimeseriesControlCompressedSortedVersion &&
            version != timeseries::kTimeseriesControlCompressedUnsortedVersion) {
            uasserted(
                ErrorCodes::BadValue,
                fmt::format("Invalid value for 'control.version'. Expected 1, 2, or 3, but got {}.",
                            version));
        }

        // Perform the actual validation
        validateBucketIdTimestamp(timeseriesOptions, bucketId, min, criticalValidationOnly);

        validateBucketTimeSpan(timeseriesOptions,
                               collection->areTimeseriesBucketsFixed(),
                               min,
                               max,
                               criticalValidationOnly);

        validateBucketData(timeseriesOptions,
                           collection->getDefaultCollator(),
                           version,
                           control[timeseries::kBucketControlCountFieldName],
                           min,
                           max,
                           data,
                           criticalValidationOnly);
    } catch (DBException& ex) {
        // Catch any validation error and attach extra context to be able to debug validation errors
        // or remediate corrupt buckets
        ex.addContext(fmt::format("Bucket _id: {}", bucketId.toString()));
        // Perform logging of occurances. This is rate limited to protect against malicious use.
        logExceptionRateLimited(ex);
        throw;
    }
}

void validateBucketIdTimestamp(const TimeseriesOptions& timeseriesOptions,
                               const OID& id,
                               const BSONObj& controlMin,
                               bool criticalValidationOnly) {
    // Ensure the time field exists
    const StringData timeField = timeseriesOptions.getTimeField();

    // Compares both timestamps as Dates.
    auto minTimestamp = controlMin[timeField].Date();
    auto oidEmbeddedTimestamp = id.asDateT();

    // If this bucket contains extended-range measurements, we cannot assert that the
    // minTimestamp matches the embedded timestamp.
    if (minTimestamp != oidEmbeddedTimestamp &&
        !timeseries::dateOutsideStandardRange(minTimestamp) && !criticalValidationOnly) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Mismatch between the embedded timestamp "
                                << oidEmbeddedTimestamp.toString()
                                << " in the time-series bucket '_id' field and the timestamp "
                                << minTimestamp.toString() << " in 'control.min' field.");
    }
}

void validateBucketTimeSpan(const TimeseriesOptions& timeseriesOptions,
                            bool fixedBucketingEnabled,
                            const BSONObj& controlMin,
                            const BSONObj& controlMax,
                            bool criticalValidationOnly) {
    auto minTimestamp = controlMin[timeseriesOptions.getTimeField()].Date();
    auto maxTimestamp = controlMax[timeseriesOptions.getTimeField()].Date();
    auto bucketMaxSpanSeconds = timeseriesOptions.getBucketMaxSpanSeconds();
    if (maxTimestamp - minTimestamp >= Seconds(*bucketMaxSpanSeconds) && !criticalValidationOnly) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Time span of measurements in the bucket is too large. "
                                << "The difference between control.max and control.min is "
                                << (maxTimestamp - minTimestamp).toString()
                                << ", but the maximum allowed span is " << bucketMaxSpanSeconds
                                << " seconds.");
    }

    // Enforce that control.min time is aligned to the fixed bucket boundary when the
    // fixed-bucketing optimization is enabled.
    if (fixedBucketingEnabled && !criticalValidationOnly) {
        auto expectedMinTimestamp = roundTimestampToGranularity(minTimestamp, timeseriesOptions);
        uassert(ErrorCodes::BadValue,
                fmt::format("control.min.{} is not rounded to expected boundary when "
                            "fixed-bucketing is enabled. Expected {}, but got {}.",
                            timeseriesOptions.getTimeField(),
                            minTimestamp.toString(),
                            expectedMinTimestamp.toString()),
                minTimestamp == expectedMinTimestamp);
    }
}

void validateBucketData(const TimeseriesOptions& timeseriesOptions,
                        const CollatorInterface* collator,
                        int bucketVersion,
                        BSONElement controlCount,
                        const BSONObj& controlMin,
                        const BSONObj& controlMax,
                        const BSONObj& data,
                        bool criticalValidationOnly) {

    // Builds a hash map for the fields to avoid repeated traversals.
    auto buildFieldTable = [](StringDataMap<BSONElement>& table, const BSONObj& fields) {
        for (const auto& field : fields) {
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Duplicate field '" << field.fieldNameStringData()
                                  << "' detected in bucket.",
                    table.try_emplace(field.fieldNameStringData(), field).second);
        }
    };

    StringDataMap<BSONElement> dataFields;
    StringDataMap<BSONElement> controlMinFields;
    StringDataMap<BSONElement> controlMaxFields;
    buildFieldTable(dataFields, data);
    buildFieldTable(controlMinFields, controlMin);
    buildFieldTable(controlMaxFields, controlMax);

    // Checks that the number of 'control.min' and 'control.max' fields agrees with number of 'data'
    // fields.
    if (dataFields.size() != controlMinFields.size() ||
        controlMinFields.size() != controlMaxFields.size()) {
        uasserted(
            ErrorCodes::BadValue,
            fmt::format("Mismatch between the number of time-series control fields and the number "
                        "of data fields. Control had {} min fields and {} max fields, but observed "
                        "data had {} fields.",
                        controlMinFields.size(),
                        controlMaxFields.size(),
                        dataFields.size()));
    };

    if (bucketVersion == timeseries::kTimeseriesControlUncompressedVersion) {
        _validateUncompressedBucketData(timeseriesOptions,
                                        collator,
                                        dataFields,
                                        controlMinFields,
                                        controlMaxFields,
                                        criticalValidationOnly);
    } else {
        int count = controlCount.numberInt();
        uassert(ErrorCodes::BadValue,
                "Unexpected control.count value, undefined integer representation",
                count == controlCount.safeNumberInt());
        _validateCompressedBucketData(timeseriesOptions,
                                      collator,
                                      bucketVersion,
                                      count,
                                      dataFields,
                                      controlMinFields,
                                      controlMaxFields,
                                      criticalValidationOnly);
    }
}
}  // namespace mongo::timeseries
