/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_metadata_fields.h"

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

namespace {
Value missingToNull(Value maybeMissing) {
    return maybeMissing.missing() ? Value(BSONNULL) : maybeMissing;
}
}  // namespace

DocumentMetadataFields::DocumentMetadataFields(const DocumentMetadataFields& other)
    : _holder(other._holder ? std::make_unique<MetadataHolder>(*other._holder) : nullptr) {}

DocumentMetadataFields& DocumentMetadataFields::operator=(const DocumentMetadataFields& other) {
    _holder = other._holder ? std::make_unique<MetadataHolder>(*other._holder) : nullptr;
    return *this;
}

DocumentMetadataFields::DocumentMetadataFields(DocumentMetadataFields&& other)
    : _holder(std::move(other._holder)) {}

DocumentMetadataFields& DocumentMetadataFields::operator=(DocumentMetadataFields&& other) {
    _holder = std::move(other._holder);
    return *this;
}

void DocumentMetadataFields::mergeWith(const DocumentMetadataFields& other) {
    if (!hasTextScore() && other.hasTextScore()) {
        setTextScore(other.getTextScore());
    }
    if (!hasRandVal() && other.hasRandVal()) {
        setRandVal(other.getRandVal());
    }
    if (!hasSortKey() && other.hasSortKey()) {
        setSortKey(other.getSortKey(), other.isSingleElementKey());
    }
    if (!hasGeoNearDistance() && other.hasGeoNearDistance()) {
        setGeoNearDistance(other.getGeoNearDistance());
    }
    if (!hasGeoNearPoint() && other.hasGeoNearPoint()) {
        setGeoNearPoint(other.getGeoNearPoint());
    }
    if (!hasSearchScore() && other.hasSearchScore()) {
        setSearchScore(other.getSearchScore());
    }
    if (!hasSearchHighlights() && other.hasSearchHighlights()) {
        setSearchHighlights(other.getSearchHighlights());
    }
    if (!hasIndexKey() && other.hasIndexKey()) {
        setIndexKey(other.getIndexKey());
    }
}

void DocumentMetadataFields::copyFrom(const DocumentMetadataFields& other) {
    if (other.hasTextScore()) {
        setTextScore(other.getTextScore());
    }
    if (other.hasRandVal()) {
        setRandVal(other.getRandVal());
    }
    if (other.hasSortKey()) {
        setSortKey(other.getSortKey(), other.isSingleElementKey());
    }
    if (other.hasGeoNearDistance()) {
        setGeoNearDistance(other.getGeoNearDistance());
    }
    if (other.hasGeoNearPoint()) {
        setGeoNearPoint(other.getGeoNearPoint());
    }
    if (other.hasSearchScore()) {
        setSearchScore(other.getSearchScore());
    }
    if (other.hasSearchHighlights()) {
        setSearchHighlights(other.getSearchHighlights());
    }
    if (other.hasIndexKey()) {
        setIndexKey(other.getIndexKey());
    }
}

size_t DocumentMetadataFields::getApproximateSize() const {
    if (!_holder) {
        return 0;
    }

    // Purposefully exclude the size of the DocumentMetadataFields, as this is accounted for
    // elsewhere. Here we only consider the "deep" size of the MetadataHolder.
    size_t size = sizeof(MetadataHolder);

    // Count the "deep" portion of the metadata values.
    size += _holder->sortKey.getApproximateSize();
    size += _holder->geoNearPoint.getApproximateSize();
    // Size of Value is double counted - once in sizeof(MetadataFields) and once in
    // getApproximateSize()
    size -= sizeof(_holder->geoNearPoint);
    size += _holder->searchHighlights.getApproximateSize();
    size -= sizeof(_holder->searchHighlights);
    size += _holder->indexKey.objsize();

    return size;
}

void DocumentMetadataFields::serializeForSorter(BufBuilder& buf) const {
    // If there is no metadata, all we need to do is write a zero byte.
    if (!_holder) {
        buf.appendNum(static_cast<char>(0));
        return;
    }

    if (hasTextScore()) {
        buf.appendNum(static_cast<char>(MetaType::kTextScore + 1));
        buf.appendNum(getTextScore());
    }
    if (hasRandVal()) {
        buf.appendNum(static_cast<char>(MetaType::kRandVal + 1));
        buf.appendNum(getRandVal());
    }
    if (hasSortKey()) {
        buf.appendNum(static_cast<char>(MetaType::kSortKey + 1));
        buf.appendChar(isSingleElementKey() ? 1 : 0);
        getSortKey().serializeForSorter(buf);
    }
    if (hasGeoNearDistance()) {
        buf.appendNum(static_cast<char>(MetaType::kGeoNearDist + 1));
        buf.appendNum(getGeoNearDistance());
    }
    if (hasGeoNearPoint()) {
        buf.appendNum(static_cast<char>(MetaType::kGeoNearPoint + 1));
        getGeoNearPoint().serializeForSorter(buf);
    }
    if (hasSearchScore()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchScore + 1));
        buf.appendNum(getSearchScore());
    }
    if (hasSearchHighlights()) {
        buf.appendNum(static_cast<char>(MetaType::kSearchHighlights + 1));
        getSearchHighlights().serializeForSorter(buf);
    }
    if (hasIndexKey()) {
        buf.appendNum(static_cast<char>(MetaType::kIndexKey + 1));
        getIndexKey().appendSelfToBufBuilder(buf);
    }
    buf.appendNum(static_cast<char>(0));
}

void DocumentMetadataFields::deserializeForSorter(BufReader& buf, DocumentMetadataFields* out) {
    invariant(out);

    while (char marker = buf.read<char>()) {
        if (marker == static_cast<char>(MetaType::kTextScore) + 1) {
            out->setTextScore(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kRandVal) + 1) {
            out->setRandVal(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kSortKey) + 1) {
            char isSingleElementKey = buf.read<char>();
            out->setSortKey(Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()),
                            isSingleElementKey);
        } else if (marker == static_cast<char>(MetaType::kGeoNearDist) + 1) {
            out->setGeoNearDistance(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kGeoNearPoint) + 1) {
            out->setGeoNearPoint(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kSearchScore) + 1) {
            out->setSearchScore(buf.read<LittleEndian<double>>());
        } else if (marker == static_cast<char>(MetaType::kSearchHighlights) + 1) {
            out->setSearchHighlights(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else if (marker == static_cast<char>(MetaType::kIndexKey) + 1) {
            out->setIndexKey(
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings()));
        } else {
            uasserted(28744, "Unrecognized marker, unable to deserialize buffer");
        }
    }
}

BSONObj DocumentMetadataFields::serializeSortKey(bool isSingleElementKey, const Value& value) {
    // Missing values don't serialize correctly in this format, so use nulls instead, since they are
    // considered equivalent with woCompare().
    if (isSingleElementKey) {
        return BSON("" << missingToNull(value));
    }
    invariant(value.isArray());
    BSONObjBuilder bb;
    for (auto&& val : value.getArray()) {
        bb << "" << missingToNull(val);
    }
    return bb.obj();
}

Value DocumentMetadataFields::deserializeSortKey(bool isSingleElementKey,
                                                 const BSONObj& bsonSortKey) {
    std::vector<Value> keys;
    for (auto&& elt : bsonSortKey) {
        keys.push_back(Value{elt});
    }
    if (isSingleElementKey) {
        // As a special case for a sort on a single field, we do not put the keys into an array.
        return keys[0];
    }
    return Value{std::move(keys)};
}

}  // namespace mongo
