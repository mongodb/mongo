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

namespace mongo {

    namespace {

        // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
        int sgn(int i) {
            if (i == 0)
                return 0;
            return i > 0 ? 1 : -1;
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
        }
        else {
            return Interval();
        }
    }

    string IndexBounds::toString() const {
        stringstream ss;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                ss << ", ";
            }
            const OrderedIntervalList& oil = fields[i];
            ss << "field #" << i << "['" << oil.name << "']: ";
            for (size_t j = 0; j < oil.intervals.size(); ++j) {
                const Interval& iv = oil.intervals[j];
                if (iv.startInclusive) {
                    ss << "[";
                }
                else {
                    ss << "(";
                }
                // false means omit the field name
                ss << iv.start.toString(false);
                ss << ", ";
                ss << iv.end.toString(false);
                if (iv.endInclusive) {
                    ss << "]";
                }
                else {
                    ss << ")";
                }
            }
        }

        return ss.str();
    }

    //
    // Validity checking for bounds
    //

    bool IndexBounds::isValidFor(const BSONObj& keyPattern, int direction) {
        BSONObjIterator it(keyPattern);

        for (size_t i = 0; i < fields.size(); ++i) {
            // We expect a bound for each field in the index.
            if (!it.more()) { return false; }
            BSONElement elt = it.next();

            const OrderedIntervalList& field = fields[i];

            // Make sure the names match up.
            if (field.name != elt.fieldName()) { return false; }

            // Special indices are all inserted increasing.  elt.number() will return 0 if it's
            // not a number.  Special indices are strings, not numbers.
            int expectedOrientation = direction * ((elt.number() >= 0) ? 1 : -1);

            // Make sure each interval's start is oriented correctly with respect to its end.
            for (size_t j = 0; j < field.intervals.size(); ++j) {
                // false means don't consider field name.
                int cmp = sgn(field.intervals[j].end.woCompare(field.intervals[j].start, false));

                if (cmp == 0 && field.intervals[j].startInclusive
                    && field.intervals[j].endInclusive) { continue; }

                if (cmp != expectedOrientation) { return false; }
            }

            // Make sure each interval is oriented correctly with respect to its neighbors.
            for (size_t j = 1; j < field.intervals.size(); ++j) {
                int cmp = sgn(field.intervals[j].start.woCompare(field.intervals[j - 1].end,
                                                                 false));

                if (cmp == 0) {
                    // The end of one interval is the start of another.  This is only valid if
                    // they're both open intervals.  Otherwise, it should have been combined to form
                    // one interval.
                    if (field.intervals[j].startInclusive || field.intervals[j - 1].endInclusive) {
                        return false;
                    }
                }
                else if (cmp != expectedOrientation) {
                    return false;
                }
            }
        }

        return !it.more();
    }

    //
    // Iteration over index bounds
    //

    IndexBoundsChecker::IndexBoundsChecker(const IndexBounds* bounds, const BSONObj& keyPattern,
                                             int scanDirection)
        : _bounds(bounds), _curInterval(bounds->fields.size(), 0) {

        BSONObjIterator it(keyPattern);
        while (it.more()) {
            int indexDirection = it.next().number() >= 0 ? 1 : -1;
            _expectedDirection.push_back(indexDirection * scanDirection);
        }
    }

    void IndexBoundsChecker::getStartKey(vector<const BSONElement*>* valueOut,
                                          vector<bool>* inclusiveOut) {
        for (size_t i = 0; i < _bounds->fields.size(); ++i) {
            (*valueOut)[i] = &_bounds->fields[i].intervals[0].start;
            (*inclusiveOut)[i] = _bounds->fields[i].intervals[0].startInclusive;
        }
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

    IndexBoundsChecker::KeyState IndexBoundsChecker::checkKey(const BSONObj& key,
                                                                int* keyEltsToUse,
                                                                bool* movePastKeyElts,
                                                                vector<const BSONElement*>* out,
                                                                vector<bool>* incOut) {
        verify(_curInterval.size() > 0);
        verify(out->size() == _curInterval.size());
        verify(incOut->size() == _curInterval.size());

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
        // Tell the caller to move forward to the start of the current interval.
        if (BEHIND == orientation) {
            *keyEltsToUse = firstNonContainedField;
            *movePastKeyElts = false;

            for (size_t j = firstNonContainedField; j < _curInterval.size(); ++j) {
                const OrderedIntervalList& oil = _bounds->fields[j];
                (*out)[j] = &oil.intervals[_curInterval[j]].start;
                (*incOut)[j] = oil.intervals[_curInterval[j]].startInclusive;
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
            }
            else if (BEHIND == where) {
                // firstNonContained field is between the intervals (newIntervalForField-1) and
                // newIntervalForField.  We have to tell the caller to move forward until he at
                // least hits our new current interval.
                _curInterval[firstNonContainedField] = newIntervalForField;

                // All other fields to the right start at their first interval.
                for (size_t i = firstNonContainedField + 1; i < _curInterval.size(); ++i) {
                    _curInterval[i] = 0;
                }

                *keyEltsToUse = firstNonContainedField;
                *movePastKeyElts = false;

                for (size_t i = firstNonContainedField; i < _curInterval.size(); ++i) {
                    const OrderedIntervalList& oil = _bounds->fields[i];
                    (*out)[i] = &oil.intervals[_curInterval[i]].start;
                    (*incOut)[i] = oil.intervals[_curInterval[i]].startInclusive;
                }

                return MUST_ADVANCE;
            }
            else {
                verify (AHEAD == where);
                // Field number 'firstNonContainedField' cannot possibly be placed into an interval,
                // as it is already past its last possible interval.  The caller must move forward
                // to a key with a greater value for the previous field.

                // If all fields to the left have hit the end of their intervals, we can't ask them
                // to move forward and we should stop iterating.
                if (!spaceLeftToAdvance(firstNonContainedField, keyValues)) {
                    return DONE;
                }

                *keyEltsToUse = firstNonContainedField;
                *movePastKeyElts = true;

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

    // static
    IndexBoundsChecker::Location IndexBoundsChecker::intervalCmp(const Interval& interval,
                                                                   const BSONElement& key,
                                                                   const int expectedDirection) {
        int cmp = sgn(key.woCompare(interval.start, false));
        bool startOK = (cmp == expectedDirection) || (cmp == 0 && interval.startInclusive);
        if (!startOK) { return BEHIND; }

        cmp = sgn(key.woCompare(interval.end, false));
        bool endOK = (cmp == -expectedDirection) || (cmp == 0 && interval.endInclusive);
        if (!endOK) { return AHEAD; }

        return WITHIN;
    }

    // static
    IndexBoundsChecker::Location IndexBoundsChecker::findIntervalForField(const BSONElement& elt,
            const OrderedIntervalList& oil, const int expectedDirection, size_t* newIntervalIndex) {

        for (size_t i = 0; i < oil.intervals.size(); ++i) {
            Location where = intervalCmp(oil.intervals[i], elt, expectedDirection);

            // Intervals are ordered in the same direction as our keys.  The first interval we
            // aren't ahead of is the one we're looking for.
            if (AHEAD != where) {
                *newIntervalIndex = i;
                return where;
            }
        }

        // If we're here, we're ahead of all intervals.
        return AHEAD;
    }

}  // namespace mongo
