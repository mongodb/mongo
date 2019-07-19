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
#include "mongo/db/pipeline/value.h"

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
        TEXT_SCORE,
        RAND_VAL,
        SORT_KEY,
        GEONEAR_DIST,
        GEONEAR_POINT,
        SEARCH_SCORE,
        SEARCH_HIGHLIGHTS,
        INDEX_KEY,

        // New fields must be added before the NUM_FIELDS sentinel.
        NUM_FIELDS
    };

    /**
     * Reads serialized metadata out of 'buf', and uses it to populate 'out'. Expects 'buf' to have
     * been written to by a previous call to serializeForSorter(). It is illegal to pass a null
     * pointer for 'out'.
     */
    static void deserializeForSorter(BufReader& buf, DocumentMetadataFields* out);

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
        return _holder && _holder->metaFields.test(MetaType::TEXT_SCORE);
    }

    double getTextScore() const {
        invariant(hasTextScore());
        return _holder->textScore;
    }

    void setTextScore(double score) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::TEXT_SCORE);
        _holder->textScore = score;
    }

    bool hasRandVal() const {
        return _holder && _holder->metaFields.test(MetaType::RAND_VAL);
    }

    double getRandVal() const {
        invariant(hasRandVal());
        return _holder->randVal;
    }

    void setRandVal(double val) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::RAND_VAL);
        _holder->randVal = val;
    }

    bool hasSortKey() const {
        return _holder && _holder->metaFields.test(MetaType::SORT_KEY);
    }

    BSONObj getSortKey() const {
        invariant(hasSortKey());
        return _holder->sortKey;
    }

    void setSortKey(BSONObj sortKey) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::SORT_KEY);
        _holder->sortKey = sortKey.getOwned();
    }

    bool hasGeoNearDistance() const {
        return _holder && _holder->metaFields.test(MetaType::GEONEAR_DIST);
    }

    double getGeoNearDistance() const {
        invariant(hasGeoNearDistance());
        return _holder->geoNearDistance;
    }

    void setGeoNearDistance(double dist) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::GEONEAR_DIST);
        _holder->geoNearDistance = dist;
    }

    bool hasGeoNearPoint() const {
        return _holder && _holder->metaFields.test(MetaType::GEONEAR_POINT);
    }

    Value getGeoNearPoint() const {
        invariant(hasGeoNearPoint());
        return _holder->geoNearPoint;
    }

    void setGeoNearPoint(Value point) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::GEONEAR_POINT);
        _holder->geoNearPoint = std::move(point);
    }

    bool hasSearchScore() const {
        return _holder && _holder->metaFields.test(MetaType::SEARCH_SCORE);
    }

    double getSearchScore() const {
        invariant(hasSearchScore());
        return _holder->searchScore;
    }

    void setSearchScore(double score) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::SEARCH_SCORE);
        _holder->searchScore = score;
    }

    bool hasSearchHighlights() const {
        return _holder && _holder->metaFields.test(MetaType::SEARCH_HIGHLIGHTS);
    }

    Value getSearchHighlights() const {
        invariant(hasSearchHighlights());
        return _holder->searchHighlights;
    }

    void setSearchHighlights(Value highlights) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::SEARCH_HIGHLIGHTS);
        _holder->searchHighlights = highlights;
    }

    bool hasIndexKey() const {
        return _holder && _holder->metaFields.test(MetaType::INDEX_KEY);
    }

    BSONObj getIndexKey() const {
        invariant(hasIndexKey());
        return _holder->indexKey;
    }

    void setIndexKey(BSONObj indexKey) {
        if (!_holder) {
            _holder = std::make_unique<MetadataHolder>();
        }

        _holder->metaFields.set(MetaType::INDEX_KEY);
        _holder->indexKey = indexKey.getOwned();
    }

    void serializeForSorter(BufBuilder& buf) const;

private:
    // A simple data struct housing all possible metadata fields.
    struct MetadataHolder {
        std::bitset<MetaType::NUM_FIELDS> metaFields;
        double textScore{0.0};
        double randVal{0.0};
        BSONObj sortKey;
        double geoNearDistance{0.0};
        Value geoNearPoint;
        double searchScore{0.0};
        Value searchHighlights;
        BSONObj indexKey;
    };

    // Null until the first setter is called, at which point a MetadataHolder struct is allocated.
    std::unique_ptr<MetadataHolder> _holder;
};

}  // namespace mongo
