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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/timeseries/bucket_compression.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace timeseries {

boost::optional<BSONObj> compressBucket(const BSONObj& bucketDoc, StringData timeFieldName) try {
    // Helper for uncompressed measurements
    struct Measurement {
        BSONElement timeField;
        std::vector<BSONElement> dataFields;
    };

    BSONObjBuilder builder;                 // builder to build the compressed bucket
    std::vector<Measurement> measurements;  // Extracted measurements from uncompressed bucket
    boost::optional<BSONObjIterator> time;  // Iterator to read time fields from uncompressed bucket
    std::vector<std::pair<StringData, BSONObjIterator>>
        columns;  // Iterators to read data fields from uncompressed bucket

    BSONElement controlElement;
    std::vector<BSONElement> otherElements;

    // Read everything from the uncompressed bucket
    for (auto& elem : bucketDoc) {
        // Record control element, we need to parse the uncompressed bucket before writing new
        // control block.
        if (elem.fieldNameStringData() == kBucketControlFieldName) {
            controlElement = elem;
            continue;
        }

        // Everything that's not under data or control is left as-is, record elements so we can
        // write later (we want control to be first).
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
        return boost::none;
    }

    // Read all measurements from bucket
    while (time->more()) {
        // Get and advance the time iterator
        auto timeElement = time->next();

        // Get BSONElement's to all data elements. Missing data fields are represented as EOO.
        Measurement measurement;
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
        return boost::none;
    }

    // Sort all the measurements on time order.
    std::sort(measurements.begin(),
              measurements.end(),
              [](const Measurement& lhs, const Measurement& rhs) {
                  return lhs.timeField.timestamp() < rhs.timeField.timestamp();
              });

    // Write control block
    {
        BSONObjBuilder control(builder.subobjStart(kBucketControlFieldName));

        // Set right version, leave other control fields unchanged
        bool versionSet = false;
        for (const auto& controlField : controlElement.Obj()) {
            if (controlField.fieldNameStringData() == kBucketControlVersionFieldName) {
                control.append(kBucketControlVersionFieldName, kTimeseriesControlCompressedVersion);
                versionSet = true;
            } else {
                control.append(controlField);
            }
        }

        // Set version if it was missing from uncompressed bucket
        if (!versionSet) {
            control.append(kBucketControlVersionFieldName, kTimeseriesControlCompressedVersion);
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
        BSONObjBuilder dataBuilder = builder.subobjStart(kBucketDataFieldName);
        BufBuilder columnBuffer;  // Reusable buffer to avoid extra allocs per column.

        // Add compressed time field first
        {
            BSONColumnBuilder timeColumn(timeFieldName, std::move(columnBuffer));
            for (const auto& measurement : measurements) {
                timeColumn.append(measurement.timeField);
            }
            dataBuilder.append(timeFieldName, timeColumn.finalize());
            columnBuffer = timeColumn.detach();
        }

        // Then add compressed data fields.
        for (size_t i = 0; i < columns.size(); ++i) {
            BSONColumnBuilder column(columns[i].first, std::move(columnBuffer));
            for (const auto& measurement : measurements) {
                if (auto elem = measurement.dataFields[i]) {
                    column.append(elem);
                } else {
                    column.skip();
                }
            }
            dataBuilder.append(column.fieldName(), column.finalize());
            columnBuffer = column.detach();
        }
    }

    return builder.obj();
} catch (...) {
    // Skip compression if we encounter any exception
    LOGV2_DEBUG(5857800,
                1,
                "Exception when compressing timeseries bucket, leaving it uncompressed",
                "error"_attr = exceptionToStatus());
    return boost::none;
}

}  // namespace timeseries
}  // namespace mongo
