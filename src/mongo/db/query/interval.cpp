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
 */

#include "mongo/db/query/interval.h"

namespace mongo {

    namespace {

        /** Returns true if lhs and rhs intersection is not empty */
        bool intersects(const Interval& lhs, const Interval& rhs) {
            int res = lhs.start.woCompare(rhs.end, false);
            if (res > 0) {
                return false;
            }
            else if (res == 0 && (!lhs.startInclusive || !rhs.endInclusive)) {
                return false;
            }

            res = rhs.start.woCompare(lhs.end, false);
            if (res > 0) {
                return false;
            }
            else if (res == 0 && (!rhs.startInclusive || !lhs.endInclusive)) {
                return false;
            }

            return true;
        }

        /** Returns true if lhs and rhs represent the same interval */
        bool exact(const Interval& lhs, const Interval& rhs) {
            if (lhs.startInclusive != rhs.startInclusive) {
                return false;
            }

            if (lhs.endInclusive != rhs.endInclusive) {
                return false;
            }

            int res = lhs.start.woCompare(rhs.start, false);
            if (res != 0) {
                return false;
            }

            res = lhs.end.woCompare(rhs.end, false);
            if (res != 0) {
                return false;
            }

            return true;
        }

        /** Returns true if lhs is fully withing rhs */
        bool within(const Interval& lhs, const Interval& rhs) {
            int res = lhs.start.woCompare(rhs.start, false);
            if (res < 0) {
                return false;
            }
            else if (res == 0 && lhs.startInclusive && !rhs.startInclusive) {
                return false;
            }

            res = lhs.end.woCompare(rhs.end, false);
            if (res > 0) {
                return false;
            }
            else if (res == 0 && lhs.endInclusive && !rhs.endInclusive) {
                return false;
            }

            return true;
        }

        /** Returns true if the start of lhs comes before the start of rhs */
        bool precedes(const Interval& lhs, const Interval& rhs) {
            int res = lhs.start.woCompare(rhs.start, false);
            if (res < 0) {
                return true;
            }
            else if (res == 0 && lhs.startInclusive && !rhs.startInclusive) {
                return true;
            }
            return false;
        }

    } // unnamed namespace

    Interval:: Interval()
        : _intervalData(BSONObj())
        , start(BSONElement())
        , startInclusive(false)
        , end(BSONElement())
        , endInclusive(false) {
    }

    Interval::Interval(BSONObj base, bool si, bool ei) {
        init(base, si, ei);
    }

    void Interval::init(BSONObj base, bool si, bool ei) {
        dassert(base.nFields() >= 2);

        _intervalData = base.getOwned();
        BSONObjIterator it(_intervalData);
        start = it.next();
        end = it.next();
        startInclusive = si;
        endInclusive = ei;
    }

    bool Interval::isEmpty() const {
        return _intervalData.nFields() == 0;
    }

    // TODO: shortcut number of comparisons
    Interval::IntervalComparison Interval::compare(const Interval& other) const {

        //
        // Intersect cases
        //

        if (intersects(*this, other)) {
            if (exact(*this, other)) {
                return INTERVAL_EQUALS;
            }
            if (within(*this, other)) {
                return INTERVAL_WITHIN;
            }
            if (within(other, *this)) {
                return INTERVAL_CONTAINS;
            }
            if (precedes(*this, other)) {
                    return INTERVAL_OVERLAPS_BEFORE;
            }
            return INTERVAL_OVERLAPS_AFTER;
        }

        //
        // Non-intersect cases
        //

        if (precedes(*this, other)) {
            return INTERVAL_PRECEDES;
        }
        return INTERVAL_SUCCEDS;
    }

    void Interval::intersect(const Interval& other, IntervalComparison cmp) {
        if (cmp == INTERVAL_UNKNOWN) {
            cmp = this->compare(other);
        }

        BSONObjBuilder builder;
        switch (cmp) {

        case INTERVAL_EQUALS:
        case INTERVAL_WITHIN:
            break;

        case INTERVAL_CONTAINS:
            builder.append(other.start);
            builder.append(other.end);
            init(builder.obj(), other.startInclusive, other.endInclusive);
            break;

        case INTERVAL_OVERLAPS_AFTER:
            builder.append(start);
            builder.append(other.end);
            init(builder.obj(), startInclusive, other.endInclusive);
            break;

        case INTERVAL_OVERLAPS_BEFORE:
            builder.append(other.start);
            builder.append(end);
            init(builder.obj(), other.startInclusive, endInclusive);
            break;

        case INTERVAL_PRECEDES:
        case INTERVAL_SUCCEDS:
            *this = Interval();
            break;

        default:
            dassert(false);
        }
    }

    void Interval::combine(const Interval& other, IntervalComparison cmp) {
        if (cmp == INTERVAL_UNKNOWN) {
            cmp = this->compare(other);
        }

        BSONObjBuilder builder;
        switch (cmp) {

        case INTERVAL_EQUALS:
        case INTERVAL_CONTAINS:
            break;

        case INTERVAL_WITHIN:
            builder.append(other.start);
            builder.append(other.end);
            init(builder.obj(), other.startInclusive, other.endInclusive);
            break;

        case INTERVAL_OVERLAPS_AFTER:
        case INTERVAL_SUCCEDS:
            builder.append(other.start);
            builder.append(end);
            init(builder.obj(), other.startInclusive, endInclusive);
            break;

        case INTERVAL_OVERLAPS_BEFORE:
        case INTERVAL_PRECEDES:
            builder.append(start);
            builder.append(other.end);
            init(builder.obj(), startInclusive, other.endInclusive);
            break;

        default:
            dassert(false);
        }
    }

} // namespace mongo
