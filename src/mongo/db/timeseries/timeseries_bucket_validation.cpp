// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/timeseries_bucket_validation.h"

#include "mongo/bson/column/bsoncolumn_expressions.h"
#include "mongo/bson/column/bsoncolumn_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_extended_range.h"
#include "mongo/db/timeseries/timeseries_options.h"

#include <charconv>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::timeseries {

using BCV = BucketConsistencyViolation;

namespace {
/**
 * Performs a rate limited log of a validation failure. Maximum 10 logs every 10 seconds are
 * allowed.
 */
void logViolationRateLimited(BCV violation, std::string_view reason) {
    // Atomics to implement lockless log rate limiting
    static Atomic<int64_t> lastLogTime{std::numeric_limits<int64_t>::min()};
    static Atomic<int64_t> numErrorsSinceAdvanceLogTime{0};
    static Atomic<int64_t> numErrorsTotal{0};

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
                      "violation"_attr = static_cast<int>(violation),
                      "reason"_attr = reason);
    }
}

/**
 * Returns true if 'ex' is one of the errors that a corrupt bucket document is expected to produce,
 * and which therefore may be reported as kInvalidBsonData. Anything else (interruption, resource
 * exhaustion, a programming error, ...) is unrelated to the contents of the bucket and must be
 * propagated rather than reported as corruption.
 *
 * The expected errors are:
 *  - 10065/13111: BSONElement accessors (.Obj(), .Date(), .OID()) on an absent or wrong-typed
 *    field.
 *  - 12602100/12602101: FlatBSON/MinMax rejecting a duplicate field name.
 *  - InvalidBSON/InvalidBSONColumn and the 90956xx range: malformed compressed column data, thrown
 *    by BSONColumn decompression and by the bsoncolumn::count()/minmax() expressions.
 */
bool isExpectedBucketCorruptionError(const DBException& ex) {
    const auto code = static_cast<int32_t>(ex.code());
    switch (code) {
        case 10065:
        case 13111:
        case 12602100:
        case 12602101:
            return true;
        default:
            break;
    }
    if (ex.code() == ErrorCodes::InvalidBSON || ex.code() == ErrorCodes::InvalidBSONColumn) {
        return true;
    }
    // Assertion codes reserved for the bsoncolumn expression implementations.
    return code >= 9095600 && code <= 9095699;
}

/**
 * Attempts to parse the field name to integer.
 */
int _idxInt(std::string_view idx) {
    int value = INT_MIN;
    auto [ptr, ec] = std::from_chars(idx.data(), idx.data() + idx.size(), value);
    // Ensure that the parsing consumes the entire buffer.
    if (ec != std::errc{} || ptr != idx.data() + idx.size()) {
        return INT_MIN;
    }
    return value;
}

/**
 * Validates an uncompressed column against expected min/max.
 */
BCV _validateUncompressedMinMax(std::string_view fieldName,
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

        if (idx <= prevIdx) {
            return BCV::kIndexNotIncreasing;
        }
        if (idx > maxCount) {
            return BCV::kIndexOutOfRange;
        }
        if (idx < 0) {
            return BCV::kIndexBadValue;
        }

        minmax.update(metric.wrap(fieldName), boost::none, collator);
        prevIdx = idx;
    }

    if (minmax.min().woCompare(min.wrap(),
                               /*ordering=*/BSONObj(),
                               BSONObj::ComparisonRules::kConsiderFieldName |
                                   BSONObj::ComparisonRules::kIgnoreFieldOrder,
                               collator) != 0) {
        return BCV::kMinMaxMismatch;
    }

    if (minmax.max().woCompare(max.wrap(),
                               /*ordering=*/BSONObj(),
                               BSONObj::ComparisonRules::kConsiderFieldName |
                                   BSONObj::ComparisonRules::kIgnoreFieldOrder,
                               collator) != 0) {
        return BCV::kMinMaxMismatch;
    }

    return BCV::kNone;
}

/**
 * Validates a compressed column against expected count and min/max.
 */
BCV _validateCompressedMinMax(boost::intrusive_ptr<BSONElementStorage>& allocator,
                              std::string_view fieldName,
                              BSONElement data,
                              BSONElement min,
                              BSONElement max,
                              int expectedCount,
                              const CollatorInterface* collator,
                              bool criticalValidationOnly) {
    if (data.type() != BSONType::binData) {
        return BCV::kBadDataType;
    }

    int len = 0;
    const char* binary = data.binData(len);
    BinDataType type = data.binDataType();
    if (type != BinDataType::Column) {
        return BCV::kBadBinDataSubtype;
    }

    // Disable remaining validation if critical-only is set.
    if (criticalValidationOnly) {
        return BCV::kNone;
    }

    try {
        // All columns should have the count as stored in the control object.
        size_t cnt = bsoncolumn::count(binary, len);
        if (expectedCount != static_cast<int>(cnt)) {
            return BCV::kCountMismatch;
        }

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

            if (minmax.min().woCompare(min.wrap(),
                                       /*ordering=*/BSONObj(),
                                       BSONObj::ComparisonRules::kConsiderFieldName |
                                           BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                       collator) != 0) {
                return BCV::kMinMaxMismatch;
            }

            if (minmax.max().woCompare(max.wrap(),
                                       /*ordering=*/BSONObj(),
                                       BSONObj::ComparisonRules::kConsiderFieldName |
                                           BSONObj::ComparisonRules::kIgnoreFieldOrder,
                                       collator) != 0) {
                return BCV::kMinMaxMismatch;
            }
        } else {
            // Scalar types can use a fast-path to calculate min/max from the compressed column
            // directly without materializing the entire content.
            auto [minElem, maxElem] = bsoncolumn::minmax<bsoncolumn::BSONElementMaterializer>(
                binary, len, allocator, collator);

            if (minElem.woCompare(min, BSONObj::ComparisonRules::kIgnoreFieldOrder, collator) !=
                0) {
                return BCV::kMinMaxMismatch;
            }

            if (maxElem.woCompare(max, BSONObj::ComparisonRules::kIgnoreFieldOrder, collator) !=
                0) {
                return BCV::kMinMaxMismatch;
            }
        }
    } catch (const DBException& ex) {
        if (!isExpectedBucketCorruptionError(ex)) {
            throw;
        }
        return BCV::kInvalidBsonData;
    }

    return BCV::kNone;
}

/**
 * Validates an uncompressed time column against bucket _id, bucket time span and expected min/max.
 * Stores the element count in *outCount on success.
 */
BCV _validateUncompressedTimeField(const TimeseriesOptions& timeseriesOptions,
                                   BSONElement data,
                                   BSONElement min,
                                   BSONElement max,
                                   const CollatorInterface* collator,
                                   int* outCount) {
    tracking::Context trackingContext;
    timeseries::bucket_catalog::MinMax minmax{trackingContext};

    int cnt = 0;
    for (const auto& metric : data.Obj()) {
        // Checks that indices are consecutively increasing numbers starting from 0.
        auto idx = _idxInt(metric.fieldNameStringData());
        if (idx != cnt) {
            return BCV::kIndexNotIncreasing;
        }

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

    if (!minTimestampsMatch || !maxTimestampsMatch) {
        return BCV::kMinMaxMismatch;
    }

    *outCount = cnt;
    return BCV::kNone;
}

/**
 * Validates a compressed time column against bucket _id, bucket time span, expected count and
 * min/max.
 */
BCV _validateCompressedTimeField(boost::intrusive_ptr<BSONElementStorage>& allocator,
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
    if (type != BinDataType::Column) {
        return BCV::kBadBinDataSubtype;
    }

    // Disable remaining validation if critical-only is set.
    if (criticalValidationOnly) {
        return BCV::kNone;
    }

    try {
        size_t cnt = bsoncolumn::count(binary, len);
        if (expectedCount != static_cast<int>(cnt)) {
            return BCV::kCountMismatch;
        }

        // Time field is always a scalar so we can use fast BSON comparison to calculate min/max.
        auto [minElem, maxElem] = bsoncolumn::minmax<bsoncolumn::BSONElementMaterializer>(
            binary, len, allocator, collator);

        // With measurement-level deletes (deletes with non-metafield filters) it is possible
        // that the earliest measurements got deleted. Since we keep the bucket's minTime
        // unchanged in that case, we cannot rely on the minTime always corresponding with what
        // the actual minimum measurement time is. We can, however, rely on the fact that the
        // rounded time of the earliest measurement is at greater than or equal to the
        // control.min time-field.
        auto minTimestampsMatch =
            timeseries::roundTimestampToGranularity(minElem.Date(), timeseriesOptions) >=
            timeseries::roundTimestampToGranularity(min.Date(), timeseriesOptions);
        // For the maximum check, if we had measurements that were pre-1970 (the lower end of
        // the extended range check), it is possible that the control.max value gets rounded up
        // to the epoch and is greater than the observed maximum timestamp. In the case where
        // the control.min is earlier than the epoch, we should relax the check.
        auto maxTimestampsMatch = (minElem.Date() < Date_t()) ? max.Date() >= maxElem.Date()
                                                              : max.Date() == maxElem.Date();

        if (!minTimestampsMatch || !maxTimestampsMatch) {
            return BCV::kMinMaxMismatch;
        }
    } catch (const DBException& ex) {
        if (!isExpectedBucketCorruptionError(ex)) {
            throw;
        }
        return BCV::kInvalidBsonData;
    }

    return BCV::kNone;
}

/**
 * Validates an uncompressed bucket data object.
 */
BCV _validateUncompressedBucketData(const TimeseriesOptions& timeseriesOptions,
                                    const CollatorInterface* collator,
                                    const StringDataMap<BSONElement>& dataFields,
                                    const StringDataMap<BSONElement>& controlMinFields,
                                    const StringDataMap<BSONElement>& controlMaxFields,
                                    bool criticalValidationOnly) {
    auto it = dataFields.find(timeseriesOptions.getTimeField());
    if (it == dataFields.end()) {
        return BCV::kMissingTimeField;
    }
    BSONElement time = it->second;

    it = controlMinFields.find(timeseriesOptions.getTimeField());
    if (it == controlMinFields.end()) {
        return BCV::kMissingField;
    }
    BSONElement min = it->second;

    it = controlMaxFields.find(timeseriesOptions.getTimeField());
    if (it == controlMaxFields.end()) {
        return BCV::kMissingField;
    }
    BSONElement max = it->second;

    // Disable remaining validation if critical-only is set.
    if (criticalValidationOnly) {
        return BCV::kNone;
    }

    // Validate the time column first, we use this to discover the count to validate the other
    // columns with.
    int count = 0;
    if (auto v =
            _validateUncompressedTimeField(timeseriesOptions, time, min, max, collator, &count);
        v != BCV::kNone) {
        return v;
    }

    for (auto&& data : dataFields) {
        // Time field is already validated.
        if (data.first == timeseriesOptions.getTimeField()) {
            continue;
        }

        it = controlMinFields.find(data.first);
        if (it == controlMinFields.end()) {
            return BCV::kMissingField;
        }
        BSONElement dataMin = it->second;

        it = controlMaxFields.find(data.first);
        if (it == controlMaxFields.end()) {
            return BCV::kMissingField;
        }
        BSONElement dataMax = it->second;

        if (auto v = _validateUncompressedMinMax(
                data.first, data.second, dataMin, dataMax, count, collator);
            v != BCV::kNone) {
            return v;
        }
    }

    return BCV::kNone;
}

/**
 * Validates a compressed bucket data object.
 */
BCV _validateCompressedBucketData(const TimeseriesOptions& timeseriesOptions,
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
        if (it == controlMinFields.end()) {
            return BCV::kMissingField;
        }
        BSONElement min = it->second;

        it = controlMaxFields.find(data.first);
        if (it == controlMaxFields.end()) {
            return BCV::kMissingField;
        }
        BSONElement max = it->second;

        BCV v;
        if (data.first == timeseriesOptions.getTimeField()) {
            v = _validateCompressedTimeField(allocator,
                                             timeseriesOptions,
                                             data.second,
                                             min,
                                             max,
                                             bucketCount,
                                             collator,
                                             criticalValidationOnly);
        } else {
            v = _validateCompressedMinMax(allocator,
                                          data.first,
                                          data.second,
                                          min,
                                          max,
                                          bucketCount,
                                          collator,
                                          criticalValidationOnly);
        }
        if (v != BCV::kNone) {
            return v;
        }
    }

    return BCV::kNone;
}

}  // namespace


BCV validateBucketConsistency(const Collection* collection, const BSONObj& bucketDoc) {
    const bool criticalValidationOnly = gTimeseriesLessStrictBucketValidator.load();
    OID bucketId;
    // BSON field accessors (.OID(), .Obj(), .Date(), etc.) throw DBException when a field is
    // absent or the wrong type. We catch such structural errors here and report them as
    // corruption; any other error is rethrown rather than misreported as a bad bucket.
    try {
        // Extract top-level fields first so bucketId is available for error logging.
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
            logViolationRateLimited(
                BCV::kBadVersion,
                fmt::format("Invalid value for 'control.version'. Expected 1, 2, or 3, but got {}. "
                            "Bucket _id: {}",
                            version,
                            bucketId.toString()));
            return BCV::kBadVersion;
        }

        // Perform the actual validation. BSON accessor calls inside sub-functions (e.g.
        // .Date() on the time field) are also covered by the outer catch.
        auto checkAndLog = [&](BCV violation, std::string_view context) -> BCV {
            if (violation != BCV::kNone) {
                logViolationRateLimited(
                    violation, fmt::format("{}. Bucket _id: {}", context, bucketId.toString()));
            }
            return violation;
        };

        if (auto v = checkAndLog(
                validateBucketIdTimestamp(timeseriesOptions, bucketId, min, criticalValidationOnly),
                "validateBucketIdTimestamp");
            v != BCV::kNone) {
            return v;
        }

        if (auto v = checkAndLog(
                validateBucketTimeSpan(timeseriesOptions, min, max, criticalValidationOnly),
                "validateBucketTimeSpan");
            v != BCV::kNone) {
            return v;
        }

        if (auto v =
                checkAndLog(validateBucketData(timeseriesOptions,
                                               collection->getDefaultCollator(),
                                               version,
                                               control[timeseries::kBucketControlCountFieldName],
                                               min,
                                               max,
                                               data,
                                               criticalValidationOnly),
                            "validateBucketData");
            v != BCV::kNone) {
            return v;
        }
    } catch (const DBException& ex) {
        if (!isExpectedBucketCorruptionError(ex)) {
            throw;
        }
        logViolationRateLimited(
            BCV::kInvalidBsonData,
            fmt::format("{}. Bucket _id: {}", ex.toStatus().reason(), bucketId.toString()));
        return BCV::kInvalidBsonData;
    }

    return BCV::kNone;
}

BCV validateBucketIdTimestamp(const TimeseriesOptions& timeseriesOptions,
                              const OID& id,
                              const BSONObj& controlMin,
                              bool criticalValidationOnly) {
    // Ensure the time field exists
    const std::string_view timeField = timeseriesOptions.getTimeField();

    // Compares both timestamps as Dates.
    auto minTimestamp = controlMin[timeField].Date();
    auto oidEmbeddedTimestamp = id.asDateT();

    // If this bucket contains extended-range measurements, we cannot assert that the
    // minTimestamp matches the embedded timestamp.
    if (minTimestamp != oidEmbeddedTimestamp &&
        !timeseries::dateOutsideStandardRange(minTimestamp) && !criticalValidationOnly) {
        return BCV::kIdTimestampMismatch;
    }

    return BCV::kNone;
}

BCV validateBucketTimeSpan(const TimeseriesOptions& timeseriesOptions,
                           const BSONObj& controlMin,
                           const BSONObj& controlMax,
                           bool criticalValidationOnly) {
    auto minTimestamp = controlMin[timeseriesOptions.getTimeField()].Date();
    // Only 'minTimestamp' needs to be checked for extended range here: the sole check gated by
    // 'fixedBucketingEnabled' below just validates that 'control.min' is aligned to the fixed
    // bucket boundary.
    const bool fixedBucketingEnabled = canUseFixedBucketOptimizations(
        timeseriesOptions, timeseries::dateOutsideStandardRange(minTimestamp));
    auto maxTimestamp = controlMax[timeseriesOptions.getTimeField()].Date();
    auto bucketMaxSpanSeconds = timeseriesOptions.getBucketMaxSpanSeconds();
    if (maxTimestamp - minTimestamp >= Seconds(*bucketMaxSpanSeconds) && !criticalValidationOnly) {
        return BCV::kTimeSpanTooLarge;
    }

    // Enforce that control.min time is aligned to the fixed bucket boundary when the
    // fixed-bucketing optimization is enabled.
    if (fixedBucketingEnabled && !criticalValidationOnly) {
        auto expectedMinTimestamp = roundTimestampToGranularity(minTimestamp, timeseriesOptions);
        if (minTimestamp != expectedMinTimestamp) {
            return BCV::kMinTimeNotRounded;
        }
    }

    return BCV::kNone;
}

BCV validateBucketData(const TimeseriesOptions& timeseriesOptions,
                       const CollatorInterface* collator,
                       int bucketVersion,
                       BSONElement controlCount,
                       const BSONObj& controlMin,
                       const BSONObj& controlMax,
                       const BSONObj& data,
                       bool criticalValidationOnly) {

    // Builds a hash map for the fields to avoid repeated traversals. Returns kNone on success or
    // kDuplicateField if a field name appears more than once.
    auto buildFieldTable = [](StringDataMap<BSONElement>& table, const BSONObj& fields) -> BCV {
        for (const auto& field : fields) {
            if (!table.try_emplace(field.fieldNameStringData(), field).second) {
                return BCV::kDuplicateField;
            }
        }
        return BCV::kNone;
    };

    StringDataMap<BSONElement> dataFields;
    StringDataMap<BSONElement> controlMinFields;
    StringDataMap<BSONElement> controlMaxFields;
    if (auto v = buildFieldTable(dataFields, data); v != BCV::kNone) {
        return v;
    }
    if (auto v = buildFieldTable(controlMinFields, controlMin); v != BCV::kNone) {
        return v;
    }
    if (auto v = buildFieldTable(controlMaxFields, controlMax); v != BCV::kNone) {
        return v;
    }

    // Checks that the number of 'control.min' and 'control.max' fields agrees with number of 'data'
    // fields.
    if (dataFields.size() != controlMinFields.size() ||
        controlMinFields.size() != controlMaxFields.size()) {
        return BCV::kFieldCountMismatch;
    }

    if (bucketVersion == timeseries::kTimeseriesControlUncompressedVersion) {
        return _validateUncompressedBucketData(timeseriesOptions,
                                               collator,
                                               dataFields,
                                               controlMinFields,
                                               controlMaxFields,
                                               criticalValidationOnly);
    } else {
        int count = controlCount.numberInt();
        if (count != controlCount.safeNumberInt()) {
            return BCV::kBadControlCount;
        }
        return _validateCompressedBucketData(timeseriesOptions,
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
