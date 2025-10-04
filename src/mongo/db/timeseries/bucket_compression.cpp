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


#include "mongo/db/timeseries/bucket_compression.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/bson/column/bsoncolumnbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/write_ops/measurement.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace timeseries {

namespace {
MONGO_FAIL_POINT_DEFINE(simulateBsonColumnCompressionDataLoss);

CompressionResult _compressBucket(const BSONObj& bucketDoc,
                                  StringData timeFieldName,
                                  const NamespaceString& nss,
                                  bool validateDecompression) try {
    CompressionResult result;

    // Buffer to help manipulate data if simulateBsonColumnCompressionDataLoss is set.
    // Contents must outlive `measurements` defined below.
    std::unique_ptr<char[]> tamperedData;

    BSONObjBuilder builder;  // builder to build the compressed bucket
    std::vector<write_ops_utils::details::Measurement>
        measurements;                       // Extracted measurements from uncompressed bucket
    boost::optional<BSONObjIterator> time;  // Iterator to read time fields from uncompressed bucket
    std::vector<std::pair<StringData, BSONObjIterator>>
        columns;  // Iterators to read data fields from uncompressed bucket

    BSONElement bucketId;
    BSONElement controlElement;
    std::vector<BSONElement> otherElements;

    // Read everything from the uncompressed bucket
    for (auto& elem : bucketDoc) {
        // Record bucketId
        if (elem.fieldNameStringData() == "_id"_sd) {
            bucketId = elem;
            continue;
        }

        // Record control element, we need to parse the uncompressed bucket before writing new
        // control block.
        if (elem.fieldNameStringData() == kBucketControlFieldName) {
            controlElement = elem;
            continue;
        }

        // Everything that's not under data or control is left as-is, record elements so we can
        // write later (we want _id and control to be first).
        if (elem.fieldNameStringData() != kBucketDataFieldName) {
            otherElements.push_back(elem);
            continue;
        }

        // Setup iterators to read all fields under data in lock-step
        for (auto& columnObj : elem.Obj()) {
            if (columnObj.fieldNameStringData() == timeFieldName) {
                time.emplace(columnObj.Obj());
            } else {
                columns.emplace_back(columnObj.fieldNameStringData(), columnObj.Obj());
            }
        }
    }

    // If provided time field didn't exist then there is nothing to do
    if (!time) {
        return result;
    }

    // Read all measurements from bucket
    while (time->more()) {
        // Get and advance the time iterator
        auto timeElement = time->next();

        // Get BSONElement's to all data elements. Missing data fields are represented as EOO.
        write_ops_utils::details::Measurement measurement;
        measurement.timeField = timeElement;
        measurement.dataFields.resize(columns.size());

        // Read one element from each data field iterator
        for (size_t i = 0; i < columns.size(); ++i) {
            auto& column = columns[i].second;
            // If we reach the end nothing more to do, all subsequent elements will be left as
            // EOO/missing.
            if (!column.more()) {
                continue;
            }

            // Check that the field name match the name of the time field. Field names are
            // strings of integer indexes, "0", "1" etc. Data fields may have missing entries
            // where the time field may not. So we can use this fact and just do a simple string
            // compare against the time field. If it does not match our data field is skipped
            // and the iterator is positioned at an element with a higher index. We should then
            // leave the data field as EOO and not advance the data iterator.
            auto elem = *column;
            if (timeElement.fieldNameStringData() == elem.fieldNameStringData()) {
                // Extract the element and advance the iterator
                measurement.dataFields[i] = elem;
                column.advance(elem);
            }
        }

        measurements.push_back(std::move(measurement));
    }

    // Verify that we are at end for all data iterators, if we are not then there is something
    // funky with the bucket and we have not read everything. We cannot compress as that would
    // lose user data.
    // This can happen if the bucket contain unordered keys in its data fields {"0": ..., "2":
    // ..., "1": ...}. Or if there are more data fields than time fields.
    if (std::any_of(columns.begin(), columns.end(), [](const auto& entry) {
            return entry.second.more();
        })) {
        LOGV2_DEBUG(5857801,
                    1,
                    "Failed to parse timeseries bucket during compression, leaving uncompressed");
        return result;
    }

    // Sort all the measurements on time order.
    std::sort(measurements.begin(),
              measurements.end(),
              [](const write_ops_utils::details::Measurement& lhs,
                 const write_ops_utils::details::Measurement& rhs) {
                  return lhs.timeField.date() < rhs.timeField.date();
              });

    // Write _id unless EOO which it can be in some unittests
    if (!bucketId.eoo()) {
        builder.append(bucketId);
    }

    // Write control block
    {
        BSONObjBuilder control(builder.subobjStart(kBucketControlFieldName));

        // Set the version to indicate that the bucket was compressed. Leave other control fields
        // unchanged.
        bool versionSet = false;
        for (const auto& controlField : controlElement.Obj()) {
            if (controlField.fieldNameStringData() == kBucketControlVersionFieldName) {
                control.append(kBucketControlVersionFieldName,
                               kTimeseriesControlCompressedSortedVersion);
                versionSet = true;
            } else {
                control.append(controlField);
            }
        }

        // Set version if it was missing from uncompressed bucket
        if (!versionSet) {
            control.append(kBucketControlVersionFieldName,
                           kTimeseriesControlCompressedSortedVersion);
        }

        // Set count
        control.append(kBucketControlCountFieldName, static_cast<int32_t>(measurements.size()));
    }

    // Write non control or data elements that are left as-is.
    for (auto&& elem : otherElements) {
        builder.append(elem);
    }

    // Last, compress elements and build compressed bucket
    {
        // Helper to validate compressed data by binary comparing decompressed with original.
        auto validate = [&](BSONBinData binary, StringData fieldName, auto getField) {
            if (!validateDecompression)
                return true;

            BSONColumn col(binary);
            auto measurementEnd = measurements.end();
            auto columnEnd = col.end();

            auto res =
                std::mismatch(measurements.begin(),
                              measurementEnd,
                              col.begin(),
                              columnEnd,
                              [&getField](const auto& measurement, BSONElement decompressed) {
                                  return getField(measurement).binaryEqualValues(decompressed);
                              });


            // If both are at end then there is no mismatch
            if (res.first == measurementEnd && res.second == columnEnd) {
                return true;
            }

            // If one is at end then we have a size mismatch
            if (res.first == measurementEnd || res.second == columnEnd) {
                LOGV2_ERROR(
                    6179302,
                    "Time-series bucket compression failed due to decompression size mismatch",
                    logAttrs(nss),
                    "bucketId"_attr = bucketId.wrap(),
                    "original"_attr = measurements.size(),
                    "decompressed"_attr = col.size(),
                    "bucket"_attr = redact(bucketDoc));
                // invariant in debug builds to generate dump
                dassert(simulateBsonColumnCompressionDataLoss.shouldFail());
                return false;
            }

            // Otherwise the elements themselves don't match
            auto index = std::distance(measurements.begin(), res.first);
            LOGV2_ERROR(6179301,
                        "Time-series bucket compression failed due to decompression data loss",
                        logAttrs(nss),
                        "bucketId"_attr = bucketId.wrap(),
                        "index"_attr = index,
                        "type"_attr = getField(*res.first).type(),
                        "original"_attr = redact(getField(*res.first).wrap()),
                        "decompressed"_attr = redact(res.second->wrap()),
                        "bucket"_attr = redact(bucketDoc));
            // invariant in debug builds to generate dump
            dassert(simulateBsonColumnCompressionDataLoss.shouldFail());
            return false;
        };

        BSONObjBuilder dataBuilder = builder.subobjStart(kBucketDataFieldName);
        // Reusable buffer to avoid extra allocs per column.
        allocator_aware::BufBuilder columnBuffer;

        // Add compressed time field first
        {
            BSONColumnBuilder<> timeColumn(std::move(columnBuffer));
            for (const auto& measurement : measurements) {
                timeColumn.append(measurement.timeField);
            }
            BSONBinData timeBinary = timeColumn.finalize();

            // Simulate compression data loss by tampering with original data when FailPoint is set.
            // This should be detected in the validate call below.
            if (MONGO_unlikely(simulateBsonColumnCompressionDataLoss.shouldFail() &&
                               !measurements.empty())) {
                // We copy the entire BSONElement and modify the first value byte. The original
                // BSONElement is not touched
                BSONElement elem = measurements.front().timeField;
                tamperedData.reset(new char[elem.size()]);
                memcpy(tamperedData.get(), elem.rawdata(), elem.size());

                BSONElement tampered(tamperedData.get());
                ++(*const_cast<char*>(tampered.value()));
                measurements.front().timeField = tampered;
            }

            if (!validate(timeBinary, timeFieldName, [](const auto& measurement) {
                    return measurement.timeField;
                })) {
                result.decompressionFailed = true;
                return result;
            }
            dataBuilder.append(timeFieldName, timeBinary);
            columnBuffer = timeColumn.detach();
        }

        // Then add compressed data fields.
        for (size_t i = 0; i < columns.size(); ++i) {
            BSONColumnBuilder<> column(std::move(columnBuffer));
            for (const auto& measurement : measurements) {
                if (auto elem = measurement.dataFields[i]) {
                    column.append(elem);
                } else {
                    column.skip();
                }
            }
            BSONBinData dataBinary = column.finalize();
            if (!validate(dataBinary, columns[i].first, [i](const auto& measurement) {
                    return measurement.dataFields[i];
                })) {
                result.decompressionFailed = true;
                return result;
            }
            dataBuilder.append(columns[i].first, dataBinary);
            // We only record when the interleaved mode has to re-start. i.e. when more than one
            // interleaved start control byte was written in the binary
            if (int interleavedStarts = column.numInterleavedStartWritten();
                interleavedStarts > 1) {
                result.numInterleavedRestarts += interleavedStarts - 1;
            }
            columnBuffer = column.detach();
        }
    }

    result.compressedBucket = builder.obj();
    return result;
} catch (...) {
    return {};
}
}  // namespace

CompressionResult compressBucket(const BSONObj& bucketDoc,
                                 StringData timeFieldName,
                                 const NamespaceString& ns,
                                 bool validateDecompression) try {
    // Compressing already compressed buckets is a no-op.
    if (isCompressedBucket(bucketDoc)) {
        CompressionResult res;
        res.compressedBucket = bucketDoc.getOwned();
        return res;
    }
    auto result = _compressBucket(bucketDoc, timeFieldName, ns, validateDecompression);
    if (!result.compressedBucket &&
        !MONGO_unlikely(simulateBsonColumnCompressionDataLoss.shouldFail())) {
        LOGV2_ERROR_OPTIONS(8000400,
                            {logv2::LogTruncation::Disabled},
                            "Couldn't compress time-series bucket",
                            "bucket"_attr =
                                redact(base64::encode(bucketDoc.objdata(), bucketDoc.objsize())),
                            "ns"_attr = ns.toStringForErrorMsg());
        // Also log without any risk of PII
        LOGV2_ERROR(9547401,
                    "Couldn't compress time-series bucket",
                    "bucketId"_attr = bucketDoc["_id"],
                    "ns"_attr = ns.toStringForErrorMsg());
        return {};
    }
    return result;
} catch (...) {
    // Make sure no exceptions leak out of this function
    LOGV2_DEBUG(5857800,
                1,
                "Exception when compressing timeseries bucket, leaving it uncompressed",
                "error"_attr = exceptionToStatus());
    return {};
}

boost::optional<BSONObj> decompressBucket(const BSONObj& bucketDoc) try {
    BSONObjBuilder builder;

    for (auto&& topLevel : bucketDoc) {
        if (topLevel.fieldNameStringData() == kBucketControlFieldName) {
            BSONObjBuilder controlBuilder{builder.subobjStart(kBucketControlFieldName)};

            for (auto&& e : topLevel.Obj()) {
                if (e.fieldNameStringData() == kBucketControlVersionFieldName) {
                    // Check that we have a compressed bucket, and rewrite the version to signal
                    // it's uncompressed now.
                    if (e.type() != BSONType::numberInt ||
                        (e.numberInt() != kTimeseriesControlCompressedSortedVersion &&
                         e.numberInt() != kTimeseriesControlCompressedUnsortedVersion)) {
                        // This bucket isn't compressed.
                        return boost::none;
                    }
                    builder.append(kBucketControlVersionFieldName,
                                   kTimeseriesControlUncompressedVersion);
                } else if (e.fieldNameStringData() == kBucketControlCountFieldName) {
                    // Omit the count field when decompressing.
                    continue;
                } else {
                    // Just copy all the other fields.
                    builder.append(e);
                }
            }
        } else if (topLevel.fieldNameStringData() == kBucketDataFieldName) {
            BSONObjBuilder dataBuilder{builder.subobjStart(kBucketDataFieldName)};

            // Iterate over the compressed data columns and decompress each one.
            for (auto&& e : topLevel.Obj()) {
                if (e.type() != BSONType::binData) {
                    // This bucket isn't actually compressed.
                    return boost::none;
                }

                BSONObjBuilder columnBuilder{dataBuilder.subobjStart(e.fieldNameStringData())};

                BSONColumn column{e};
                DecimalCounter<uint32_t> count{0};
                for (auto&& measurement : column) {
                    if (!measurement.eoo()) {
                        builder.appendAs(measurement, count);
                    }
                    count++;
                }
            }
        } else {
            // If it's not control or data, we can just copy it and continue.
            builder.append(topLevel);
        }
    }

    return builder.obj();
} catch (...) {
    return boost::none;
}

bool isCompressedBucket(const BSONObj& bucketDoc) {
    auto&& controlField = bucketDoc[timeseries::kBucketControlFieldName];
    uassert(6540600,
            "Time-series bucket documents must have 'control' object present",
            controlField && controlField.type() == BSONType::object);

    auto&& controlFieldObj = controlField.Obj();
    auto&& versionField = controlFieldObj[timeseries::kBucketControlVersionFieldName];
    uassert(6540601,
            "Time-series bucket documents must have 'control.version' field present",
            versionField && isNumericBSONType(versionField.type()));
    auto version = versionField.Number();

    if (version == kTimeseriesControlUncompressedVersion) {
        return false;
    } else if (version == kTimeseriesControlCompressedSortedVersion ||
               version == kTimeseriesControlCompressedUnsortedVersion) {
        return true;
    } else {
        uasserted(6540602, "Invalid bucket version");
    }
}

}  // namespace timeseries
}  // namespace mongo
