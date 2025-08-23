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

#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace mongo {

using std::string;
using std::vector;

namespace {

// Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
int sgn(int i) {
    if (i == 0)
        return 0;
    return i > 0 ? 1 : -1;
}

/**
 * Returns BEHIND if the key is behind the interval.
 * Returns WITHIN if the key is within the interval.
 * Returns AHEAD if the key is ahead the interval.
 *
 * All directions are oriented along 'direction'.
 */
IndexBoundsChecker::Location intervalCmp(const Interval& interval,
                                         const BSONElement& key,
                                         const int expectedDirection) {
    int cmp = sgn(key.woCompare(interval.start, false));
    bool startOK = (cmp == expectedDirection) || (cmp == 0 && interval.startInclusive);
    if (!startOK) {
        return IndexBoundsChecker::BEHIND;
    }

    cmp = sgn(key.woCompare(interval.end, false));
    bool endOK = (cmp == -expectedDirection) || (cmp == 0 && interval.endInclusive);
    if (!endOK) {
        return IndexBoundsChecker::AHEAD;
    }

    return IndexBoundsChecker::WITHIN;
}

}  // namespace

// For debugging.
size_t IndexBounds::size() const {
    return fields.size();
}

string IndexBounds::getFieldName(size_t i) const {
    return i < size() ? fields[i].name : "";
}

size_t IndexBounds::getNumIntervals(size_t i) const {
    return i < size() ? fields[i].intervals.size() : 0;
}

Interval IndexBounds::getInterval(size_t i, size_t j) const {
    if (i < size() && j < fields[i].intervals.size()) {
        return fields[i].intervals[j];
    } else {
        return Interval();
    }
}

bool IndexBounds::operator==(const IndexBounds& other) const {
    if (this->isSimpleRange != other.isSimpleRange) {
        return false;
    }

    if (this->isSimpleRange) {
        return SimpleBSONObjComparator::kInstance.evaluate(this->startKey == other.startKey) &&
            SimpleBSONObjComparator::kInstance.evaluate(this->endKey == other.endKey) &&
            (this->boundInclusion == other.boundInclusion);
    }

    if (this->fields.size() != other.fields.size()) {
        return false;
    }

    for (size_t i = 0; i < this->fields.size(); ++i) {
        if (this->fields[i] != other.fields[i]) {
            return false;
        }
    }

    return true;
}

bool IndexBounds::operator!=(const IndexBounds& other) const {
    return !(*this == other);
}

string OrderedIntervalList::toString(bool hasNonSimpleCollation) const {
    str::stream ss;
    ss << "['" << name << "']: ";
    for (size_t j = 0; j < intervals.size(); ++j) {
        ss << intervals[j].toString(hasNonSimpleCollation);
        if (j < intervals.size() - 1) {
            ss << ", ";
        }
    }
    return ss;
}

bool IndexBounds::isStartIncludedInBound(BoundInclusion boundInclusion) {
    return boundInclusion == BoundInclusion::kIncludeBothStartAndEndKeys ||
        boundInclusion == BoundInclusion::kIncludeStartKeyOnly;
}

bool IndexBounds::isEndIncludedInBound(BoundInclusion boundInclusion) {
    return boundInclusion == BoundInclusion::kIncludeBothStartAndEndKeys ||
        boundInclusion == BoundInclusion::kIncludeEndKeyOnly;
}

BoundInclusion IndexBounds::makeBoundInclusionFromBoundBools(bool startKeyInclusive,
                                                             bool endKeyInclusive) {
    if (startKeyInclusive) {
        if (endKeyInclusive) {
            return BoundInclusion::kIncludeBothStartAndEndKeys;
        } else {
            return BoundInclusion::kIncludeStartKeyOnly;
        }
    } else {
        if (endKeyInclusive) {
            return BoundInclusion::kIncludeEndKeyOnly;
        } else {
            return BoundInclusion::kExcludeBothStartAndEndKeys;
        }
    }
}

BoundInclusion IndexBounds::reverseBoundInclusion(BoundInclusion b) {
    switch (b) {
        case BoundInclusion::kIncludeStartKeyOnly:
            return BoundInclusion::kIncludeEndKeyOnly;
        case BoundInclusion::kIncludeEndKeyOnly:
            return BoundInclusion::kIncludeStartKeyOnly;
        case BoundInclusion::kIncludeBothStartAndEndKeys:
        case BoundInclusion::kExcludeBothStartAndEndKeys:
            // These are both symmetric.
            return b;
        default:
            MONGO_UNREACHABLE;
    }
}


bool OrderedIntervalList::operator==(const OrderedIntervalList& other) const {
    if (this->name != other.name) {
        return false;
    }

    if (this->intervals.size() != other.intervals.size()) {
        return false;
    }

    for (size_t i = 0; i < this->intervals.size(); ++i) {
        if (this->intervals[i] != other.intervals[i]) {
            return false;
        }
    }

    return true;
}

bool OrderedIntervalList::operator!=(const OrderedIntervalList& other) const {
    return !(*this == other);
}

void OrderedIntervalList::reverse() {
    for (size_t i = 0; i < (intervals.size() + 1) / 2; i++) {
        const size_t otherIdx = intervals.size() - i - 1;
        intervals[i].reverse();
        if (i != otherIdx) {
            intervals[otherIdx].reverse();
            std::swap(intervals[i], intervals[otherIdx]);
        }
    }
}

OrderedIntervalList OrderedIntervalList::reverseClone() const {
    OrderedIntervalList clone(name);

    for (auto it = intervals.rbegin(); it != intervals.rend(); ++it) {
        clone.intervals.push_back(it->reverseClone());
    }

    return clone;
}

Interval::Direction OrderedIntervalList::computeDirection() const {
    if (intervals.empty())
        return Interval::Direction::kDirectionNone;

    // Because the interval list is ordered, we only need to compare the two endpoints of the
    // overall list. If the endpoints are ascending or descending, then each interval already
    // respects that order. And if the endpoints are equal, then all the intervals must be squeezed
    // into a single point.
    bool compareFieldNames = false;
    int res = intervals.front().start.woCompare(intervals.back().end, compareFieldNames);
    if (res == 0)
        return Interval::Direction::kDirectionNone;
    return res < 0 ? Interval::Direction::kDirectionAscending
                   : Interval::Direction::kDirectionDescending;
}

bool OrderedIntervalList::isMinToMax() const {
    return intervals.size() == 1 && intervals[0].isMinToMax();
}

bool OrderedIntervalList::isMaxToMin() const {
    return intervals.size() == 1 && intervals[0].isMaxToMin();
}

bool OrderedIntervalList::isFullyOpen() const {
    return intervals.size() == 1 && intervals[0].isFullyOpen();
}

bool OrderedIntervalList::isPoint() const {
    return intervals.size() == 1 && intervals[0].isPoint();
}

bool OrderedIntervalList::containsOnlyPointIntervals() const {
    for (const auto& interval : intervals) {
        if (!interval.isPoint()) {
            return false;
        }
    }

    return true;
}

/**
 * Queries whose bounds overlap the Object type bracket may require special handling, since the $**
 * index does not index complete objects but instead only contains the leaves along each of its
 * subpaths. Since we ban all object-value queries except those on the empty object {}, this will
 * typically only be relevant for bounds involving MinKey and MaxKey, such as {$exists: true}.
 */
bool OrderedIntervalList::boundsOverlapObjectTypeBracket() const {
    // Create an Interval representing the subrange ({}, []) of the object type bracket. We exclude
    // both ends of the bracket because $** indexes support queries on empty objects and arrays.
    static const Interval objectTypeBracketBounds = []() {
        BSONObjBuilder objBracketBounds;
        objBracketBounds.appendMinForType("", stdx::to_underlying(BSONType::object));
        objBracketBounds.appendMaxForType("", stdx::to_underlying(BSONType::object));
        return Interval(objBracketBounds.obj(), false /*startIncluded*/, false /*endIncluded*/);
    }();

    // Determine whether any of the ordered intervals overlap with the object type bracket. Because
    // Interval's various bounds-comparison methods all depend upon the bounds being in ascending
    // order, we reverse the direction of the input OIL if necessary here.
    const bool isDescending = (computeDirection() == Interval::Direction::kDirectionDescending);
    const auto& oilAscending = (isDescending ? reverseClone() : *this);
    // Iterate through each of the OIL's intervals. If the current interval precedes the bracket, we
    // must check the next interval in sequence. If the interval succeeds the bracket then we can
    // stop checking. If we neither precede nor succeed the object type bracket, then they overlap.
    for (const auto& interval : oilAscending.intervals) {
        switch (interval.compare(objectTypeBracketBounds)) {
            case Interval::IntervalComparison::INTERVAL_PRECEDES_COULD_UNION:
            case Interval::IntervalComparison::INTERVAL_PRECEDES:
                // Break out of the switch and proceed to check the next interval.
                break;

            case Interval::IntervalComparison::INTERVAL_SUCCEEDS:
                return false;

            default:
                return true;
        }
    }
    // If we're here, then all the OIL's bounds precede the object type bracket.
    return false;
}

// static
void OrderedIntervalList::complement() {
    BSONObjBuilder minBob;
    minBob.appendMinKey("");
    BSONObj minObj = minBob.obj();

    // We complement by scanning the entire range of BSON values from MinKey to MaxKey. The value
    // from which we must begin the next complemented interval is kept in 'curBoundary'.
    BSONElement curBoundary = minObj.firstElement();

    // If 'curInclusive' is true, then 'curBoundary' is included in one of the original intervals,
    // and hence should not be included in the complement (and vice-versa if 'curInclusive' is
    // false).
    bool curInclusive = false;

    // We will build up a list of intervals that represents the inversion of those in the OIL.
    vector<Interval> newIntervals;
    for (const auto& curInt : intervals) {
        if ((0 != curInt.start.woCompare(curBoundary, /*compareFieldNames*/ false) ||
             (!curInclusive && !curInt.startInclusive))) {
            // Make a new interval from 'curBoundary' to the start of 'curInterval'.
            BSONObjBuilder intBob;
            intBob.append(curBoundary);
            intBob.append(curInt.start);
            Interval newInt(intBob.obj(), !curInclusive, !curInt.startInclusive);
            newIntervals.push_back(newInt);
        }

        // Reset the boundary for the next iteration.
        curBoundary = curInt.end;
        curInclusive = curInt.endInclusive;
    }

    // We may have to add a final interval which ends in MaxKey.
    BSONObjBuilder maxBob;
    maxBob.appendMaxKey("");
    BSONObj maxObj = maxBob.obj();
    BSONElement maxKey = maxObj.firstElement();
    if (0 != maxKey.woCompare(curBoundary, /*compareFieldNames*/ false) || !curInclusive) {
        BSONObjBuilder intBob;
        intBob.append(curBoundary);
        intBob.append(maxKey);
        Interval newInt(intBob.obj(), !curInclusive, true);
        newIntervals.push_back(newInt);
    }

    // Replace the old list of intervals with the new one.
    intervals.clear();
    intervals.insert(intervals.end(), newIntervals.begin(), newIntervals.end());
}

string IndexBounds::toString(bool hasNonSimpleCollation) const {
    str::stream ss;
    if (isSimpleRange) {
        if (IndexBounds::isStartIncludedInBound(boundInclusion)) {
            ss << "[";
        } else {
            ss << "(";
        }
        ss << startKey.toString() << ", ";
        if (endKey.isEmpty()) {
            ss << "]";
        } else {
            ss << endKey.toString();
            if (IndexBounds::isEndIncludedInBound(boundInclusion)) {
                ss << "]";
            } else {
                ss << ")";
            }
        }
        return ss;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) {
            ss << ", ";
        }
        ss << "field #" << i << fields[i].toString(hasNonSimpleCollation);
    }

    return ss;
}

BSONObj IndexBounds::toBSON(bool hasNonSimpleCollation) const {
    BSONObjBuilder bob;
    vector<OrderedIntervalList>::const_iterator itField;
    for (itField = fields.begin(); itField != fields.end(); ++itField) {
        BSONArrayBuilder fieldBuilder(bob.subarrayStart(itField->name));

        vector<Interval>::const_iterator itInterval;
        for (itInterval = itField->intervals.begin(); itInterval != itField->intervals.end();
             ++itInterval) {
            std::string intervalStr = itInterval->toString(hasNonSimpleCollation);
            // Insulate against hitting BSON size limit.
            if ((bob.len() + (int)intervalStr.size()) > BSONObjMaxUserSize) {
                fieldBuilder.append("warning: bounds truncated due to BSON size limit");
                fieldBuilder.doneFast();
                return bob.obj();
            }

            fieldBuilder.append(intervalStr);
        }

        fieldBuilder.doneFast();
    }

    return bob.obj();
}

IndexBounds IndexBounds::forwardize() const {
    IndexBounds newBounds;
    newBounds.isSimpleRange = isSimpleRange;

    if (isSimpleRange) {
        const int cmpRes = startKey.woCompare(endKey);
        if (cmpRes <= 0) {
            newBounds.startKey = startKey;
            newBounds.endKey = endKey;
            newBounds.boundInclusion = boundInclusion;
        } else {
            // Swap start and end key.
            newBounds.endKey = startKey;
            newBounds.startKey = endKey;
            newBounds.boundInclusion = IndexBounds::reverseBoundInclusion(boundInclusion);
        }

        return newBounds;
    }

    newBounds.fields.reserve(fields.size());
    std::transform(fields.begin(),
                   fields.end(),
                   std::back_inserter(newBounds.fields),
                   [](const OrderedIntervalList& oil) {
                       if (oil.computeDirection() == Interval::Direction::kDirectionDescending) {
                           return oil.reverseClone();
                       }
                       return oil;
                   });

    return newBounds;
}

IndexBounds IndexBounds::reverse() const {
    IndexBounds reversed(*this);

    if (reversed.isSimpleRange) {
        std::swap(reversed.startKey, reversed.endKey);
        // If only one bound is included, swap which one is included.
        reversed.boundInclusion = reverseBoundInclusion(reversed.boundInclusion);
    } else {
        for (auto& orderedIntervalList : reversed.fields) {
            orderedIntervalList.reverse();
        }
    }

    return reversed;
}

bool IndexBounds::isUnbounded() const {
    return std::all_of(fields.begin(), fields.end(), [](const auto& field) {
        return field.isMinToMax() || field.isMaxToMin();
    });
}

//
// Validity checking for bounds
//

bool OrderedIntervalList::isValidFor(int expectedOrientation) const {
    // Make sure each interval's start is oriented correctly with respect to its end.
    for (size_t j = 0; j < intervals.size(); ++j) {
        // false means don't consider field name.
        int cmp = sgn(intervals[j].end.woCompare(intervals[j].start, false));

        if (cmp == 0 && intervals[j].startInclusive && intervals[j].endInclusive) {
            continue;
        }

        if (cmp != expectedOrientation) {
            return false;
        }
    }

    // Make sure each interval is oriented correctly with respect to its neighbors.
    for (size_t j = 1; j < intervals.size(); ++j) {
        int cmp = sgn(intervals[j].start.woCompare(intervals[j - 1].end, false));

        // TODO: We could care if the end of one interval is the start of another.  The bounds
        // are still valid but they're a bit sloppy; they could have been combined to form one
        // interval if either of them is inclusive.
        if (0 == cmp) {
            continue;
        }

        if (cmp != expectedOrientation) {
            return false;
        }
    }
    return true;
}

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

bool IndexBounds::isValidFor(const BSONObj& keyPattern, int direction, bool hasNonSimpleCollation) {
    if (isSimpleRange) {
        bool valid = direction == sgn(endKey.woCompare(startKey, keyPattern, false));
        if (!valid) {
            LOGV2_OPTIONS(10921300,
                          {logv2::LogTruncation::Disabled},
                          "Index bounds are not valid for key pattern and direction",
                          "bounds"_attr = redact(toString(hasNonSimpleCollation)),
                          "keyPattern"_attr = keyPattern.toString(true /* redact values */),
                          "direction"_attr = direction,
                          "reason"_attr = "Simple range bounds are not in the right direction");
        }
        return valid;
    }

    BSONObjIterator it(keyPattern);

    for (size_t i = 0; i < fields.size(); ++i) {
        // We expect a bound for each field in the index.
        if (!it.more()) {
            LOGV2_OPTIONS(10921301,
                          {logv2::LogTruncation::Disabled},
                          "Index bounds are not valid for key pattern and direction",
                          "bounds"_attr = redact(toString(hasNonSimpleCollation)),
                          "keyPattern"_attr = keyPattern.toString(true /* redact values */),
                          "direction"_attr = direction,
                          "reason"_attr = "Not enough fields in key pattern");

            return false;
        }
        BSONElement elt = it.next();

        const OrderedIntervalList& field = fields[i];

        // Make sure the names match up.
        if (field.name != elt.fieldName()) {
            LOGV2_OPTIONS(10921302,
                          {logv2::LogTruncation::Disabled},
                          "Index bounds are not valid for key pattern and direction",
                          "bounds"_attr = redact(toString(hasNonSimpleCollation)),
                          "keyPattern"_attr = keyPattern.toString(true /* redact values */),
                          "direction"_attr = direction,
                          "fieldName"_attr = field.name,
                          "eltFieldName"_attr = elt.fieldName(),
                          "reason"_attr = "Field name does not match key pattern's");

            return false;
        }

        // Special indices are all inserted increasing.  elt.number() will return 0 if it's
        // not a number.  Special indices are strings, not numbers.
        int expectedOrientation = direction * ((elt.number() >= 0) ? 1 : -1);

        if (!field.isValidFor(expectedOrientation)) {
            LOGV2_OPTIONS(10921303,
                          {logv2::LogTruncation::Disabled},
                          "Index bounds are not valid for key pattern and direction",
                          "bounds"_attr = redact(toString(hasNonSimpleCollation)),
                          "keyPattern"_attr = keyPattern.toString(true /* redact values */),
                          "direction"_attr = direction,
                          "fieldName"_attr = field.name,
                          "expectedOrientation"_attr = expectedOrientation,
                          "reason"_attr = "Field is not valid for expected orientation");

            return false;
        }
    }

    if (it.more()) {
        LOGV2_OPTIONS(10921304,
                      {logv2::LogTruncation::Disabled},
                      "Index bounds are not valid for key pattern and direction",
                      "bounds"_attr = redact(toString(hasNonSimpleCollation)),
                      "keyPattern"_attr = keyPattern.toString(true /* redact values */),
                      "direction"_attr = direction,
                      "reason"_attr = "Too many fields in key pattern");
        return false;
    }
    return true;
}

#undef MONGO_LOGV2_DEFAULT_COMPONENT

//
// Iteration over index bounds
//

IndexBoundsChecker::IndexBoundsChecker(const IndexBounds* bounds,
                                       const BSONObj& keyPattern,
                                       int scanDirection)
    : _bounds(bounds), _curInterval(bounds->fields.size(), 0) {
    _keyValues.resize(_curInterval.size());

    BSONObjIterator it(keyPattern);
    while (it.more()) {
        int indexDirection = it.next().number() >= 0 ? 1 : -1;
        _expectedDirection.push_back(indexDirection * scanDirection);
    }
}

bool IndexBoundsChecker::getStartSeekPoint(IndexSeekPoint* out) {
    out->prefixLen = 0;
    out->firstExclusive = -1;
    out->keySuffix.resize(_bounds->fields.size());

    for (int i = _bounds->fields.size() - 1; i >= out->prefixLen; --i) {
        if (0 == _bounds->fields[i].intervals.size()) {
            return false;
        }
        out->keySuffix[i] = _bounds->fields[i].intervals[0].start;
        if (!_bounds->fields[i].intervals[0].startInclusive) {
            out->firstExclusive = i;
        }
    }

    return true;
}

bool IndexBoundsChecker::findLeftmostProblem(const vector<BSONElement>& keyValues,
                                             size_t* where,
                                             Location* what) {
    // For each field in the index key, see if it's in the interval it should be.
    for (size_t i = 0; i < _curInterval.size(); ++i) {
        const OrderedIntervalList& field = _bounds->fields[i];
        const Interval& currentInterval = field.intervals[_curInterval[i]];
        Location cmp = intervalCmp(currentInterval, keyValues[i], _expectedDirection[i]);

        // If it's not in the interval we think it is...
        if (0 != cmp) {
            *where = i;
            *what = cmp;
            return true;
        }
    }

    return false;
}

bool IndexBoundsChecker::spaceLeftToAdvance(size_t fieldsToCheck,
                                            const vector<BSONElement>& keyValues) {
    // Check end conditions.  Since we need to move the keys before
    // firstNonContainedField forward, let's make sure that those fields are not at the
    // end of their bounds.
    for (size_t i = 0; i < fieldsToCheck; ++i) {
        // Field 'i' isn't at its last interval.  There's possibly a key we could move forward
        // to, either in the current interval or the next one.
        if (_curInterval[i] != _bounds->fields[i].intervals.size() - 1) {
            return true;
        }

        // Field 'i' is at its last interval.
        const Interval& ival = _bounds->fields[i].intervals[_curInterval[i]];

        // We're OK if it's an open interval.  There are an infinite number of keys between any
        // key and the end point...
        if (!ival.endInclusive) {
            return true;
        }

        // If it's a closed interval, we're fine so long as we haven't hit the end point of
        // the interval.
        if (-_expectedDirection[i] == sgn(keyValues[i].woCompare(ival.end, false))) {
            return true;
        }
    }

    return false;
}

bool IndexBoundsChecker::isValidKey(const BSONObj& key) {
    BSONObjIterator it(key);
    size_t curOil = 0;
    while (it.more()) {
        BSONElement elt = it.next();
        size_t whichInterval;
        Location loc = findIntervalForField(
            elt, _bounds->fields[curOil], _expectedDirection[curOil], &whichInterval);
        if (WITHIN != loc) {
            return false;
        }
        ++curOil;
    }
    return true;
}

IndexBoundsChecker::KeyState IndexBoundsChecker::checkKeyWithEndPosition(
    const BSONObj& currentKey,
    IndexSeekPoint* query,
    key_string::Builder& endKey,
    Ordering ord,
    bool forward) {
    auto state = checkKey(currentKey, query);
    endKey.resetToEmpty(ord);
    if (state == VALID && !_bounds->fields.back().isPoint()) {
        auto size = _keyValues.size();
        std::vector<const BSONElement*> out(size);
        key_string::Discriminator discriminator;
        for (size_t i = 0; i < size - 1; ++i) {
            if (_keyValues[i]) {
                endKey.appendBSONElement(_keyValues[i]);
            }
        }
        const OrderedIntervalList& oil = _bounds->fields.back();
        endKey.appendBSONElement(oil.intervals[_curInterval.back()].end);
        if (oil.intervals[_curInterval.back()].endInclusive) {
            discriminator = key_string::Discriminator::kInclusive;
        } else {
            discriminator = forward ? key_string::Discriminator::kExclusiveBefore
                                    : key_string::Discriminator::kExclusiveAfter;
        }
        endKey.appendDiscriminator(discriminator);
    }
    return state;
}

IndexBoundsChecker::KeyState IndexBoundsChecker::checkKey(const BSONObj& key, IndexSeekPoint* out) {
    MONGO_verify(_curInterval.size() > 0);
    out->keySuffix.resize(_curInterval.size());

    // It's useful later to go from a field number to the value for that field.  Store these.
    size_t i = 0;
    BSONObjIterator keyIt(key);
    while (keyIt.more()) {
        MONGO_verify(i < _curInterval.size());

        _keyValues[i] = keyIt.next();
        i++;
    }
    MONGO_verify(i == _curInterval.size());

    size_t firstNonContainedField;
    Location orientation;

    if (!findLeftmostProblem(_keyValues, &firstNonContainedField, &orientation)) {
        // All fields in the index are within the current interval.  Caller can use the key.
        return VALID;
    }

    // Field number 'firstNonContainedField' of the index key is before its current interval.
    if (BEHIND == orientation) {
        // It's behind our current interval, but our current interval could be wrong.  Start all
        // intervals from firstNonContainedField to the right over...
        for (size_t i = firstNonContainedField; i < _curInterval.size(); ++i) {
            _curInterval[i] = 0;
        }

        // ...and try again.  This call modifies 'orientation', so we may check its value again
        // in the clause below if field number 'firstNonContainedField' isn't in its first
        // interval.
        if (!findLeftmostProblem(_keyValues, &firstNonContainedField, &orientation)) {
            return VALID;
        }
    }

    // Field number 'firstNonContainedField' of the index key is before all current intervals.
    if (BEHIND == orientation) {
        // Tell the caller to move forward to the start of the current interval.
        out->keyPrefix = key.getOwned();
        out->prefixLen = firstNonContainedField;
        out->firstExclusive = -1;

        for (int j = _curInterval.size() - 1; j >= out->prefixLen; --j) {
            const OrderedIntervalList& oil = _bounds->fields[j];
            out->keySuffix[j] = oil.intervals[_curInterval[j]].start;
            if (!oil.intervals[_curInterval[j]].startInclusive) {
                out->firstExclusive = j;
            }
        }

        return MUST_ADVANCE;
    }

    MONGO_verify(AHEAD == orientation);

    // Field number 'firstNonContainedField' of the index key is after interval we think it's
    // in.  Fields 0 through 'firstNonContained-1' are within their current intervals and we can
    // ignore them.
    while (firstNonContainedField < _curInterval.size()) {
        // Find the interval that contains our field.
        size_t newIntervalForField;

        Location where = findIntervalForField(_keyValues[firstNonContainedField],
                                              _bounds->fields[firstNonContainedField],
                                              _expectedDirection[firstNonContainedField],
                                              &newIntervalForField);

        if (WITHIN == where) {
            // Found a new interval for field firstNonContainedField.  Move our internal choice
            // of interval to that.
            _curInterval[firstNonContainedField] = newIntervalForField;
            // Let's find valid intervals for fields to the right.
            ++firstNonContainedField;
        } else if (BEHIND == where) {
            // firstNonContained field is between the intervals (newIntervalForField-1) and
            // newIntervalForField.  We have to tell the caller to move forward until he at
            // least hits our new current interval.
            _curInterval[firstNonContainedField] = newIntervalForField;

            // All other fields to the right start at their first interval.
            for (size_t i = firstNonContainedField + 1; i < _curInterval.size(); ++i) {
                _curInterval[i] = 0;
            }

            out->keyPrefix = key.getOwned();
            out->prefixLen = firstNonContainedField;
            out->firstExclusive = -1;
            for (int i = _curInterval.size() - 1; i >= out->prefixLen; --i) {
                const OrderedIntervalList& oil = _bounds->fields[i];
                out->keySuffix[i] = oil.intervals[_curInterval[i]].start;
                if (!oil.intervals[_curInterval[i]].startInclusive) {
                    out->firstExclusive = i;
                }
            }

            return MUST_ADVANCE;
        } else {
            MONGO_verify(AHEAD == where);
            // Field number 'firstNonContainedField' cannot possibly be placed into an interval,
            // as it is already past its last possible interval.  The caller must move forward
            // to a key with a greater value for the previous field.

            // If all fields to the left have hit the end of their intervals, we can't ask them
            // to move forward and we should stop iterating.
            if (!spaceLeftToAdvance(firstNonContainedField, _keyValues)) {
                return DONE;
            }

            out->keyPrefix = key.getOwned();
            out->prefixLen = firstNonContainedField;
            out->firstExclusive = firstNonContainedField - 1;

            for (size_t i = firstNonContainedField; i < _curInterval.size(); ++i) {
                _curInterval[i] = 0;
            }

            // If movePastKeyElts is true, we don't examine any fields after the keyEltsToUse
            // fields of the key.  As such we don't populate the out/incOut.
            return MUST_ADVANCE;
        }
    }

    MONGO_verify(firstNonContainedField == _curInterval.size());
    return VALID;
}

namespace {

/**
 * Returns true if key (first member of pair) is AHEAD of interval
 * along 'direction' (second member of pair).
 */
bool isKeyAheadOfInterval(const Interval& interval,
                          const std::pair<BSONElement, int>& keyAndDirection) {
    const BSONElement& elt = keyAndDirection.first;
    int expectedDirection = keyAndDirection.second;
    IndexBoundsChecker::Location where = intervalCmp(interval, elt, expectedDirection);
    return IndexBoundsChecker::AHEAD == where;
}

}  // namespace

// static
IndexBoundsChecker::Location IndexBoundsChecker::findIntervalForField(
    const BSONElement& elt,
    const OrderedIntervalList& oil,
    const int expectedDirection,
    size_t* newIntervalIndex) {
    // Binary search for interval.
    // Intervals are ordered in the same direction as our keys.
    // Key behind all intervals: [BEHIND, ..., BEHIND]
    // Key ahead of all intervals: [AHEAD, ..., AHEAD]
    // Key within one interval: [AHEAD, ..., WITHIN, BEHIND, ...]
    // Key not in any inteval: [AHEAD, ..., AHEAD, BEHIND, ...]

    // Find left-most BEHIND/WITHIN interval.
    vector<Interval>::const_iterator i = std::lower_bound(oil.intervals.begin(),
                                                          oil.intervals.end(),
                                                          std::make_pair(elt, expectedDirection),
                                                          isKeyAheadOfInterval);

    // Key ahead of all intervals.
    if (i == oil.intervals.end()) {
        return AHEAD;
    }

    // Found either interval containing key or left-most BEHIND interval.
    *newIntervalIndex = std::distance(oil.intervals.begin(), i);

    // Additional check to determine if interval contains key.
    Location where = intervalCmp(*i, elt, expectedDirection);
    invariant(BEHIND == where || WITHIN == where);

    return where;
}

bool isLowerBound(const BSONElement& value, bool isInclusive) {
    switch (value.type()) {
        case BSONType::numberInt:
        case BSONType::numberDouble:
        case BSONType::numberLong:
        case BSONType::numberDecimal:
            // Lower bound value for numbers.
            return (std::isinf(value.numberDouble()) || std::isnan(value.numberDouble())) &&
                isInclusive == true;
        case BSONType::string:
            // Lower bound value for strings.
            return value.str().empty() && isInclusive == true;
        case BSONType::date:
            // Lower bound value for dates.
            return value.date() == Date_t::min() && isInclusive == true;
        case BSONType::timestamp:
            // Lower bound value for timestamps.
            return value.timestamp() == Timestamp::min() && isInclusive == true;
        case BSONType::oid:
            // Lower bound value for ObjectID.
            return value.OID() == OID() && isInclusive == true;
        case BSONType::object:
        case BSONType::array:
            // Lower bound value for Object and Array.
            return value.Obj().isEmpty() && isInclusive == true;
        case BSONType::binData:
        case BSONType::eoo:
        case BSONType::minKey:
        case BSONType::maxKey:
        case BSONType::boolean:  // Boolean bounds are considered always open since they are
                                 // non-selective.
        case BSONType::null:
        case BSONType::undefined:
        case BSONType::symbol:
        case BSONType::regEx:
        case BSONType::dbRef:
        case BSONType::code:
        case BSONType::codeWScope:
            return true;
    }

    MONGO_UNREACHABLE_TASSERT(8102100);
}

bool isUpperBound(const BSONElement& value, bool isInclusive) {
    switch (value.type()) {
        case BSONType::numberInt:
        case BSONType::numberDouble:
        case BSONType::numberLong:
        case BSONType::numberDecimal:
            // Upper bound value for numbers.
            return std::isinf(value.numberDouble()) && isInclusive == true;
        case BSONType::string:
            // A string value cannot be an upper bound value.
            return false;
        case BSONType::date:
            // Upper bound value for Date.
            return value.date() == Date_t::max() && isInclusive == true;
        case BSONType::timestamp:
            // Upper bound value for Timestamp.
            return value.timestamp() == Timestamp::max() && isInclusive == true;
        case BSONType::oid:
            // Upper bound value for ObjectID.
            return value.OID() == OID::max() && isInclusive == true;
        case BSONType::object:
            // Upper bound value for String.
            return value.Obj().isEmpty() && isInclusive == false;
        case BSONType::array:
            // Upper bound value for Object.
            return value.Obj().isEmpty() && isInclusive == false;
        case BSONType::binData:
            // Upper bound value for Array.
            return value.valuesize() == 0 && isInclusive == false;
        case BSONType::eoo:
        case BSONType::minKey:
        case BSONType::maxKey:
        case BSONType::boolean:  // Boolean bounds are considered always open since they are
                                 // non-selective.
        case BSONType::null:
        case BSONType::undefined:
        case BSONType::symbol:
        case BSONType::regEx:
        case BSONType::dbRef:
        case BSONType::code:
        case BSONType::codeWScope:
            return true;
    }

    MONGO_UNREACHABLE_TASSERT(8102101);
}

}  // namespace mongo
