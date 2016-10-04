/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

/** A range of values for one field. */
struct Interval {
    // No BSONValue means we have to keep a BSONObj and pointers (BSONElement) into it.
    // 'start' may not point at the first field in _intervalData.
    // 'end' may not point at the last field in _intervalData.
    // 'start' and 'end' may point at the same field.
    BSONObj _intervalData;

    // Start and End must be ordered according to the index order.
    BSONElement start;
    bool startInclusive;

    BSONElement end;
    bool endInclusive;

    /** Creates an empty interval */
    Interval();

    std::string toString() const {
        mongoutils::str::stream ss;
        if (startInclusive) {
            ss << "[";
        } else {
            ss << "(";
        }
        // false means omit the field name
        ss << start.toString(false);
        ss << ", ";
        ss << end.toString(false);
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

    /**
     * toString for IntervalComparison
     */
    static std::string cmpstr(IntervalComparison c);

    //
    // Mutation of intervals
    //

    /**
     * Swap start and end points of interval.
     */
    void reverse();

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
    return lhs.compare(rhs) == Interval::INTERVAL_EQUALS;
}

inline bool operator!=(const Interval& lhs, const Interval& rhs) {
    return !(lhs == rhs);
}

}  // namespace mongo
