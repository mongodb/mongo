// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/hex.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo {

/** A range of values for one field. */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] Interval {
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
