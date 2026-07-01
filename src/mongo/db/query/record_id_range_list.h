/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/record_id_range.h"

#include <vector>

namespace mongo {

/**
 * An ordered, non-overlapping list of RecordId ranges representing the union of those ranges.
 *
 * Invariants maintained by all constructors and free functions:
 *  - Ranges are sorted by lower bound (ascending).
 *  - No two ranges overlap or are adjacent (they have been merged).
 *  - Only the first range may have an unbounded start (min = boost::none, treated as -∞).
 *  - Only the last range may have an unbounded end (max = boost::none, treated as +∞).
 *
 * Special cases:
 *  - A default-constructed RecordIdRangeList is "unbounded" (∀): a single fully-open range
 *    matching all RecordIds.
 *  - An empty _ranges vector is the "empty" list (∅): no ranges, no records match.
 */
class MONGO_MOD_PUBLIC RecordIdRangeList {
public:
    /**
     * Constructs an unbounded list (∀) — a single fully-open range with no min and no max.
     */
    RecordIdRangeList() {
        _ranges.push_back(RecordIdRange{});
    }

    explicit RecordIdRangeList(RecordIdRange range) {
        _ranges.push_back(std::move(range));
    }

    /*
     * Factory functions to construct as the union of the given ranges/range lists.
     *
     * The ranges are sorted, overlapping/adjacent entries are merged, and the semi-unbounded
     * invariant is enforced.
     *
     * Identity: makeUnion/unite({}) returns an empty list (∅) — "no records
     * match". This is the correct result for, e.g., an empty $in or $or: the union of no ranges is
     * the empty set.
     */
    static RecordIdRangeList makeUnion(std::vector<RecordIdRange> ranges);
    static RecordIdRangeList unite(std::vector<RecordIdRangeList> lists);

    /**
     * Factory function to construct as the intersection of a list of other `RecordIdRangeList`s.
     *
     * Identity: intersect({}) returns an unbounded list (∀) — "all records match". This
     * is the correct identity for intersection: the intersection of no constraints is the universal
     * set.
     */
    static RecordIdRangeList intersect(std::vector<RecordIdRangeList> lists);

    /**
     * Returns true iff this list represents "all RecordIds" (∀) — i.e. a single fully-open range
     * with no min and no max.
     *
     * An empty list (∅, _ranges is empty) returns false: it is a real constraint meaning no records
     * match, and callers must act on it.
     */
    bool isUnbounded() const {
        if (_ranges.size() != 1) {
            return false;
        }
        const auto& r = _ranges.front();
        return !r.getMin() && !r.getMax();
    }

    /**
     * Returns if this list is empty. An empty RecordIdRangeList contains no RecordId ranges and
     * hence matches no RecordId.
     */
    bool isEmpty() const {
        return _ranges.empty();
    }

    /**
     * Returns the outer bounding range of this list:
     *   ie. {min: front().getMin(), max: back().getMax()} with the correct bound inclusivities.
     *
     * If _ranges is empty (empty list, ∅), returns a range with both min and max set to a
     * default-constructed RecordIdBound with exclusive inclusivity — an empty range.
     */
    RecordIdRange outerBounds() const;

    /**
     * Serializes this list to a BSON array for explain output.  Each element has the form:
     *   { "min": <value>,        // omitted if the range has no lower bound
     *     "minInclusive": <bool>,
     *     "max": <value>,        // omitted if the range has no upper bound
     *     "maxInclusive": <bool> }
     */
    BSONArray toBSONArray() const;

    const std::vector<RecordIdRange>& getRanges() const {
        return _ranges;
    }

    // Tag struct returned if the rid given to seek is beyond all the ranges in the list.
    struct SeekBeyondAllRanges {};
    // Tag struct returned if the rid given to seek is in a specific range.
    struct SeekInRange {
        size_t idx;
    };
    // Tag struct returned if the rid given to seek falls between two ranges in the list.
    struct SeekBeforeRange {
        size_t idx;
    };
    using SeekResult = std::variant<SeekBeyondAllRanges, SeekInRange, SeekBeforeRange>;

    // Uses std::lower_bound to seek for rid between the ranges:
    //   if  forward: (startIdx, getRanges().size())
    //   if !forward: [0, startIdx)
    SeekResult seek(const RecordId& rid, size_t startIdx, bool forward) const;


private:
    struct EMPTY_TAG {};

    // Special constructor that constructs an empty RecordIdRangeList.
    RecordIdRangeList(EMPTY_TAG) {}

    std::vector<RecordIdRange> _ranges;
};

}  // namespace mongo
