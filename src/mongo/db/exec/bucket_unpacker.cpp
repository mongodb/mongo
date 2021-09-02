/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/bucket_unpacker.h"
#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo {

/**
 * Erase computed meta projection fields if they are present in the exclusion field set.
 */
void eraseExcludedComputedMetaProjFields(BucketUnpacker::Behavior unpackerBehavior,
                                         BucketSpec* bucketSpec) {
    if (unpackerBehavior == BucketUnpacker::Behavior::kExclude &&
        bucketSpec->computedMetaProjFields.size() > 0) {
        for (auto it = bucketSpec->computedMetaProjFields.begin();
             it != bucketSpec->computedMetaProjFields.end();) {
            if (bucketSpec->fieldSet.find(*it) != bucketSpec->fieldSet.end()) {
                it = bucketSpec->computedMetaProjFields.erase(it);
            } else {
                it++;
            }
        }
    }
}

BucketUnpacker::BucketUnpacker(BucketSpec spec, Behavior unpackerBehavior) {
    setBucketSpecAndBehavior(std::move(spec), unpackerBehavior);
}

// Calculates the number of measurements in a bucket given the 'targetTimestampObjSize' using the
// 'BucketUnpacker::kTimestampObjSizeTable' table. If the 'targetTimestampObjSize' hits a record in
// the table, this helper returns the measurement count corresponding to the table record.
// Otherwise, the 'targetTimestampObjSize' is used to probe the table for the smallest {b_i, S_i}
// pair such that 'targetTimestampObjSize' < S_i. Once the interval is found, the upper bound of the
// pair for the interval is computed and then linear interpolation is used to compute the
// measurement count corresponding to the 'targetTimestampObjSize' provided.
int BucketUnpacker::computeMeasurementCount(int targetTimestampObjSize) {
    auto currentInterval =
        std::find_if(std::begin(BucketUnpacker::kTimestampObjSizeTable),
                     std::end(BucketUnpacker::kTimestampObjSizeTable),
                     [&](const auto& entry) { return targetTimestampObjSize <= entry.second; });

    if (currentInterval->second == targetTimestampObjSize) {
        return currentInterval->first;
    }
    // This points to the first interval larger than the target 'targetTimestampObjSize', the actual
    // interval that will cover the object size is the interval before the current one.
    tassert(5422104,
            "currentInterval should not point to the first table entry",
            currentInterval > BucketUnpacker::kTimestampObjSizeTable.begin());
    --currentInterval;

    auto nDigitsInRowKey = 1 + (currentInterval - BucketUnpacker::kTimestampObjSizeTable.begin());

    return currentInterval->first +
        ((targetTimestampObjSize - currentInterval->second) / (10 + nDigitsInRowKey));
}

void BucketUnpacker::reset(BSONObj&& bucket) {
    _fieldIters.clear();
    _timeFieldIter = boost::none;

    _bucket = std::move(bucket);
    uassert(5346510, "An empty bucket cannot be unpacked", !_bucket.isEmpty());

    auto&& dataRegion = _bucket.getField(timeseries::kBucketDataFieldName).Obj();
    if (dataRegion.isEmpty()) {
        // If the data field of a bucket is present but it holds an empty object, there's nothing to
        // unpack.
        return;
    }

    auto&& timeFieldElem = dataRegion.getField(_spec.timeField);
    uassert(5346700,
            "The $_internalUnpackBucket stage requires the data region to have a timeField object",
            timeFieldElem);

    _timeFieldIter = BSONObjIterator{timeFieldElem.Obj()};

    _metaValue = _bucket[timeseries::kBucketMetaFieldName];
    if (_spec.metaField) {
        // The spec indicates that there might be a metadata region. Missing metadata in
        // measurements is expressed with missing metadata in a bucket. But we disallow undefined
        // since the undefined BSON type is deprecated.
        uassert(5369600,
                "The $_internalUnpackBucket stage allows metadata to be absent or otherwise, it "
                "must not be the deprecated undefined bson type",
                !_metaValue || _metaValue.type() != BSONType::Undefined);
    } else {
        // If the spec indicates that the time series collection has no metadata field, then we
        // should not find a metadata region in the underlying bucket documents.
        uassert(5369601,
                "The $_internalUnpackBucket stage expects buckets to have missing metadata regions "
                "if the metaField parameter is not provided",
                !_metaValue);
    }

    // Walk the data region of the bucket, and decide if an iterator should be set up based on the
    // include or exclude case.
    for (auto&& elem : dataRegion) {
        auto& colName = elem.fieldNameStringData();
        if (colName == _spec.timeField) {
            // Skip adding a FieldIterator for the timeField since the timestamp value from
            // _timeFieldIter can be placed accordingly in the materialized measurement.
            continue;
        }

        // Includes a field when '_unpackerBehavior' is 'kInclude' and it's found in 'fieldSet' or
        // _unpackerBehavior is 'kExclude' and it's not found in 'fieldSet'.
        if (determineIncludeField(colName, _unpackerBehavior, _spec)) {
            _fieldIters.emplace_back(colName.toString(), BSONObjIterator{elem.Obj()});
        }
    }

    // Update computed meta projections with values from this bucket.
    if (!_spec.computedMetaProjFields.empty()) {
        for (auto&& name : _spec.computedMetaProjFields) {
            _computedMetaProjections[name] = _bucket[name];
        }
    }

    // Save the measurement count for the bucket.
    _numberOfMeasurements = computeMeasurementCount(timeFieldElem.objsize());
}

void BucketUnpacker::setBucketSpecAndBehavior(BucketSpec&& bucketSpec, Behavior behavior) {
    _includeMetaField = eraseMetaFromFieldSetAndDetermineIncludeMeta(behavior, &bucketSpec);
    _includeTimeField = determineIncludeTimeField(behavior, &bucketSpec);
    _unpackerBehavior = behavior;
    eraseExcludedComputedMetaProjFields(behavior, &bucketSpec);
    _spec = std::move(bucketSpec);
}

const std::set<StringData> BucketUnpacker::reservedBucketFieldNames = {
    timeseries::kBucketIdFieldName,
    timeseries::kBucketDataFieldName,
    timeseries::kBucketMetaFieldName,
    timeseries::kBucketControlFieldName};

void BucketUnpacker::addComputedMetaProjFields(const std::vector<StringData>& computedFieldNames) {
    for (auto&& field : computedFieldNames) {
        _spec.computedMetaProjFields.emplace_back(field.toString());

        // If we're already specifically including fields, we need to add the computed fields to
        // the included field set to ensure they are unpacked.
        if (_unpackerBehavior == BucketUnpacker::Behavior::kInclude) {
            _spec.fieldSet.emplace(field);
        }
    }
}

Document BucketUnpacker::getNext() {
    tassert(5521503, "'getNext()' requires the bucket to be owned", _bucket.isOwned());
    tassert(5422100, "'getNext()' was called after the bucket has been exhausted", hasNext());

    auto measurement = MutableDocument{};
    auto&& timeElem = _timeFieldIter->next();
    if (_includeTimeField) {
        measurement.addField(_spec.timeField, Value{timeElem});
    }

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (_includeMetaField && _metaValue) {
        measurement.addField(*_spec.metaField, Value{_metaValue});
    }

    auto& currentIdx = timeElem.fieldNameStringData();
    for (auto&& [colName, colIter] : _fieldIters) {
        if (auto&& elem = *colIter; colIter.more() && elem.fieldNameStringData() == currentIdx) {
            measurement.addField(colName, Value{elem});
            colIter.advance(elem);
        }
    }

    // Add computed meta projections.
    for (auto&& name : _spec.computedMetaProjFields) {
        measurement.addField(name, Value{_computedMetaProjections[name]});
    }

    return measurement.freeze();
}

Document BucketUnpacker::extractSingleMeasurement(int j) {
    tassert(5422101,
            "'extractSingleMeasurment' expects j to be greater than or equal to zero and less than "
            "or equal to the number of measurements in a bucket",
            j >= 0 && j < _numberOfMeasurements);

    auto measurement = MutableDocument{};

    auto rowKey = std::to_string(j);
    auto targetIdx = StringData{rowKey};
    auto&& dataRegion = _bucket.getField(timeseries::kBucketDataFieldName).Obj();

    if (_includeMetaField && !_metaValue.isNull()) {
        measurement.addField(*_spec.metaField, Value{_metaValue});
    }

    for (auto&& dataElem : dataRegion) {
        auto colName = dataElem.fieldNameStringData();
        if (!determineIncludeField(colName, _unpackerBehavior, _spec)) {
            continue;
        }
        auto value = dataElem[targetIdx];
        if (value) {
            measurement.addField(dataElem.fieldNameStringData(), Value{value});
        }
    }

    // Add computed meta projections.
    for (auto&& name : _spec.computedMetaProjFields) {
        measurement.addField(name, Value{_computedMetaProjections[name]});
    }

    return measurement.freeze();
}
}  // namespace mongo
