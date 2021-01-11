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

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(_internalUnpackBucket,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalUnpackBucket::createFromBson);

void BucketUnpacker::reset(Document&& bucket) {
    _fieldIters.clear();
    _timeFieldIter = boost::none;

    _bucket = std::move(bucket);
    uassert(5346510, "An empty bucket cannot be unpacked", !_bucket.empty());

    if (_bucket[kBucketDataFieldName].getDocument().empty()) {
        // If the data field of a bucket is present but it holds an empty object, there's nothing to
        // unpack.
        return;
    }

    _metaValue = _bucket[kBucketMetaFieldName];
    uassert(5346511,
            "A metadata value cannot be undefined nor missing if metaField is specified",
            !_spec.metaField ||
                (_metaValue.getType() != BSONType::Undefined && !_metaValue.missing()));

    _timeFieldIter = _bucket[kBucketDataFieldName][_spec.timeField].getDocument().fieldIterator();

    // Walk the data region of the bucket, and decide if an iterator should be set up based on the
    // include or exclude case.
    auto colIter = _bucket[kBucketDataFieldName].getDocument().fieldIterator();
    while (colIter.more()) {
        auto&& [colName, colVal] = colIter.next();
        if (colName == _spec.timeField) {
            // Skip adding a FieldIterator for the timeField since the timestamp value from
            // _timeFieldIter can be placed accordingly in the materialized measurement.
            continue;
        }
        auto found = _spec.fieldSet.find(colName.toString()) != _spec.fieldSet.end();
        if ((_unpackerBehavior == Behavior::kInclude) == found) {
            _fieldIters.push_back({colName.toString(), colVal.getDocument().fieldIterator()});
        }
    }
}

Document BucketUnpacker::getNext() {
    invariant(hasNext());

    auto measurement = MutableDocument{};

    auto&& [currentIdx, timeVal] = _timeFieldIter->next();
    if (_includeTimeField) {
        measurement.addField(_spec.timeField, timeVal);
    }

    if (!_metaValue.nullish()) {
        measurement.addField(*_spec.metaField, _metaValue);
    }

    for (auto&& [colName, colIter] : _fieldIters) {
        if (colIter.more() && colIter.fieldName() == currentIdx) {
            auto&& [_, val] = colIter.next();
            measurement.addField(colName, val);
        }
    }

    return measurement.freeze();
}

DocumentSourceInternalUnpackBucket::DocumentSourceInternalUnpackBucket(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, BucketUnpacker bucketUnpacker)
    : DocumentSource(kStageName, expCtx), _bucketUnpacker(std::move(bucketUnpacker)) {}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalUnpackBucket::createFromBson(
    BSONElement specElem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5346500,
            "$_internalUnpackBucket specification must be an object",
            specElem.type() == Object);

    BucketUnpacker::Behavior unpackerBehavior;
    BucketSpec bucketSpec;
    auto hasIncludeExclude = false;
    std::vector<std::string> fields;
    for (auto&& elem : specElem.embeddedObject()) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "include" || fieldName == "exclude") {
            uassert(5346501,
                    "include or exclude field must be an array",
                    elem.type() == BSONType::Array);

            for (auto&& elt : elem.embeddedObject()) {
                uassert(5346502,
                        "include or exclude field element must be a string",
                        elt.type() == BSONType::String);
                auto field = elt.valueStringData();
                uassert(5346503,
                        "include or exclude field element must be a single-element field path",
                        field.find('.') == std::string::npos);
                bucketSpec.fieldSet.emplace(field);
            }
            unpackerBehavior = fieldName == "include" ? BucketUnpacker::Behavior::kInclude
                                                      : BucketUnpacker::Behavior::kExclude;
            hasIncludeExclude = true;
        } else if (fieldName == kTimeFieldName) {
            uassert(5346504, "timeField field must be a string", elem.type() == BSONType::String);
            bucketSpec.timeField = elem.str();
        } else if (fieldName == kMetaFieldName) {
            uassert(5346505,
                    str::stream() << "metaField field must be a string, got: " << elem.type(),
                    elem.type() == BSONType::String);
            bucketSpec.metaField = elem.str();
        } else {
            uasserted(5346506,
                      str::stream()
                          << "unrecognized parameter to $_internalUnpackBucket: " << fieldName);
        }
    }

    // Check that none of the required arguments are missing.
    uassert(5346507,
            "The $_internalUnpackBucket stage requries an include/exclude parameter",
            hasIncludeExclude);

    uassert(5346508,
            "The $_internalUnpackBucket stage requires a timeField parameter",
            specElem[kTimeFieldName].ok());

    // Determine if timestamp values should be included in the materialized measurements.
    auto includeTimeField = (unpackerBehavior == BucketUnpacker::Behavior::kInclude) ==
        (bucketSpec.fieldSet.find(bucketSpec.timeField) != bucketSpec.fieldSet.end());

    return make_intrusive<DocumentSourceInternalUnpackBucket>(
        expCtx, BucketUnpacker{std::move(bucketSpec), unpackerBehavior, includeTimeField});
}

Value DocumentSourceInternalUnpackBucket::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument out;
    auto behavior =
        _bucketUnpacker.behavior() == BucketUnpacker::Behavior::kInclude ? kInclude : kExclude;
    auto&& spec = _bucketUnpacker.bucketSpec();
    std::vector<Value> fields;
    for (auto&& field : spec.fieldSet) {
        fields.emplace_back(field);
    }
    out.addField(behavior, Value{fields});
    out.addField(kTimeFieldName, Value{spec.timeField});
    if (spec.metaField) {
        out.addField(kMetaFieldName, Value{*spec.metaField});
    }
    return Value(DOC(getSourceName() << out.freeze()));
}

DocumentSource::GetNextResult DocumentSourceInternalUnpackBucket::doGetNext() {
    if (_bucketUnpacker.hasNext()) {
        return _bucketUnpacker.getNext();
    }

    auto nextResult = pSource->getNext();
    if (nextResult.isAdvanced()) {
        auto bucket = nextResult.getDocument();
        _bucketUnpacker.reset(std::move(bucket));
        uassert(
            5346509,
            str::stream() << "A bucket with _id "
                          << _bucketUnpacker.bucket()[BucketUnpacker::kBucketIdFieldName].toString()
                          << " contains an empty data region",
            _bucketUnpacker.hasNext());
        return _bucketUnpacker.getNext();
    }

    return nextResult;
}
}  // namespace mongo
