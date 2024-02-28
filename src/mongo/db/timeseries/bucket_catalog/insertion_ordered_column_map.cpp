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

#include "mongo/db/timeseries/bucket_catalog/insertion_ordered_column_map.h"
#include "mongo/bson/util/bsoncolumn.h"

namespace mongo::timeseries::bucket_catalog {

InsertionOrderedColumnMap::InsertionOrderedColumnMap(TrackingContext& trackingContext)
    : _trackingContext(trackingContext),
      _builders(makeTrackedStringMap<MeasurementCountAndBuilder>(_trackingContext)),
      _insertionOrder(make_tracked_vector<tracked_string>(_trackingContext)) {}

void InsertionOrderedColumnMap::initBuilders(BSONObj bucketDataDocWithCompressedBuilders,
                                             size_t numMeasurements) {
    for (auto&& [key, columnValue] : bucketDataDocWithCompressedBuilders) {
        int binLength = 0;
        const char* binData = columnValue.binData(binLength);
        // TODO SERVER-79416: Use BSONColumnBuilder reopen constructor. It leaves the builder in a
        // diff-producing state so this needs to be integrated together with the intermediate()
        // call.
        auto [it, inserted] =
            _builders.try_emplace(make_tracked_string(_trackingContext, key.data(), key.size()),
                                  numMeasurements,
                                  BSONColumnBuilder());
        BSONColumn c(binData, binLength);
        for (auto&& elem : c) {
            std::get<1>(it->second).append(elem);
        }

        _insertionOrder.emplace_back(make_tracked_string(_trackingContext, key.data()));
        _insertionOrderSize += key.size();
    }
    _measurementCount = numMeasurements;
}

void InsertionOrderedColumnMap::_insertNewKey(StringData key,
                                              const BSONElement& elem,
                                              BSONColumnBuilder builder,
                                              size_t numMeasurements) {
    builder.append(elem);
    _builders.try_emplace(make_tracked_string(_trackingContext, key.data(), key.size()),
                          numMeasurements,
                          std::move(builder));
    _insertionOrder.emplace_back(make_tracked_string(_trackingContext, key.data()));
    _insertionOrderSize += key.size();
}


void InsertionOrderedColumnMap::_fillSkipsInMissingFields() {
    size_t numExpectedMeasurements = _measurementCount;

    // Fill in skips for any fields that existed in prior measurements in this bucket, but
    // weren't in this measurement.
    for (auto& [key, pairValue] : _builders) {
        auto& [numMeasurements, builder] = pairValue;
        if (numMeasurements != numExpectedMeasurements) {
            invariant((numMeasurements + 1) == numExpectedMeasurements,
                      "Measurement count should only be off by one when inserting measurements.");
            builder.skip();
            ++numMeasurements;
        }
    }
}

void InsertionOrderedColumnMap::insertOne(std::vector<BSONElement> oneMeasurementDataFields) {
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

BSONColumnBuilder& InsertionOrderedColumnMap::getBuilder(StringData key) {
    auto it = _builders.find(key);
    invariant(it != _builders.end());
    return std::get<1>(it->second);
}

StringData InsertionOrderedColumnMap::_getDirect() {
    invariant(_pos < _insertionOrder.size());
    return StringData(_insertionOrder[_pos].c_str());
}

boost::optional<StringData> InsertionOrderedColumnMap::begin() {
    _pos = 0;
    return next();
}

boost::optional<StringData> InsertionOrderedColumnMap::next() {
    if (_pos < _insertionOrder.size()) {
        StringData result = _getDirect();
        ++_pos;
        return result;
    }
    return boost::none;
}

void InsertionOrderedColumnMap::_assertInternalStateIdentical_forTest() {
    size_t keySizes = 0;
    for (auto& [key, pairValue] : _builders) {
        keySizes += key.size();
        auto& [numMeasurements, builder] = pairValue;
        BSONBinData binData = builder.finalize();
        BSONColumn col(binData);

        invariant(col.size() == numMeasurements);
        invariant(numMeasurements == _measurementCount);

        // All keys in _builders should exist in _insertionOrder.
        invariant(std::find(_insertionOrder.begin(), _insertionOrder.end(), key.c_str()) !=
                  std::end(_insertionOrder));
    }

    // Number of keys in both structures should be the same.
    invariant(_insertionOrder.size() == _builders.size());

    // All keys in _insertionOrder should exist in _builders.
    for (auto& key : _insertionOrder) {
        invariant(_builders.find(key) != _builders.end());
    }
    invariant(keySizes == _insertionOrderSize);

    // Current iterator position must be [0, N+1].
    invariant(_pos >= 0);
    invariant(_pos <= _insertionOrder.size() + 1);
}


}  // namespace mongo::timeseries::bucket_catalog
