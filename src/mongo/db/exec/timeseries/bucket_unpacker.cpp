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

#include "mongo/db/exec/timeseries/bucket_unpacker.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#include <s2cellid.h>


namespace mongo::timeseries {

class BucketUnpacker::UnpackingImpl {
public:
    UnpackingImpl() = default;
    virtual ~UnpackingImpl() = default;

    virtual void addField(const BSONElement& field) = 0;
    virtual int measurementCount(const BSONElement& timeField) const = 0;
    virtual bool getNext(MutableDocument& measurement,
                         const BucketSpec& spec,
                         const Value& metaValue,
                         bool includeTimeField,
                         bool includeMetaField) = 0;
    virtual bool getNext(BSONObjBuilder& builder,
                         const BucketSpec& spec,
                         const BSONElement& metaValue,
                         bool includeTimeField,
                         bool includeMetaField) = 0;
    virtual void extractSingleMeasurement(MutableDocument& measurement,
                                          int j,
                                          const BucketSpec& spec,
                                          const std::set<std::string>& unpackFieldsToIncludeExclude,
                                          const BSONObj& bucket,
                                          const Value& metaValue,
                                          bool includeTimeField,
                                          bool includeMetaField) = 0;

    // Provides an upper bound on the number of fields in each measurement.
    virtual std::size_t numberOfFields() = 0;

protected:
    // Data field count is variable, but time and metadata are fixed.
    constexpr static std::size_t kFixedFieldNumber = 2;
};

namespace {
// Unpacker for V1 uncompressed buckets
class BucketUnpackerV1 : public BucketUnpacker::UnpackingImpl {
public:
    BucketUnpackerV1(const BSONElement& timeField);

    void addField(const BSONElement& field) override;
    int measurementCount(const BSONElement& timeField) const override;
    bool getNext(MutableDocument& measurement,
                 const BucketSpec& spec,
                 const Value& metaValue,
                 bool includeTimeField,
                 bool includeMetaField) override;
    bool getNext(BSONObjBuilder& builder,
                 const BucketSpec& spec,
                 const BSONElement& metaValue,
                 bool includeTimeField,
                 bool includeMetaField) override;
    void extractSingleMeasurement(MutableDocument& measurement,
                                  int j,
                                  const BucketSpec& spec,
                                  const std::set<std::string>& unpackFieldsToIncludeExclude,
                                  const BSONObj& bucket,
                                  const Value& metaValue,
                                  bool includeTimeField,
                                  bool includeMetaField) override;
    std::size_t numberOfFields() override;

private:
    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    BSONObjIterator _timeFieldIter;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'BucketSpec'.
    std::vector<std::pair<std::string, BSONObjIterator>> _fieldIters;
};

BucketUnpackerV1::BucketUnpackerV1(const BSONElement& timeField)
    : _timeFieldIter(BSONObjIterator{timeField.Obj()}) {}

void BucketUnpackerV1::addField(const BSONElement& field) {
    _fieldIters.emplace_back(field.fieldNameStringData(), BSONObjIterator{field.Obj()});
}

int BucketUnpackerV1::measurementCount(const BSONElement& timeField) const {
    return computeElementCountFromTimestampObjSize(timeField.objsize());
}

bool BucketUnpackerV1::getNext(MutableDocument& measurement,
                               const BucketSpec& spec,
                               const Value& metaValue,
                               bool includeTimeField,
                               bool includeMetaField) {
    auto&& timeElem = _timeFieldIter.next();
    if (includeTimeField) {
        measurement.addField(spec.timeFieldHashed(), Value{timeElem});
    }

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    const auto& currentIdx = timeElem.fieldNameStringData();
    for (auto&& [colName, colIter] : _fieldIters) {
        if (auto&& elem = *colIter; colIter.more() && elem.fieldNameStringData() == currentIdx) {
            measurement.addField(colName, Value{elem});
            colIter.advance(elem);
        }
    }

    return _timeFieldIter.more();
}

bool BucketUnpackerV1::getNext(BSONObjBuilder& builder,
                               const BucketSpec& spec,
                               const BSONElement& metaValue,
                               bool includeTimeField,
                               bool includeMetaField) {
    auto&& timeElem = _timeFieldIter.next();
    if (includeTimeField) {
        builder.appendAs(timeElem, spec.timeField());
    }

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (includeMetaField && !metaValue.eoo()) {
        builder.appendAs(metaValue, *spec.metaField());
    }

    const auto& currentIdx = timeElem.fieldNameStringData();
    for (auto&& [colName, colIter] : _fieldIters) {
        if (auto&& elem = *colIter; colIter.more() && elem.fieldNameStringData() == currentIdx) {
            builder.appendAs(elem, colName);
            colIter.advance(elem);
        }
    }

    return _timeFieldIter.more();
}

void BucketUnpackerV1::extractSingleMeasurement(
    MutableDocument& measurement,
    int j,
    const BucketSpec& spec,
    const std::set<std::string>& unpackFieldsToIncludeExclude,
    const BSONObj& bucket,
    const Value& metaValue,
    bool includeTimeField,
    bool includeMetaField) {
    auto rowKey = std::to_string(j);
    auto targetIdx = StringData{rowKey};
    auto&& dataRegion = bucket.getField(kBucketDataFieldName).Obj();

    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    for (auto&& dataElem : dataRegion) {
        const auto& colName = dataElem.fieldNameStringData();
        if (!determineIncludeField(colName, spec.behavior(), unpackFieldsToIncludeExclude)) {
            continue;
        }
        auto value = dataElem[targetIdx];
        if (value) {
            measurement.addField(dataElem.fieldNameStringData(), Value{value});
        }
    }
}

std::size_t BucketUnpackerV1::numberOfFields() {
    // The data fields are tracked by _fieldIters, but we need to account also for the time field
    // and possibly the meta field.
    return kFixedFieldNumber + _fieldIters.size();
}

// Unpacker for V2 compressed buckets
class BucketUnpackerV2 : public BucketUnpacker::UnpackingImpl {
public:
    BucketUnpackerV2(const BSONElement& timeField, int elementCount);

    void addField(const BSONElement& field) override;
    int measurementCount(const BSONElement& timeField) const override;
    bool getNext(MutableDocument& measurement,
                 const BucketSpec& spec,
                 const Value& metaValue,
                 bool includeTimeField,
                 bool includeMetaField) override;
    bool getNext(BSONObjBuilder& builder,
                 const BucketSpec& spec,
                 const BSONElement& metaValue,
                 bool includeTimeField,
                 bool includeMetaField) override;
    void extractSingleMeasurement(MutableDocument& measurement,
                                  int j,
                                  const BucketSpec& spec,
                                  const std::set<std::string>& unpackFieldsToIncludeExclude,
                                  const BSONObj& bucket,
                                  const Value& metaValue,
                                  bool includeTimeField,
                                  bool includeMetaField) override;
    std::size_t numberOfFields() override;

private:
    struct ColumnStore {
        ColumnStore(BSONElement elem)
            : column(elem),
              it(column.begin()),
              fieldName(elem.fieldNameStringData(), FieldNameHasher{}(elem.fieldNameStringData())) {
        }

        BSONColumn column;
        BSONColumn::Iterator it;
        HashedFieldName fieldName;
    };

    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    ColumnStore _timeColumn;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'BucketSpec'.
    std::vector<ColumnStore> _fieldColumns;

    // Element count
    int _elementCount;
};

BucketUnpackerV2::BucketUnpackerV2(const BSONElement& timeField, int elementCount)
    : _timeColumn(timeField), _elementCount(elementCount) {
    if (_elementCount == -1) {
        _elementCount = _timeColumn.column.size();
    }
}

void BucketUnpackerV2::addField(const BSONElement& field) {
    _fieldColumns.emplace_back(field);
}

int BucketUnpackerV2::measurementCount(const BSONElement& timeField) const {
    return _elementCount;
}

bool BucketUnpackerV2::getNext(MutableDocument& measurement,
                               const BucketSpec& spec,
                               const Value& metaValue,
                               bool includeTimeField,
                               bool includeMetaField) {
    // Get element and increment iterator
    const auto& timeElem = *_timeColumn.it;
    if (includeTimeField) {
        measurement.addField(spec.timeFieldHashed(), Value{timeElem});
    }
    ++_timeColumn.it;

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    for (auto& fieldColumn : _fieldColumns) {
        uassert(6067601,
                "Bucket unexpectedly contained fewer values than count",
                fieldColumn.it.more());
        const BSONElement& elem = *fieldColumn.it;
        // EOO represents missing field
        if (!elem.eoo()) {
            measurement.addField(fieldColumn.fieldName, Value{elem});
        }
        ++fieldColumn.it;
    }

    return _timeColumn.it.more();
}

bool BucketUnpackerV2::getNext(BSONObjBuilder& builder,
                               const BucketSpec& spec,
                               const BSONElement& metaValue,
                               bool includeTimeField,
                               bool includeMetaField) {
    // Get element and increment iterator
    const auto& timeElem = *_timeColumn.it;
    if (includeTimeField) {
        builder.appendAs(timeElem, spec.timeField());
    }
    ++_timeColumn.it;

    // Includes metaField when we're instructed to do so and metaField value exists.
    if (includeMetaField && !metaValue.eoo()) {
        builder.appendAs(metaValue, *spec.metaField());
    }

    for (auto& fieldColumn : _fieldColumns) {
        uassert(7026803,
                "Bucket unexpectedly contained fewer values than count",
                fieldColumn.it.more());
        const BSONElement& elem = *fieldColumn.it;
        // EOO represents missing field
        if (!elem.eoo()) {
            builder.appendAs(elem, fieldColumn.fieldName.key());
        }
        ++fieldColumn.it;
    }

    return _timeColumn.it.more();
}

void BucketUnpackerV2::extractSingleMeasurement(
    MutableDocument& measurement,
    int j,
    const BucketSpec& spec,
    const std::set<std::string>& unpackFieldsToIncludeExclude,
    const BSONObj& bucket,
    const Value& metaValue,
    bool includeTimeField,
    bool includeMetaField) {
    if (includeTimeField) {
        auto val = _timeColumn.column[j];
        uassert(
            6067500, "Bucket unexpectedly contained fewer values than count", val && !val->eoo());
        measurement.addField(spec.timeFieldHashed(), Value{*val});
    }

    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    if (includeTimeField) {
        for (auto& fieldColumn : _fieldColumns) {
            auto val = fieldColumn.column[j];
            uassert(6067600, "Bucket unexpectedly contained fewer values than count", val);
            measurement.addField(fieldColumn.fieldName, Value{*val});
        }
    }
}

std::size_t BucketUnpackerV2::numberOfFields() {
    // The data fields are tracked by _fieldColumns, but we need to account also for the time field
    // and possibly the meta field.
    return kFixedFieldNumber + _fieldColumns.size();
}

using BucketUnpackerV3 = BucketUnpackerV2;
}  // namespace

BucketUnpacker::BucketUnpacker() = default;
BucketUnpacker::BucketUnpacker(BucketUnpacker&& other) = default;
BucketUnpacker::~BucketUnpacker() = default;
BucketUnpacker& BucketUnpacker::operator=(BucketUnpacker&& rhs) = default;

BucketUnpacker::BucketUnpacker(BucketSpec spec) {
    setBucketSpec(std::move(spec));
}

void BucketUnpacker::addComputedMetaProjFields(const std::vector<StringData>& computedFieldNames) {
    for (auto&& field : computedFieldNames) {
        _spec.addComputedMetaProjFields(field);

        // If we're already specifically including fields, we need to add the computed fields to
        // the included field set to indicate they're in the output doc.
        if (_spec.behavior() == BucketSpec::Behavior::kInclude) {
            _spec.addIncludeExcludeField(field);
        } else {
            // Since exclude is applied after addComputedMetaProjFields, we must erase the new field
            // from the include/exclude fields so this doesn't get removed.
            _spec.removeIncludeExcludeField(std::string{field});
        }
    }

    // Recalculate _includeTimeField, since both computedMetaProjFields and fieldSet may have
    // changed.
    determineIncludeTimeField();
}

Document BucketUnpacker::getNext() {
    tassert(5521503, "'getNext()' requires the bucket to be owned", _bucket.isOwned());
    tassert(5422100, "'getNext()' was called after the bucket has been exhausted", hasNext());

    // MutableDocument reserves memory based on the number of fields, but uses a fixed size of 25
    // bytes plus an allowance of 7 characters for the field name. Doubling the number of fields
    // should give us enough overhead for longer field names without wasting too much memory.
    auto measurement = MutableDocument{2 * _unpackingImpl->numberOfFields()};
    _hasNext = _unpackingImpl->getNext(
        measurement, _spec, _metaValue, _includeTimeField, _includeMetaField);

    // Add computed meta projections.
    for (auto&& name : _spec.computedMetaProjFields()) {
        measurement.addField(name, Value{_computedMetaProjections[name]});
    }

    if (_includeMinTimeAsMetadata && _minTime) {
        measurement.metadata().setTimeseriesBucketMinTime(*_minTime);
    }

    if (_includeMaxTimeAsMetadata && _maxTime) {
        measurement.metadata().setTimeseriesBucketMaxTime(*_maxTime);
    }

    return measurement.freeze();
}

BSONObj BucketUnpacker::getNextBson() {
    tassert(7026800, "'getNextBson()' requires the bucket to be owned", _bucket.isOwned());
    tassert(7026801, "'getNextBson()' was called after the bucket has been exhausted", hasNext());
    tassert(7026802,
            "'getNextBson()' cannot return max and min time as metadata",
            !_includeMaxTimeAsMetadata && !_includeMinTimeAsMetadata);

    BSONObjBuilder builder;
    _hasNext = _unpackingImpl->getNext(
        builder, _spec, _metaBSONElem, _includeTimeField, _includeMetaField);

    // Add computed meta projections.
    for (auto&& name : _spec.computedMetaProjFields()) {
        if (_computedMetaProjections[name]) {
            builder.appendAs(_computedMetaProjections[name], name);
        }
    }

    return builder.obj();
}

Document BucketUnpacker::extractSingleMeasurement(int j) {
    tassert(5422101,
            "'extractSingleMeasurment' expects j to be greater than or equal to zero and less than "
            "or equal to the number of measurements in a bucket",
            j >= 0 && j < _numberOfMeasurements);

    auto measurement = MutableDocument{};
    _unpackingImpl->extractSingleMeasurement(measurement,
                                             j,
                                             _spec,
                                             fieldsToIncludeExcludeDuringUnpack(),
                                             _bucket,
                                             _metaValue,
                                             _includeTimeField,
                                             _includeMetaField);

    // Add computed meta projections.
    for (auto&& name : _spec.computedMetaProjFields()) {
        measurement.addField(name, Value{_computedMetaProjections[name]});
    }

    return measurement.freeze();
}

void BucketUnpacker::reset(BSONObj&& bucket, bool bucketMatchedQuery) {
    _unpackingImpl.reset();
    _bucket = std::move(bucket);
    _bucketMatchedQuery = bucketMatchedQuery;
    uassert(5346510, "An empty bucket cannot be unpacked", !_bucket.isEmpty());

    auto&& dataRegion = _bucket.getField(kBucketDataFieldName).Obj();
    if (dataRegion.isEmpty()) {
        // If the data field of a bucket is present but it holds an empty object, there's nothing to
        // unpack.
        return;
    }

    auto&& timeFieldElem = dataRegion.getField(_spec.timeField());
    uassert(5346700,
            "The $_internalUnpackBucket stage requires the data region to have a timeField object",
            timeFieldElem);

    _metaBSONElem = _bucket[kBucketMetaFieldName];
    _metaValue = Value{_metaBSONElem};
    if (_spec.metaField()) {
        // The spec indicates that there might be a metadata region. Missing metadata in
        // measurements is expressed with missing metadata in a bucket. But we disallow undefined
        // since the undefined BSON type is deprecated.
        uassert(5369600,
                "The $_internalUnpackBucket stage allows metadata to be absent or otherwise, it "
                "must not be the deprecated undefined bson type",
                _metaValue.missing() || _metaValue.getType() != BSONType::undefined);
    } else {
        // If the spec indicates that the time series collection has no metadata field, then we
        // should not find a metadata region in the underlying bucket documents.
        uassert(5369601,
                "The $_internalUnpackBucket stage expects buckets to have missing metadata regions "
                "if the metaField parameter is not provided",
                _metaValue.missing());
    }

    auto&& controlField = _bucket[kBucketControlFieldName];
    uassert(5857902,
            "The $_internalUnpackBucket stage requires 'control' object to be present",
            controlField && controlField.type() == BSONType::object);

    auto&& controlFieldObj = controlField.Obj();
    auto&& controlClosed = controlFieldObj[kBucketControlClosedFieldName];
    _closedBucket = controlClosed.booleanSafe();

    if (_includeMinTimeAsMetadata) {
        auto&& controlMin = controlFieldObj[kBucketControlMinFieldName];
        uassert(6460203,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << kControlMinFieldNamePrefix << "' object to be present",
                controlMin && controlMin.type() == BSONType::object);
        auto&& controlMinObj = controlMin.Obj();
        auto&& minTime = controlMinObj[_spec.timeField()];
        uassert(6460204,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << kControlMinFieldNamePrefix << "." << _spec.timeField()
                              << "' to be a date",
                minTime && minTime.type() == BSONType::date);
        _minTime = minTime.date();
    }

    if (_includeMaxTimeAsMetadata) {
        auto&& controlMax = controlFieldObj[kBucketControlMaxFieldName];
        uassert(6460205,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << kControlMaxFieldNamePrefix << "' object to be present",
                controlMax && controlMax.type() == BSONType::object);
        auto&& controlMaxObj = controlMax.Obj();
        auto&& maxTime = controlMaxObj[_spec.timeField()];
        uassert(6460206,
                str::stream() << "The $_internalUnpackBucket stage requires '"
                              << kControlMaxFieldNamePrefix << "." << _spec.timeField()
                              << "' to be a date",
                maxTime && maxTime.type() == BSONType::date);
        _maxTime = maxTime.date();
    }

    auto&& versionField = controlFieldObj[kBucketControlVersionFieldName];
    uassert(5857903,
            "The $_internalUnpackBucket stage requires 'control.version' field to be present",
            versionField && isNumericBSONType(versionField.type()));
    auto version = versionField.safeNumberInt();

    switch (version) {
        case kTimeseriesControlUncompressedVersion: {
            _unpackingImpl = std::make_unique<BucketUnpackerV1>(timeFieldElem);
            break;
        }
        case kTimeseriesControlCompressedSortedVersion: {
            auto countField = controlField.Obj()[kBucketControlCountFieldName];
            _unpackingImpl = std::make_unique<BucketUnpackerV2>(
                timeFieldElem,
                countField && isNumericBSONType(countField.type())
                    ? static_cast<int>(countField.Number())
                    : -1);
            break;
        }
        case kTimeseriesControlCompressedUnsortedVersion: {
            auto countField = controlField.Obj()[kBucketControlCountFieldName];
            _unpackingImpl = std::make_unique<BucketUnpackerV3>(
                timeFieldElem,
                countField && isNumericBSONType(countField.type())
                    ? static_cast<int>(countField.Number())
                    : -1);
            break;
        }
        default: {
            uasserted(5857900, "Invalid bucket version");
            break;
        }
    }

    // Walk the data region of the bucket, and decide if an iterator should be set up based on the
    // include or exclude case.
    for (auto&& elem : dataRegion) {
        auto colName = elem.fieldNameStringData();
        if (colName == _spec.timeField()) {
            // Skip adding a FieldIterator for the timeField since the timestamp value from
            // _timeFieldIter can be placed accordingly in the materialized measurement.
            continue;
        }

        // Includes a field when '_spec.behavior()' is 'kInclude' and it's found in 'fieldSet' or
        // _spec.behavior() is 'kExclude' and it's not found in 'fieldSet'.
        if (determineIncludeField(
                colName, _spec.behavior(), fieldsToIncludeExcludeDuringUnpack())) {
            _unpackingImpl->addField(elem);
        }
    }

    // Update computed meta projections with values from this bucket.
    for (auto&& name : _spec.computedMetaProjFields()) {
        _computedMetaProjections[name] = _bucket[name];
    }

    // Save the measurement count for the bucket.
    _numberOfMeasurements = _unpackingImpl->measurementCount(timeFieldElem);
    _hasNext = _numberOfMeasurements > 0;
}

void BucketUnpacker::determineIncludeTimeField() {
    const bool isInclude = _spec.behavior() == BucketSpec::Behavior::kInclude;
    const bool fieldSetContainsTime =
        _spec.fieldSet().find(_spec.timeField()) != _spec.fieldSet().end();

    const auto& metaProjFields = _spec.computedMetaProjFields();
    const bool metaProjContains = metaProjFields.find(_spec.timeField()) != metaProjFields.cend();

    // If computedMetaProjFields contains the time field, we exclude it from unpacking no matter
    // what, since it will be overwritten anyway.
    _includeTimeField = isInclude == fieldSetContainsTime && !metaProjContains;
}

void BucketUnpacker::eraseMetaFromFieldSetAndDetermineIncludeMeta() {
    if (!_spec.metaField() ||
        _spec.computedMetaProjFields().find(*_spec.metaField()) !=
            _spec.computedMetaProjFields().cend()) {
        _includeMetaField = false;
    } else if (auto itr = _spec.fieldSet().find(*_spec.metaField());
               itr != _spec.fieldSet().end()) {
        _spec.removeIncludeExcludeField(*_spec.metaField());
        _includeMetaField = _spec.behavior() == BucketSpec::Behavior::kInclude;
    } else {
        _includeMetaField = _spec.behavior() == BucketSpec::Behavior::kExclude;
    }
}

void BucketUnpacker::eraseUnneededComputedMetaProjFields() {
    //  If this is an inclusion spec and the current computed field is not part of in the include
    //  fields, it means the computed field should not be available after the current unpack stage.
    //  Similarly, for exclusion spec, if the current computed field is part of the exclude fields,
    //  the computed fields should not be available after the current unpack stage. This can happen
    //  if there was a $project stage after a $addFields stage.
    _spec.eraseIfPredTrueFromComputedMetaProjFields(
        [&](const std::string& field) { return !_spec.doesBucketSpecProvideField(field); });
}

void BucketUnpacker::setBucketSpec(BucketSpec&& bucketSpec) {
    _spec = std::move(bucketSpec);

    eraseMetaFromFieldSetAndDetermineIncludeMeta();
    determineIncludeTimeField();
    eraseUnneededComputedMetaProjFields();

    _includeMinTimeAsMetadata = _spec.includeMinTimeAsMetadata;
    _includeMaxTimeAsMetadata = _spec.includeMaxTimeAsMetadata;
}

void BucketUnpacker::setIncludeMinTimeAsMetadata() {
    _includeMinTimeAsMetadata = true;
}

void BucketUnpacker::setIncludeMaxTimeAsMetadata() {
    _includeMaxTimeAsMetadata = true;
}

const std::set<std::string>& BucketUnpacker::fieldsToIncludeExcludeDuringUnpack() {
    if (_unpackFieldsToIncludeExclude) {
        return *_unpackFieldsToIncludeExclude;
    }

    _unpackFieldsToIncludeExclude = std::set<std::string>();
    const auto& metaProjFields = _spec.computedMetaProjFields();
    if (_spec.behavior() == BucketSpec::Behavior::kInclude) {
        // For include, we unpack fieldSet - metaProjFields.
        for (auto&& field : _spec.fieldSet()) {
            if (metaProjFields.find(field) == metaProjFields.cend()) {
                _unpackFieldsToIncludeExclude->insert(field);
            }
        }
    } else {
        // For exclude, we unpack everything but fieldSet + metaProjFields.
        _unpackFieldsToIncludeExclude->insert(_spec.fieldSet().begin(), _spec.fieldSet().end());
        _unpackFieldsToIncludeExclude->insert(metaProjFields.begin(), metaProjFields.end());
    }

    return *_unpackFieldsToIncludeExclude;
}

const std::set<StringData> BucketUnpacker::reservedBucketFieldNames = {
    kBucketIdFieldName, kBucketDataFieldName, kBucketMetaFieldName, kBucketControlFieldName};

}  // namespace mongo::timeseries
