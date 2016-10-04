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

#include "mongo/db/query/interval.h"

namespace mongo {

using std::string;

Interval::Interval()
    : _intervalData(BSONObj()),
      start(BSONElement()),
      startInclusive(false),
      end(BSONElement()),
      endInclusive(false) {}

Interval::Interval(BSONObj base, bool si, bool ei) {
    init(base, si, ei);
}

void Interval::init(BSONObj base, bool si, bool ei) {
    verify(base.nFields() >= 2);

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

bool Interval::isPoint() const {
    return startInclusive && endInclusive && 0 == start.woCompare(end, false);
}

bool Interval::isNull() const {
    return (!startInclusive || !endInclusive) && 0 == start.woCompare(end, false);
}

//
// Comparison
//

bool Interval::equals(const Interval& other) const {
    if (this->startInclusive != other.startInclusive) {
        return false;
    }

    if (this->endInclusive != other.endInclusive) {
        return false;
    }

    int res = this->start.woCompare(other.start, false);
    if (res != 0) {
        return false;
    }

    res = this->end.woCompare(other.end, false);
    if (res != 0) {
        return false;
    }

    return true;
}

bool Interval::intersects(const Interval& other) const {
    int res = this->start.woCompare(other.end, false);
    if (res > 0) {
        return false;
    } else if (res == 0 && (!this->startInclusive || !other.endInclusive)) {
        return false;
    }

    res = other.start.woCompare(this->end, false);
    if (res > 0) {
        return false;
    } else if (res == 0 && (!other.startInclusive || !this->endInclusive)) {
        return false;
    }

    return true;
}

bool Interval::within(const Interval& other) const {
    int res = this->start.woCompare(other.start, false);
    if (res < 0) {
        return false;
    } else if (res == 0 && this->startInclusive && !other.startInclusive) {
        return false;
    }

    res = this->end.woCompare(other.end, false);
    if (res > 0) {
        return false;
    } else if (res == 0 && this->endInclusive && !other.endInclusive) {
        return false;
    }

    return true;
}

/** Returns true if the start of comes before the start of other */
bool Interval::precedes(const Interval& other) const {
    int res = this->start.woCompare(other.start, false);
    if (res < 0) {
        return true;
    } else if (res == 0 && this->startInclusive && !other.startInclusive) {
        return true;
    }
    return false;
}


Interval::IntervalComparison Interval::compare(const Interval& other) const {
    //
    // Intersect cases
    //

    if (this->intersects(other)) {
        if (this->equals(other)) {
            return INTERVAL_EQUALS;
        }
        if (this->within(other)) {
            return INTERVAL_WITHIN;
        }
        if (other.within(*this)) {
            return INTERVAL_CONTAINS;
        }
        if (this->precedes(other)) {
            return INTERVAL_OVERLAPS_BEFORE;
        }
        return INTERVAL_OVERLAPS_AFTER;
    }

    //
    // Non-intersect cases
    //

    if (this->precedes(other)) {
        // It's not possible for both endInclusive and other.startInclusive to be true because
        // the bounds would intersect. Refer to section on "Intersect cases" above.
        if ((endInclusive || other.startInclusive) && 0 == end.woCompare(other.start, false)) {
            return INTERVAL_PRECEDES_COULD_UNION;
        }
        return INTERVAL_PRECEDES;
    }

    return INTERVAL_SUCCEEDS;
}

//
// Mutation: Union and Intersection
//

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
        case INTERVAL_SUCCEEDS:
            *this = Interval();
            break;

        default:
            verify(false);
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
        case INTERVAL_SUCCEEDS:
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
            verify(false);
    }
}

void Interval::reverse() {
    std::swap(start, end);
    std::swap(startInclusive, endInclusive);
}

//
// Debug info
//

// static
string Interval::cmpstr(IntervalComparison c) {
    if (c == INTERVAL_EQUALS) {
        return "INTERVAL_EQUALS";
    }

    // 'this' contains the other interval.
    if (c == INTERVAL_CONTAINS) {
        return "INTERVAL_CONTAINS";
    }

    // 'this' is contained by the other interval.
    if (c == INTERVAL_WITHIN) {
        return "INTERVAL_WITHIN";
    }

    // The two intervals intersect and 'this' is before the other interval.
    if (c == INTERVAL_OVERLAPS_BEFORE) {
        return "INTERVAL_OVERLAPS_BEFORE";
    }

    // The two intervals intersect and 'this is after the other interval.
    if (c == INTERVAL_OVERLAPS_AFTER) {
        return "INTERVAL_OVERLAPS_AFTER";
    }

    // There is no intersection.
    if (c == INTERVAL_PRECEDES) {
        return "INTERVAL_PRECEDES";
    }

    if (c == INTERVAL_PRECEDES_COULD_UNION) {
        return "INTERVAL_PRECEDES_COULD_UNION";
    }

    if (c == INTERVAL_SUCCEEDS) {
        return "INTERVAL_SUCCEEDS";
    }

    if (c == INTERVAL_UNKNOWN) {
        return "INTERVAL_UNKNOWN";
    }

    return "NO IDEA DUDE";
}

}  // namespace mongo
