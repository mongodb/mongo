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

#pragma once

#include <bitset>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/record_id.h"

namespace mongo {
/**
 * This class represents the metadata that the query execution engine can associate with a
 * particular intermediate result (either index key or document) passing between execution stages.
 *
 * Since most documents do not have metadata, this class can be left in an uninitialized state, in
 * which case no memory is allocated to hold the metadata. The operator bool() overload can be used
 * to determine whether or not a DocumentMetadataFields object is uninitialized (and thus has no
 * metadata).
 *
 * Calling any of the setters is legal on an uninitialized object, and will cause the
 * DocumentMetadataFields to transition to an initialized state.
 *
 * A DocumentMetadataFields is copy constructible, copy assignable, move constructible, and move
 * assignable.
 */
class DocumentMetadataFields {
public:
    enum MetaType : char {
        // Start from 1 so that these values can be stored in a bitset.
        kGeoNearDist = 1,
        kGeoNearPoint,
        kIndexKey,
        kRandVal,
        kRecordId,
        kSearchHighlights,
        kSearchScore,
        kSortKey,
        kTextScore,
        kSearchScoreDetails,
        kTimeseriesBucketMinTime,
        kTimeseriesBucketMaxTime,

        // New fields must be added before the kNumFields sentinel.
        kNumFields
    };

    /**
     * Reads serialized metadata out of 'buf', and uses it to populate 'out'. Expects 'buf' to have
     * been written to by a previous call to serializeForSorter(). It is illegal to pass a null
     * pointer for 'out'.
     */
    static void deserializeForSorter(BufReader& buf, DocumentMetadataFields* out);

    /**
     * Converts a Value representing an in-memory sort key to a BSONArray representing a serialized
     * sort key. If 'isSingleElementKey' is true, returns a BSONArray with 'value' as its only
     * element. Otherwise, converts 'value' (which is expected to be an Array) to a BSONArray. Any
     * Value elements whose value is "missing" get converted to BSONNull.
     */
    static BSONArray serializeSortKey(bool isSingleElementKey, const Value& value);

    /**
     * Converts a BSONObj representing a serialized sort key into a Value, which we use for
     * in-memory comparisons. BSONObj {'': 1, '': [2, 3]} becomes Value [1, [2, 3]].
     */
    static Value deserializeSortKey(bool isSingleElementKey, const BSONObj& bsonSortKey);

    /**
     * Given a metadata type, return a (debug) string representation.
     */
    static const char* typeNameToDebugString(DocumentMetadataFields::MetaType type);

    /**
     * Constructs a new DocumentMetadataFields in an uninitialized state.
     */
    DocumentMetadataFields() = default;

    DocumentMetadataFields(const DocumentMetadataFields& other);
    DocumentMetadataFields& operator=(const DocumentMetadataFields& other);

    DocumentMetadataFields(DocumentMetadataFields&& other);
    DocumentMetadataFields& operator=(DocumentMetadataFields&& other);

    /**
     * For all metadata fields that 'other' has but 'this' does not have, copies these fields from
     * 'other' to 'this'.
     */
    void mergeWith(const DocumentMetadataFields& other);

    /**
     * Copies all metadata fields that are present in 'other' from 'other' to 'this', overwriting
     * values already present in 'this'.
     *
     * This differs slightly from the copy assignment operator. Copy-assignment will cause 'this' to
     * equal 'other' exactly. This operation, on the other hand, leaves the metadata fields from
     * 'this' which are not present in 'other' unmodified.
     */
    void copyFrom(const DocumentMetadataFields& other);

    /**
     * Returns an estimate in bytes of the size of the underlying metadata, which is held at a
     * distance by this object. The size of this object is not incorporated in the estimate.
     */
    size_t getApproximateSize() const;

    /**
     * Returns true if this object is in an initialized state and may hold metadata.
     */
    operator bool() const {
        return static_cast<bool>(_holder);
    }

    bool hasTextScore() const {
        return _holder && _holder->metaFields.test(MetaType::kTextScore);
    }

    double getTextScore() const {
        invariant(hasTextScore());
        return _holder->textScore;
    }

    void setTextScore(double score) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kTextScore);
        _holder->textScore = score;
    }

    bool hasRandVal() const {
        return _holder && _holder->metaFields.test(MetaType::kRandVal);
    }

    double getRandVal() const {
        invariant(hasRandVal());
        return _holder->randVal;
    }

    void setRandVal(double val) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kRandVal);
        _holder->randVal = val;
    }

    bool hasSortKey() const {
        return _holder && _holder->metaFields.test(MetaType::kSortKey);
    }

    Value getSortKey() const {
        invariant(hasSortKey());
        return _holder->sortKey;
    }

    void setSortKey(Value sortKey, bool isSingleElementKey) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kSortKey);
        _holder->isSingleElementKey = isSingleElementKey;
        _holder->sortKey = std::move(sortKey);
    }

    bool isSingleElementKey() const {
        return _holder && _holder->isSingleElementKey;
    }

    bool hasGeoNearDistance() const {
        return _holder && _holder->metaFields.test(MetaType::kGeoNearDist);
    }

    double getGeoNearDistance() const {
        invariant(hasGeoNearDistance());
        return _holder->geoNearDistance;
    }

    void setGeoNearDistance(double dist) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kGeoNearDist);
        _holder->geoNearDistance = dist;
    }

    bool hasGeoNearPoint() const {
        return _holder && _holder->metaFields.test(MetaType::kGeoNearPoint);
    }

    Value getGeoNearPoint() const {
        invariant(hasGeoNearPoint());
        return _holder->geoNearPoint;
    }

    void setGeoNearPoint(Value point) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kGeoNearPoint);
        _holder->geoNearPoint = std::move(point);
    }

    bool hasSearchScore() const {
        return _holder && _holder->metaFields.test(MetaType::kSearchScore);
    }

    double getSearchScore() const {
        invariant(hasSearchScore());
        return _holder->searchScore;
    }

    void setSearchScore(double score) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kSearchScore);
        _holder->searchScore = score;
    }

    bool hasSearchHighlights() const {
        return _holder && _holder->metaFields.test(MetaType::kSearchHighlights);
    }

    Value getSearchHighlights() const {
        invariant(hasSearchHighlights());
        return _holder->searchHighlights;
    }

    void setSearchHighlights(Value highlights) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kSearchHighlights);
        _holder->searchHighlights = highlights;
    }

    bool hasIndexKey() const {
        return _holder && _holder->metaFields.test(MetaType::kIndexKey);
    }

    BSONObj getIndexKey() const {
        invariant(hasIndexKey());
        return _holder->indexKey;
    }

    void setIndexKey(BSONObj indexKey) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kIndexKey);
        _holder->indexKey = indexKey.getOwned();
    }

    bool hasRecordId() const {
        return _holder && _holder->metaFields.test(MetaType::kRecordId);
    }

    RecordId getRecordId() const {
        invariant(hasRecordId());
        return _holder->recordId;
    }

    void setRecordId(RecordId rid) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::kRecordId);
        _holder->recordId = std::move(rid);
    }

    bool hasSearchScoreDetails() const {
        return _holder && _holder->metaFields.test(MetaType::kSearchScoreDetails);
    }

    BSONObj getSearchScoreDetails() const {
        invariant(hasSearchScoreDetails());
        return _holder->searchScoreDetails;
    }

    void setSearchScoreDetails(BSONObj details) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }
        _holder->metaFields.set(MetaType::kSearchScoreDetails);
        _holder->searchScoreDetails = details.getOwned();
    }

    bool hasTimeseriesBucketMinTime() const {
        return _holder && _holder->metaFields.test(MetaType::kTimeseriesBucketMinTime);
    }

    Date_t getTimeseriesBucketMinTime() const {
        invariant(hasTimeseriesBucketMinTime());
        return _holder->timeseriesBucketMinTime;
    }

    void setTimeseriesBucketMinTime(Date_t time) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }
        _holder->metaFields.set(MetaType::kTimeseriesBucketMinTime);
        _holder->timeseriesBucketMinTime = time;
    }

    bool hasTimeseriesBucketMaxTime() const {
        return _holder && _holder->metaFields.test(MetaType::kTimeseriesBucketMaxTime);
    }

    Date_t getTimeseriesBucketMaxTime() const {
        invariant(hasTimeseriesBucketMaxTime());
        return _holder->timeseriesBucketMaxTime;
    }

    void setTimeseriesBucketMaxTime(Date_t time) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }
        _holder->metaFields.set(MetaType::kTimeseriesBucketMaxTime);
        _holder->timeseriesBucketMaxTime = time;
    }
    void serializeForSorter(BufBuilder& buf) const;

private:
    // A simple data struct housing all possible metadata fields.
    struct MetadataHolder {
        std::bitset<MetaType::kNumFields> metaFields;

        // True when the sort key corresponds to a single-element sort pattern, meaning that
        // comparisons should treat the sort key value as a single element, even if it is an array.
        // Only relevant when 'kSortKey' is set.
        bool isSingleElementKey;

        double textScore{0.0};
        double randVal{0.0};
        Value sortKey;
        double geoNearDistance{0.0};
        Value geoNearPoint;
        double searchScore{0.0};
        Value searchHighlights;
        BSONObj indexKey;
        RecordId recordId;
        BSONObj searchScoreDetails;
        Date_t timeseriesBucketMinTime;
        Date_t timeseriesBucketMaxTime;
    };

    // Null until the first setter is called, at which point a MetadataHolder struct is allocated.
    std::unique_ptr<MetadataHolder> _holder;
};

using QueryMetadataBitSet = std::bitset<DocumentMetadataFields::MetaType::kNumFields>;

// Prints the metadata's name to the given stream.
std::ostream& operator<<(std::ostream& stream, DocumentMetadataFields::MetaType type);
StringBuilder& operator<<(StringBuilder& sb, DocumentMetadataFields::MetaType type);
}  // namespace mongo
