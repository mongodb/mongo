/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo {

/** A range of values for one field. */
struct Interval {
    // No BSONValue means we have to keep a BSONObj and pointers (BSONElement) into it.
    // 'start' may not point at the first field in _intervalData.
    // 'end' may not point at the last field in _intervalData.
    // 'start' and 'end' may point at the same field.
    // This BSON may contain elements other than the start and end elements. We cannot make any
    // assumptions about the order of elements in this object; we should also not make any
    // assumptions about the field names of the elements, which are not guaranteed to be empty.
    BSONObj _intervalData;

    // Start and End must be ordered according to the index order.
    // For the reasons mentioned above, comparisons to 'start' and 'end' should ignore the field
    // names of the BSONElements.
    BSONElement start;
    bool startInclusive;

    // For the reasons mentioned above, comparisons to 'start' and 'end' should ignore the field
    // names of the BSONElements.
    BSONElement end;
    bool endInclusive;

    /** Creates an empty interval */
    Interval();

    /**
     * Generates a debug string for an interval. If interval 'hasNonSimpleCollation', then string
     * bounds are hex-encoded.
     */
    std::string toString(bool hasNonSimpleCollation) const {
        str::stream ss;
        if (startInclusive) {
            ss << "[";
        } else {
            ss << "(";
        }
        auto boundToString = [&](BSONElement bound) {
            if (bound.type() == BSONType::string && hasNonSimpleCollation) {
                ss << "CollationKey(";
                // False means omit the field name.
                ss << "0x" << hexblob::encodeLower(bound.valueStringData());
                ss << ")";
            } else {
                ss << bound.toString(false);
            }
        };
        boundToString(start);
        ss << ", ";
        boundToString(end);
        if (endInclusive) {
            ss << "]";
        } else {
            ss << ")";
        }
        return ss;
    }

    /**
     * Creates an interval that starts at the first field of 'base' and ends at the second
     * field of 'base'. (In other words, 'base' is a bsonobj with at least two elements, of
     * which we don't care about field names.)
     *
     * The interval's extremities are closed or not depending on whether
     * 'start'/'endIncluded' are true or not.
     */
    Interval(BSONObj base, bool startIncluded, bool endIncluded);

    /** Sets the current interval to the given values (see constructor) */
    void init(BSONObj base, bool startIncluded, bool endIncluded);

    Interval(
        BSONObj base, BSONElement start, bool startInclusive, BSONElement end, bool endInclusive);

    /**
     * Returns true if an empty-constructed interval hasn't been init()-ialized yet
     */
    bool isEmpty() const;

    /**
     * Does this interval represent exactly one point?
     */
    bool isPoint() const;

    /**
     * Returns true if start is same as end and interval is open at either end
     */
    bool isNull() const;

    enum class Direction {
        // Point intervals, empty intervals, and null intervals have no direction.
        kDirectionNone,
        kDirectionAscending,
        kDirectionDescending
    };

    /**
     * Compute the direction.
     */
    Direction getDirection() const;

    //
    // Comparison with other intervals
    //

    /**
     * Returns true if 'this' is the same interval as 'other'
     */
    bool equals(const Interval& other) const;

    /**
     * Returns true if 'this' overlaps with 'other', false otherwise.
     */
    bool intersects(const Interval& rhs) const;

    /**
     * Returns true if 'this' is within 'other', false otherwise.
     */
    bool within(const Interval& other) const;

    /**
     * Returns true if 'this' is located before 'other', false otherwise.
     */
    bool precedes(const Interval& other) const;

    /**
     * Returns true if the interval is from MinKey to MaxKey.
     */
    bool isMinToMax() const;

    /**
     * Returns true if the interval is from MaxKey to MinKey.
     */
    bool isMaxToMin() const;

    /**
     * Returns true if the interval has negative and positive infinities as bounds.
     */
    bool isFullyOpen() const;

    /**
     * Returns true if the interval has undefined start and/or end bounds.
     */
    bool isUndefined() const;

    /** Returns how 'this' compares to 'other' */
    enum IntervalComparison {
        //
        // There is some intersection.
        //

        // The two intervals are *exactly* equal.
        INTERVAL_EQUALS,

        // 'this' contains the other interval.
        INTERVAL_CONTAINS,

        // 'this' is contained by the other interval.
        INTERVAL_WITHIN,

        // The two intervals intersect and 'this' is before the other interval.
        INTERVAL_OVERLAPS_BEFORE,

        // The two intervals intersect and 'this is after the other interval.
        INTERVAL_OVERLAPS_AFTER,

        //
        // There is no intersection.
        //

        INTERVAL_PRECEDES,

        // This happens if we have [a,b) [b,c]
        INTERVAL_PRECEDES_COULD_UNION,

        INTERVAL_SUCCEEDS,

        INTERVAL_UNKNOWN
    };

    IntervalComparison compare(const Interval& other) const;

    //
    // Mutation of intervals
    //

    /**
     * Swap start and end points of interval.
     */
    void reverse();

    /**
     * Return a new Interval that's a reverse of this one.
     */
    Interval reverseClone() const;

    /**
     * Updates 'this' with the intersection of 'this' and 'other'. If 'this' and 'other'
     * have been compare()d before, that result can be optionally passed in 'cmp'
     */
    void intersect(const Interval& other, IntervalComparison cmp = INTERVAL_UNKNOWN);

    /**
     * Updates 'this" with the union of 'this' and 'other'. If 'this' and 'other' have
     * been compare()d before, that result can be optionaly passed in 'cmp'.
     */
    void combine(const Interval& other, IntervalComparison cmp = INTERVAL_UNKNOWN);
};

inline bool operator==(const Interval& lhs, const Interval& rhs) {
    return lhs.equals(rhs);
}

inline bool operator!=(const Interval& lhs, const Interval& rhs) {
    return !(lhs == rhs);
}

}  // namespace mongo
