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

#include "mongo/db/query/compiler/physical_model/interval/interval.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"

#include <utility>

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
    MONGO_verify(base.nFields() >= 2);

    _intervalData = base.getOwned();
    BSONObjIterator it(_intervalData);
    start = it.next();
    end = it.next();
    startInclusive = si;
    endInclusive = ei;
}

Interval::Interval(
    BSONObj base, BSONElement start, bool startInclusive, BSONElement end, bool endInclusive)
    : _intervalData(base),
      start(start),
      startInclusive(startInclusive),
      end(end),
      endInclusive(endInclusive) {}

bool Interval::isEmpty() const {
    return start.eoo() && end.eoo();
}

bool Interval::isPoint() const {
    return startInclusive && endInclusive && 0 == start.woCompare(end, false);
}

bool Interval::isNull() const {
    return (!startInclusive || !endInclusive) && 0 == start.woCompare(end, false);
}

Interval::Direction Interval::getDirection() const {
    if (isEmpty() || isPoint() || isNull()) {
        return Direction::kDirectionNone;
    }

    // 'false' to not consider the field name.
    const int res = start.woCompare(end, false);

    invariant(res != 0);
    return res < 0 ? Direction::kDirectionAscending : Direction::kDirectionDescending;
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
    if (kDebugBuild) {
        // This function assumes that both intervals are ascending (or are empty/point intervals).
        // Determining this may be expensive, so we only do these checks when in a debug build.
        const auto thisDir = getDirection();
        invariant(thisDir == Direction::kDirectionAscending ||
                  thisDir == Direction::kDirectionNone);
        const auto otherDir = other.getDirection();
        invariant(otherDir == Direction::kDirectionAscending ||
                  otherDir == Direction::kDirectionNone);
    }

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

bool Interval::isMinToMax() const {
    return (start.type() == BSONType::minKey && end.type() == BSONType::maxKey);
}

bool Interval::isMaxToMin() const {
    return (start.type() == BSONType::maxKey && end.type() == BSONType::minKey);
}

bool Interval::isFullyOpen() const {
    return isMinToMax() || isMaxToMin();
}

bool Interval::isUndefined() const {
    return (start.type() == BSONType::undefined || end.type() == BSONType::undefined);
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
            MONGO_verify(false);
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
            MONGO_verify(false);
    }
}

void Interval::reverse() {
    std::swap(start, end);
    std::swap(startInclusive, endInclusive);
}

Interval Interval::reverseClone() const {
    Interval reversed;
    reversed.start = end;
    reversed.end = start;
    reversed.startInclusive = endInclusive;
    reversed.endInclusive = startInclusive;
    reversed._intervalData = _intervalData;

    return reversed;
}

}  // namespace mongo
