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
    auto [ptr, ec] = std::from_chars(idx.rawData(), idx.rawData() + idx.size(), value);
    // Ensure that the parsing consume the entire buffer
    if (ec != std::errc{} || ptr != idx.rawData() + idx.size()) {
        return INT_MIN;
    }
    return value;
}

/**
 * Validates an uncompressed bucket data object.
 */
void _validateUncompressedBucketData(const TimeseriesOptions& timeseriesOptions,
                                     const CollatorInterface* collator,
                                     const StringDataMap<BSONElement>& dataFields,
                                     const StringDataMap<BSONElement>& controlMinFields,
                                     const StringDataMap<BSONElement>& controlMaxFields) {
    auto it = dataFields.find(timeseriesOptions.getTimeField());
    uassert(ErrorCodes::BadValue,
            fmt::format("Field '{}' is missing from control.max", timeseriesOptions.getTimeField()),
            it != dataFields.end());

    it = controlMinFields.find(timeseriesOptions.getTimeField());
    uassert(ErrorCodes::BadValue,
            fmt::format("Field '{}' is missing from control.min", timeseriesOptions.getTimeField()),
            it != controlMinFields.end());

    it = controlMaxFields.find(timeseriesOptions.getTimeField());
    uassert(ErrorCodes::BadValue,
            fmt::format("Field '{}' is missing from control.max", timeseriesOptions.getTimeField()),
            it != controlMaxFields.end());
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
                                   const StringDataMap<BSONElement>& controlMaxFields) {
    for (auto&& data : dataFields) {
        auto it = controlMinFields.find(data.first);
        uassert(ErrorCodes::BadValue,
                fmt::format("Field '{}' is missing from control.min", data.first),
                it != controlMinFields.end());

        it = controlMaxFields.find(data.first);
        uassert(ErrorCodes::BadValue,
                fmt::format("Field '{}' is missing from control.max", data.first),
                it != controlMaxFields.end());

        uassert(ErrorCodes::BadValue,
                fmt::format("Invalid bucket data type. Expected binData, but got {}.",
                            data.second.type()),
                data.second.type() == BinData);

        BinDataType type = data.second.binDataType();
        uassert(ErrorCodes::BadValue,
                fmt::format("Invalid bucket data binData subtype. Expected 7, but got {}.", type),
                type == BinDataType::Column);
    }
}

}  // namespace


void validateBucketConsistency(const Collection* collection, const BSONObj& bucketDoc) {
    OID bucketId;
    try {
        // First perform some basic schema validation and extract elements to validate more
        // thoroughly.
        bucketId = bucketDoc[timeseries::kBucketIdFieldName].OID();

        auto timeseriesOptions = collection->getTimeseriesOptions().value();

        BSONObj control = bucketDoc[timeseries::kBucketControlFieldName].Obj();
        BSONObj data = bucketDoc[timeseries::kBucketDataFieldName].Obj();

        const int version = control.getIntField(timeseries::kBucketControlVersionFieldName);
        BSONObj min = control[timeseries::kBucketControlMinFieldName].Obj();
        BSONObj max = control[timeseries::kBucketControlMaxFieldName].Obj();

        if (version != timeseries::kTimeseriesControlUncompressedVersion &&
            version != timeseries::kTimeseriesControlCompressedVersion) {
            uasserted(
                ErrorCodes::BadValue,
                fmt::format("Invalid value for 'control.version'. Expected 1 or 2, but got {}.",
                            version));
        }

        // Perform the actual validation
        validateBucketIdTimestamp(timeseriesOptions, bucketId, min);

        validateBucketTimeSpan(timeseriesOptions, min, max);

        validateBucketData(timeseriesOptions,
                           collection->getDefaultCollator(),
                           version,
                           control[timeseries::kBucketControlCountFieldName],
                           min,
                           max,
                           data);
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
                               const BSONObj& controlMin) {
    // Ensure the time field exists
    const StringData timeField = timeseriesOptions.getTimeField();

    // Compares both timestamps as Dates.
    auto minTimestamp = controlMin[timeField].Date();
    auto oidEmbeddedTimestamp = id.asDateT();

    // If this bucket contains extended-range measurements, we cannot assert that the
    // minTimestamp matches the embedded timestamp.
    if (minTimestamp != oidEmbeddedTimestamp &&
        !timeseries::dateOutsideStandardRange(minTimestamp)) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Mismatch between the embedded timestamp "
                                << oidEmbeddedTimestamp.toString()
                                << " in the time-series bucket '_id' field and the timestamp "
                                << minTimestamp.toString() << " in 'control.min' field.");
    }
}

void validateBucketTimeSpan(const TimeseriesOptions& timeseriesOptions,
                            const BSONObj& controlMin,
                            const BSONObj& controlMax) {
    auto minTimestamp = controlMin[timeseriesOptions.getTimeField()].Date();
    auto maxTimestamp = controlMax[timeseriesOptions.getTimeField()].Date();
    auto bucketMaxSpanSeconds = timeseriesOptions.getBucketMaxSpanSeconds();
    if (maxTimestamp - minTimestamp >= Seconds(*bucketMaxSpanSeconds)) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "Time span of measurements in the bucket is too large. "
                                << "The difference between control.max and control.min is "
                                << (maxTimestamp - minTimestamp).toString()
                                << ", but the maximum allowed span is " << bucketMaxSpanSeconds
                                << " seconds.");
    }
}

void validateBucketData(const TimeseriesOptions& timeseriesOptions,
                        const CollatorInterface* collator,
                        int bucketVersion,
                        BSONElement controlCount,
                        const BSONObj& controlMin,
                        const BSONObj& controlMax,
                        const BSONObj& data) {

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
        _validateUncompressedBucketData(
            timeseriesOptions, collator, dataFields, controlMinFields, controlMaxFields);
    } else {
        int count = controlCount.numberInt();
        uassert(ErrorCodes::BadValue,
                "Unexpected control.count value, undefined integer representation",
                count == controlCount.safeNumberInt());
        uassert(
            ErrorCodes::BadValue,
            fmt::format("Unexpected control.count value. Expect at least 1, but got {}.", count),
            count > 0);
        _validateCompressedBucketData(timeseriesOptions,
                                      collator,
                                      bucketVersion,
                                      count,
                                      dataFields,
                                      controlMinFields,
                                      controlMaxFields);
    }
}
}  // namespace mongo::timeseries
