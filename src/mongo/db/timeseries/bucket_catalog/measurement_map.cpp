/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"
#include "mongo/bson/util/bsoncolumn.h"

namespace mongo::timeseries::bucket_catalog {

MeasurementMap::MeasurementMap(TrackingContext& trackingContext)
    : _trackingContext(trackingContext),
      _builders(makeTrackedStringMap<std::tuple<size_t, BSONColumnBuilder>>(_trackingContext)) {}

void MeasurementMap::initBuilders(BSONObj bucketDataDocWithCompressedBuilders,
                                  size_t numMeasurements) {
    for (auto&& [key, columnValue] : bucketDataDocWithCompressedBuilders) {
        int binLength = 0;
        const char* binData = columnValue.binData(binLength);

        _builders.emplace(make_tracked_string(_trackingContext, key.data(), key.size()),
                          std::make_pair(numMeasurements, BSONColumnBuilder(binData, binLength)));
    }
    _measurementCount = numMeasurements;
}

void MeasurementMap::_insertNewKey(StringData key,
                                   const BSONElement& elem,
                                   BSONColumnBuilder builder,
                                   size_t numMeasurements) {
    builder.append(elem);
    _builders.try_emplace(make_tracked_string(_trackingContext, key.data(), key.size()),
                          numMeasurements,
                          std::move(builder));
}


void MeasurementMap::_fillSkipsInMissingFields() {
    size_t numExpectedMeasurements = _measurementCount;

    // Fill in skips for any fields that existed in prior measurements in this bucket, but
    // weren't in this measurement.
    for (auto& entry : _builders) {
        auto& [numMeasurements, builder] = entry.second;
        if (numMeasurements != numExpectedMeasurements) {
            invariant((numMeasurements + 1) == numExpectedMeasurements,
                      "Measurement count should only be off by one when inserting measurements.");
            builder.skip();
            ++numMeasurements;
        }
    }
}

void MeasurementMap::insertOne(std::vector<BSONElement> oneMeasurementDataFields) {
    for (const auto& elem : oneMeasurementDataFields) {
        StringData key = elem.fieldNameStringData();

        auto builderIt = _builders.find(key);
        if (builderIt == _builders.end()) {
            BSONColumnBuilder columnBuilder;
            for (size_t i = 0; i < _measurementCount; ++i) {
                columnBuilder.skip();
            }
            _insertNewKey(key, elem, std::move(columnBuilder), _measurementCount + 1);
        } else {
            auto& [numMeasurements, columnBuilder] = builderIt->second;
            columnBuilder.append(elem);
            ++numMeasurements;
        }
    }
    _measurementCount++;
    _fillSkipsInMissingFields();
}

BSONColumnBuilder& MeasurementMap::getBuilder(StringData key) {
    auto it = _builders.find(key);
    invariant(it != _builders.end());
    return std::get<1>(it->second);
}

void MeasurementMap::_assertInternalStateIdentical_forTest() {
    for (auto& entry : _builders) {
        invariant(std::get<0>(entry.second) == _measurementCount);
    }
}

}  // namespace mongo::timeseries::bucket_catalog
