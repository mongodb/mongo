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

#include "mongo/db/exec/bucket_unpacker.h"

#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/timeseries/timeseries_constants.h"

namespace mongo {
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
    virtual void extractSingleMeasurement(MutableDocument& measurement,
                                          int j,
                                          const BucketSpec& spec,
                                          const std::set<std::string>& unpackFieldsToIncludeExclude,
                                          BucketUnpacker::Behavior behavior,
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
    // A table that is useful for interpolations between the number of measurements in a bucket and
    // the byte size of a bucket's data section timestamp column. Each table entry is a pair (b_i,
    // S_i), where b_i is the number of measurements in the bucket and S_i is the byte size of the
    // timestamp BSONObj. The table is bounded by 16 MB (2 << 23 bytes) where the table entries are
    // pairs of b_i and S_i for the lower bounds of the row key digit intervals [0, 9], [10, 99],
    // [100, 999], [1000, 9999] and so on. The last entry in the table, S7, is the first entry to
    // exceed the server BSON object limit of 16 MB.
    static constexpr std::array<std::pair<int32_t, int32_t>, 8> kTimestampObjSizeTable{
        {{0, BSONObj::kMinBSONLength},
         {10, 115},
         {100, 1195},
         {1000, 12895},
         {10000, 138895},
         {100000, 1488895},
         {1000000, 15888895},
         {10000000, 168888895}}};

    static int computeElementCountFromTimestampObjSize(int targetTimestampObjSize);

    BucketUnpackerV1(const BSONElement& timeField);

    void addField(const BSONElement& field) override;
    int measurementCount(const BSONElement& timeField) const override;
    bool getNext(MutableDocument& measurement,
                 const BucketSpec& spec,
                 const Value& metaValue,
                 bool includeTimeField,
                 bool includeMetaField) override;
    void extractSingleMeasurement(MutableDocument& measurement,
                                  int j,
                                  const BucketSpec& spec,
                                  const std::set<std::string>& unpackFieldsToIncludeExclude,
                                  BucketUnpacker::Behavior behavior,
                                  const BSONObj& bucket,
                                  const Value& metaValue,
                                  bool includeTimeField,
                                  bool includeMetaField) override;
    std::size_t numberOfFields() override;

private:
    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    BSONObjIterator _timeFieldIter;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'Behavior' and 'BucketSpec'.
    std::vector<std::pair<std::string, BSONObjIterator>> _fieldIters;
};

// Calculates the number of measurements in a bucket given the 'targetTimestampObjSize' using the
// 'BucketUnpacker::kTimestampObjSizeTable' table. If the 'targetTimestampObjSize' hits a record in
// the table, this helper returns the measurement count corresponding to the table record.
// Otherwise, the 'targetTimestampObjSize' is used to probe the table for the smallest {b_i, S_i}
// pair such that 'targetTimestampObjSize' < S_i. Once the interval is found, the upper bound of the
// pair for the interval is computed and then linear interpolation is used to compute the
// measurement count corresponding to the 'targetTimestampObjSize' provided.
int BucketUnpackerV1::computeElementCountFromTimestampObjSize(int targetTimestampObjSize) {
    auto currentInterval =
        std::find_if(std::begin(BucketUnpackerV1::kTimestampObjSizeTable),
                     std::end(BucketUnpackerV1::kTimestampObjSizeTable),
                     [&](const auto& entry) { return targetTimestampObjSize <= entry.second; });

    if (currentInterval->second == targetTimestampObjSize) {
        return currentInterval->first;
    }
    // This points to the first interval larger than the target 'targetTimestampObjSize', the actual
    // interval that will cover the object size is the interval before the current one.
    tassert(5422104,
            "currentInterval should not point to the first table entry",
            currentInterval > BucketUnpackerV1::kTimestampObjSizeTable.begin());
    --currentInterval;

    auto nDigitsInRowKey = 1 + (currentInterval - BucketUnpackerV1::kTimestampObjSizeTable.begin());

    return currentInterval->first +
        ((targetTimestampObjSize - currentInterval->second) / (10 + nDigitsInRowKey));
}

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

    auto& currentIdx = timeElem.fieldNameStringData();
    for (auto&& [colName, colIter] : _fieldIters) {
        if (auto&& elem = *colIter; colIter.more() && elem.fieldNameStringData() == currentIdx) {
            measurement.addField(colName, Value{elem});
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
    BucketUnpacker::Behavior behavior,
    const BSONObj& bucket,
    const Value& metaValue,
    bool includeTimeField,
    bool includeMetaField) {
    auto rowKey = std::to_string(j);
    auto targetIdx = StringData{rowKey};
    auto&& dataRegion = bucket.getField(timeseries::kBucketDataFieldName).Obj();

    if (includeMetaField && !metaValue.missing()) {
        measurement.addField(*spec.metaFieldHashed(), metaValue);
    }

    for (auto&& dataElem : dataRegion) {
        auto colName = dataElem.fieldNameStringData();
        if (!determineIncludeField(colName, behavior, unpackFieldsToIncludeExclude)) {
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
    void extractSingleMeasurement(MutableDocument& measurement,
                                  int j,
                                  const BucketSpec& spec,
                                  const std::set<std::string>& unpackFieldsToIncludeExclude,
                                  BucketUnpacker::Behavior behavior,
                                  const BSONObj& bucket,
                                  const Value& metaValue,
                                  bool includeTimeField,
                                  bool includeMetaField) override;
    std::size_t numberOfFields() override;

private:
    struct ColumnStore {
        ColumnStore(BSONElement elem) : column(elem), it(column.begin()), end(column.end()) {}
        ColumnStore(ColumnStore&& other)
            : column(std::move(other.column)), it(other.it.moveTo(column)), end(other.end) {}

        BSONColumn column;
        BSONColumn::Iterator it;
        BSONColumn::Iterator end;
    };

    // Iterates the timestamp section of the bucket to drive the unpacking iteration.
    ColumnStore _timeColumn;

    // Iterators used to unpack the columns of the above bucket that are populated during the reset
    // phase according to the provided 'Behavior' and 'BucketSpec'.
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
                fieldColumn.it != fieldColumn.end);
        const BSONElement& elem = *fieldColumn.it;
        // EOO represents missing field
        if (!elem.eoo()) {
            measurement.addField(HashedFieldName{fieldColumn.column.nameHashed()}, Value{elem});
        }
        ++fieldColumn.it;
    }

    return _timeColumn.it != _timeColumn.end;
}

void BucketUnpackerV2::extractSingleMeasurement(
    MutableDocument& measurement,
    int j,
    const BucketSpec& spec,
    const std::set<std::string>& unpackFieldsToIncludeExclude,
    BucketUnpacker::Behavior behavior,
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
            measurement.addField(HashedFieldName{fieldColumn.column.nameHashed()}, Value{*val});
        }
    }
}

std::size_t BucketUnpackerV2::numberOfFields() {
    // The data fields are tracked by _fieldColumns, but we need to account also for the time field
    // and possibly the meta field.
    return kFixedFieldNumber + _fieldColumns.size();
}

}  // namespace

BucketSpec::BucketSpec(const std::string& timeField,
                       const boost::optional<std::string>& metaField,
                       const std::set<std::string>& fields,
                       const std::set<std::string>& computedProjections)
    : _fieldSet(fields),
      _computedMetaProjFields(computedProjections),
      _timeField(timeField),
      _timeFieldHashed(FieldNameHasher().hashedFieldName(_timeField)),
      _metaField(metaField) {
    if (_metaField) {
        _metaFieldHashed = FieldNameHasher().hashedFieldName(*_metaField);
    }
}

BucketSpec::BucketSpec(const BucketSpec& other)
    : _fieldSet(other._fieldSet),
      _computedMetaProjFields(other._computedMetaProjFields),
      _timeField(other._timeField),
      _timeFieldHashed(HashedFieldName{_timeField, other._timeFieldHashed->hash()}),
      _metaField(other._metaField) {
    if (_metaField) {
        _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
    }
}

BucketSpec::BucketSpec(BucketSpec&& other)
    : _fieldSet(std::move(other._fieldSet)),
      _computedMetaProjFields(std::move(other._computedMetaProjFields)),
      _timeField(std::move(other._timeField)),
      _timeFieldHashed(HashedFieldName{_timeField, other._timeFieldHashed->hash()}),
      _metaField(std::move(other._metaField)) {
    if (_metaField) {
        _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
    }
}

BucketSpec& BucketSpec::operator=(const BucketSpec& other) {
    if (&other != this) {
        _fieldSet = other._fieldSet;
        _computedMetaProjFields = other._computedMetaProjFields;
        _timeField = other._timeField;
        _timeFieldHashed = HashedFieldName{_timeField, other._timeFieldHashed->hash()};
        _metaField = other._metaField;
        if (_metaField) {
            _metaFieldHashed = HashedFieldName{*_metaField, other._metaFieldHashed->hash()};
        }
    }
    return *this;
}

void BucketSpec::setTimeField(std::string&& name) {
    _timeField = std::move(name);
    _timeFieldHashed = FieldNameHasher().hashedFieldName(_timeField);
}

const std::string& BucketSpec::timeField() const {
    return _timeField;
}

HashedFieldName BucketSpec::timeFieldHashed() const {
    invariant(_timeFieldHashed->key().rawData() == _timeField.data());
    invariant(_timeFieldHashed->key() == _timeField);
    return *_timeFieldHashed;
}

void BucketSpec::setMetaField(boost::optional<std::string>&& name) {
    _metaField = std::move(name);
    if (_metaField) {
        _metaFieldHashed = FieldNameHasher().hashedFieldName(*_metaField);
    } else {
        _metaFieldHashed = boost::none;
    }
}

const boost::optional<std::string>& BucketSpec::metaField() const {
    return _metaField;
}

boost::optional<HashedFieldName> BucketSpec::metaFieldHashed() const {
    return _metaFieldHashed;
}

BucketUnpacker::BucketUnpacker() = default;
BucketUnpacker::BucketUnpacker(BucketUnpacker&& other) = default;
BucketUnpacker::~BucketUnpacker() = default;
BucketUnpacker& BucketUnpacker::operator=(BucketUnpacker&& rhs) = default;

BucketUnpacker::BucketUnpacker(BucketSpec spec, Behavior unpackerBehavior) {
    setBucketSpecAndBehavior(std::move(spec), unpackerBehavior);
}

void BucketUnpacker::addComputedMetaProjFields(const std::vector<StringData>& computedFieldNames) {
    for (auto&& field : computedFieldNames) {
        _spec.addComputedMetaProjFields(field);

        // If we're already specifically including fields, we need to add the computed fields to
        // the included field set to indicate they're in the output doc.
        if (_unpackerBehavior == BucketUnpacker::Behavior::kInclude) {
            _spec.addIncludeExcludeField(field);
        } else {
            // Since exclude is applied after addComputedMetaProjFields, we must erase the new field
            // from the include/exclude fields so this doesn't get removed.
            _spec.removeIncludeExcludeField(field.toString());
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

    return measurement.freeze();
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
                                             _unpackerBehavior,
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

void BucketUnpacker::reset(BSONObj&& bucket) {
    _unpackingImpl.reset();
    _bucket = std::move(bucket);
    uassert(5346510, "An empty bucket cannot be unpacked", !_bucket.isEmpty());

    auto&& dataRegion = _bucket.getField(timeseries::kBucketDataFieldName).Obj();
    if (dataRegion.isEmpty()) {
        // If the data field of a bucket is present but it holds an empty object, there's nothing to
        // unpack.
        return;
    }

    auto&& timeFieldElem = dataRegion.getField(_spec.timeField());
    uassert(5346700,
            "The $_internalUnpackBucket stage requires the data region to have a timeField object",
            timeFieldElem);

    _metaValue = Value{_bucket[timeseries::kBucketMetaFieldName]};
    if (_spec.metaField()) {
        // The spec indicates that there might be a metadata region. Missing metadata in
        // measurements is expressed with missing metadata in a bucket. But we disallow undefined
        // since the undefined BSON type is deprecated.
        uassert(5369600,
                "The $_internalUnpackBucket stage allows metadata to be absent or otherwise, it "
                "must not be the deprecated undefined bson type",
                _metaValue.missing() || _metaValue.getType() != BSONType::Undefined);
    } else {
        // If the spec indicates that the time series collection has no metadata field, then we
        // should not find a metadata region in the underlying bucket documents.
        uassert(5369601,
                "The $_internalUnpackBucket stage expects buckets to have missing metadata regions "
                "if the metaField parameter is not provided",
                _metaValue.missing());
    }


    auto&& controlField = _bucket[timeseries::kBucketControlFieldName];
    uassert(5857902,
            "The $_internalUnpackBucket stage requires 'control' object to be present",
            controlField && controlField.type() == BSONType::Object);

    auto&& versionField = controlField.Obj()[timeseries::kBucketControlVersionFieldName];
    uassert(5857903,
            "The $_internalUnpackBucket stage requires 'control.version' field to be present",
            versionField && isNumericBSONType(versionField.type()));
    auto version = versionField.Number();

    if (version == 1) {
        _unpackingImpl = std::make_unique<BucketUnpackerV1>(timeFieldElem);
    } else if (version == 2) {
        auto countField = controlField.Obj()[timeseries::kBucketControlCountFieldName];
        _unpackingImpl =
            std::make_unique<BucketUnpackerV2>(timeFieldElem,
                                               countField && isNumericBSONType(countField.type())
                                                   ? static_cast<int>(countField.Number())
                                                   : -1);
    } else {
        uasserted(5857900, "Invalid bucket version");
    }

    // Walk the data region of the bucket, and decide if an iterator should be set up based on the
    // include or exclude case.
    for (auto&& elem : dataRegion) {
        auto& colName = elem.fieldNameStringData();
        if (colName == _spec.timeField()) {
            // Skip adding a FieldIterator for the timeField since the timestamp value from
            // _timeFieldIter can be placed accordingly in the materialized measurement.
            continue;
        }

        // Includes a field when '_unpackerBehavior' is 'kInclude' and it's found in 'fieldSet' or
        // _unpackerBehavior is 'kExclude' and it's not found in 'fieldSet'.
        if (determineIncludeField(
                colName, _unpackerBehavior, fieldsToIncludeExcludeDuringUnpack())) {
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

int BucketUnpacker::computeMeasurementCount(const BSONObj& bucket, StringData timeField) {
    auto&& controlField = bucket[timeseries::kBucketControlFieldName];
    uassert(5857904,
            "The $_internalUnpackBucket stage requires 'control' object to be present",
            controlField && controlField.type() == BSONType::Object);

    auto&& versionField = controlField.Obj()[timeseries::kBucketControlVersionFieldName];
    uassert(5857905,
            "The $_internalUnpackBucket stage requires 'control.version' field to be present",
            versionField && isNumericBSONType(versionField.type()));

    auto&& dataField = bucket[timeseries::kBucketDataFieldName];
    if (!dataField || dataField.type() != BSONType::Object)
        return 0;

    auto&& time = dataField.Obj()[timeField];
    if (!time) {
        return 0;
    }

    auto version = versionField.Number();
    if (version == 1) {
        return BucketUnpackerV1::computeElementCountFromTimestampObjSize(time.objsize());
    } else if (version == 2) {
        auto countField = controlField.Obj()[timeseries::kBucketControlCountFieldName];
        if (countField && isNumericBSONType(countField.type())) {
            return static_cast<int>(countField.Number());
        }

        return BSONColumn(time).size();
    } else {
        uasserted(5857901, "Invalid bucket version");
    }
}

void BucketUnpacker::determineIncludeTimeField() {
    const bool isInclude = _unpackerBehavior == BucketUnpacker::Behavior::kInclude;
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
        _includeMetaField = _unpackerBehavior == BucketUnpacker::Behavior::kInclude;
    } else {
        _includeMetaField = _unpackerBehavior == BucketUnpacker::Behavior::kExclude;
    }
}

void BucketUnpacker::eraseExcludedComputedMetaProjFields() {
    if (_unpackerBehavior == BucketUnpacker::Behavior::kExclude) {
        for (const auto& field : _spec.fieldSet()) {
            _spec.eraseFromComputedMetaProjFields(field);
        }
    }
}

void BucketUnpacker::setBucketSpecAndBehavior(BucketSpec&& bucketSpec, Behavior behavior) {
    _unpackerBehavior = behavior;
    _spec = std::move(bucketSpec);

    eraseMetaFromFieldSetAndDetermineIncludeMeta();
    determineIncludeTimeField();
    eraseExcludedComputedMetaProjFields();
}

const std::set<std::string>& BucketUnpacker::fieldsToIncludeExcludeDuringUnpack() {
    if (_unpackFieldsToIncludeExclude) {
        return *_unpackFieldsToIncludeExclude;
    }

    _unpackFieldsToIncludeExclude = std::set<std::string>();
    const auto& metaProjFields = _spec.computedMetaProjFields();
    if (_unpackerBehavior == BucketUnpacker::Behavior::kInclude) {
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
    timeseries::kBucketIdFieldName,
    timeseries::kBucketDataFieldName,
    timeseries::kBucketMetaFieldName,
    timeseries::kBucketControlFieldName};

}  // namespace mongo
