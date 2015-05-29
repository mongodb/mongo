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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/index_bounds.h"

#include <algorithm>
#include <tuple>
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
        return std::tie(this->startKey, this->endKey, this->endKeyInclusive) ==
            std::tie(other.startKey, other.endKey, other.endKeyInclusive);
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

string OrderedIntervalList::toString() const {
    mongoutils::str::stream ss;
    ss << "['" << name << "']: ";
    for (size_t j = 0; j < intervals.size(); ++j) {
        ss << intervals[j].toString();
        if (j < intervals.size() - 1) {
            ss << ", ";
        }
    }
    return ss;
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

// static
void OrderedIntervalList::complement() {
    BSONObjBuilder minBob;
    minBob.appendMinKey("");
    BSONObj minObj = minBob.obj();

    // We complement by scanning the entire range of BSON values
    // from MinKey to MaxKey. The value from which we must begin
    // the next complemented interval is kept in 'curBoundary'.
    BSONElement curBoundary = minObj.firstElement();

    // If 'curInclusive' is true, then 'curBoundary' is
    // included in one of the original intervals, and hence
    // should not be included in the complement (and vice-versa
    // if 'curInclusive' is false).
    bool curInclusive = false;

    // We will build up a list of intervals that represents
    // the inversion of those in the OIL.
    vector<Interval> newIntervals;
    for (size_t j = 0; j < intervals.size(); ++j) {
        Interval curInt = intervals[j];
        if (0 != curInt.start.woCompare(curBoundary) || (!curInclusive && !curInt.startInclusive)) {
            // Make a new interval from 'curBoundary' to
            // the start of 'curInterval'.
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
    if (0 != maxKey.woCompare(curBoundary) || !curInclusive) {
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

string IndexBounds::toString() const {
    mongoutils::str::stream ss;
    if (isSimpleRange) {
        ss << "[" << startKey.toString() << ", ";
        if (endKey.isEmpty()) {
            ss << "]";
        } else {
            ss << endKey.toString();
            if (endKeyInclusive) {
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
        ss << "field #" << i << fields[i].toString();
    }

    return ss;
}

BSONObj IndexBounds::toBSON() const {
    BSONObjBuilder bob;
    vector<OrderedIntervalList>::const_iterator itField;
    for (itField = fields.begin(); itField != fields.end(); ++itField) {
        BSONArrayBuilder fieldBuilder(bob.subarrayStart(itField->name));

        vector<Interval>::const_iterator itInterval;
        for (itInterval = itField->intervals.begin(); itInterval != itField->intervals.end();
             ++itInterval) {
            std::string intervalStr = itInterval->toString();

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

bool IndexBounds::isValidFor(const BSONObj& keyPattern, int direction) {
    if (isSimpleRange) {
        return direction == sgn(endKey.woCompare(startKey, keyPattern, false));
    }

    BSONObjIterator it(keyPattern);

    for (size_t i = 0; i < fields.size(); ++i) {
        // We expect a bound for each field in the index.
        if (!it.more()) {
            return false;
        }
        BSONElement elt = it.next();

        const OrderedIntervalList& field = fields[i];

        // Make sure the names match up.
        if (field.name != elt.fieldName()) {
            return false;
        }

        // Special indices are all inserted increasing.  elt.number() will return 0 if it's
        // not a number.  Special indices are strings, not numbers.
        int expectedOrientation = direction * ((elt.number() >= 0) ? 1 : -1);

        if (!field.isValidFor(expectedOrientation)) {
            return false;
        }
    }

    return !it.more();
}

//
// Iteration over index bounds
//

IndexBoundsChecker::IndexBoundsChecker(const IndexBounds* bounds,
                                       const BSONObj& keyPattern,
                                       int scanDirection)
    : _bounds(bounds), _curInterval(bounds->fields.size(), 0) {
    BSONObjIterator it(keyPattern);
    while (it.more()) {
        int indexDirection = it.next().number() >= 0 ? 1 : -1;
        _expectedDirection.push_back(indexDirection * scanDirection);
    }
}

bool IndexBoundsChecker::getStartSeekPoint(IndexSeekPoint* out) {
    out->prefixLen = 0;
    out->prefixExclusive = false;
    out->keySuffix.resize(_bounds->fields.size());
    out->suffixInclusive.resize(_bounds->fields.size());

    for (size_t i = 0; i < _bounds->fields.size(); ++i) {
        if (0 == _bounds->fields[i].intervals.size()) {
            return false;
        }
        out->keySuffix[i] = &_bounds->fields[i].intervals[0].start;
        out->suffixInclusive[i] = _bounds->fields[i].intervals[0].startInclusive;
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

IndexBoundsChecker::KeyState IndexBoundsChecker::checkKey(const BSONObj& key, IndexSeekPoint* out) {
    verify(_curInterval.size() > 0);
    out->keySuffix.resize(_curInterval.size());
    out->suffixInclusive.resize(_curInterval.size());

    // It's useful later to go from a field number to the value for that field.  Store these.
    // TODO: on optimization pass, populate the vector as-needed and keep the vector around as a
    // member variable
    vector<BSONElement> keyValues;
    BSONObjIterator keyIt(key);
    while (keyIt.more()) {
        keyValues.push_back(keyIt.next());
    }
    verify(keyValues.size() == _curInterval.size());

    size_t firstNonContainedField;
    Location orientation;

    if (!findLeftmostProblem(keyValues, &firstNonContainedField, &orientation)) {
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
        if (!findLeftmostProblem(keyValues, &firstNonContainedField, &orientation)) {
            return VALID;
        }
    }

    // Field number 'firstNonContainedField' of the index key is before all current intervals.
    if (BEHIND == orientation) {
        // Tell the caller to move forward to the start of the current interval.
        out->keyPrefix = key.getOwned();
        out->prefixLen = firstNonContainedField;
        out->prefixExclusive = false;

        for (size_t j = firstNonContainedField; j < _curInterval.size(); ++j) {
            const OrderedIntervalList& oil = _bounds->fields[j];
            out->keySuffix[j] = &oil.intervals[_curInterval[j]].start;
            out->suffixInclusive[j] = oil.intervals[_curInterval[j]].startInclusive;
        }

        return MUST_ADVANCE;
    }

    verify(AHEAD == orientation);

    // Field number 'firstNonContainedField' of the index key is after interval we think it's
    // in.  Fields 0 through 'firstNonContained-1' are within their current intervals and we can
    // ignore them.
    while (firstNonContainedField < _curInterval.size()) {
        // Find the interval that contains our field.
        size_t newIntervalForField;

        Location where = findIntervalForField(keyValues[firstNonContainedField],
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
            out->prefixExclusive = false;
            for (size_t i = firstNonContainedField; i < _curInterval.size(); ++i) {
                const OrderedIntervalList& oil = _bounds->fields[i];
                out->keySuffix[i] = &oil.intervals[_curInterval[i]].start;
                out->suffixInclusive[i] = oil.intervals[_curInterval[i]].startInclusive;
            }

            return MUST_ADVANCE;
        } else {
            verify(AHEAD == where);
            // Field number 'firstNonContainedField' cannot possibly be placed into an interval,
            // as it is already past its last possible interval.  The caller must move forward
            // to a key with a greater value for the previous field.

            // If all fields to the left have hit the end of their intervals, we can't ask them
            // to move forward and we should stop iterating.
            if (!spaceLeftToAdvance(firstNonContainedField, keyValues)) {
                return DONE;
            }

            out->keyPrefix = key.getOwned();
            out->prefixLen = firstNonContainedField;
            out->prefixExclusive = true;

            for (size_t i = firstNonContainedField; i < _curInterval.size(); ++i) {
                _curInterval[i] = 0;
            }

            // If movePastKeyElts is true, we don't examine any fields after the keyEltsToUse
            // fields of the key.  As such we don't populate the out/incOut.
            return MUST_ADVANCE;
        }
    }

    verify(firstNonContainedField == _curInterval.size());
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

}  // namespace mongo
