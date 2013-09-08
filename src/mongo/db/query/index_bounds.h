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

#pragma once

#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/query/interval.h"

namespace mongo {

    /**
     * An ordered list of intervals for one field.
     */
    struct OrderedIntervalList {
        OrderedIntervalList(const string& n) : name(n) { }

        // Must be ordered according to the index order.
        vector<Interval> intervals;

        // TODO: We could drop this.  Only used in IndexBounds::isValidFor.
        string name;
    };

    /**
     * Tied to an index.  Permissible values for all fields in the index.  Requires the index to
     * interpret.  Previously known as FieldRangeVector.
     */
    struct IndexBounds {
        // For each indexed field, the values that the field is allowed to take on.
        vector<OrderedIntervalList> fields;

        // Debugging check.
        // We must have as many fields the key pattern does.
        // The fields must be oriented in the direction we'd encounter them given the indexing
        // direction (the value of the field in keyPattern) and index traversal direction provided
        // by 'direction'.
        //
        // An example: [7, 20]
        // We can traverse this forward if indexed ascending
        // We can traverse this backwards if indexed descending.
        bool isValidFor(const BSONObj& keyPattern, int direction);

        // Methods below used for debugging purpose only. Do not use outside testing code.
        size_t size() const;
        std::string getFieldName(size_t i) const;
        size_t getNumIntervals(size_t i) const;
        Interval getInterval(size_t i, size_t j) const;
        std::string toString() const;

        // TODO: KILL THIS?
        // We need this for legacy non-index indices (2d/2dsphere) that take a BSONObj and don't
        // deal with the kind of absurd Btree-only behavior of IndexBoundsChecker.
        bool isSimpleRange;
        BSONObj startKey;
        BSONObj endKey;
        bool endKeyInclusive;
    };

    /**
     * A helper used by IndexScan to navigate an index.
     */
    class IndexBoundsChecker {
    public:
        /**
         * keyPattern is the index that we're iterating over.
         * bounds are the bounds we're allowed to iterate over.
         * direction is the direction we're moving over the index, 1 or -1.
         *
         * Bounds not owned by us.
         */
        IndexBoundsChecker(const IndexBounds* bounds, const BSONObj& keyPattern, int direction);

        /**
         * Get the key that we should with.
         */
        void getStartKey(vector<const BSONElement*>* valueOut, vector<bool>* inclusiveOut);

        /**
         * The states of a key from an index scan.  See checkKey below.
         */
        enum KeyState {
            VALID,
            MUST_ADVANCE,
            DONE,
        };

        /**
         * This function checks if the key is within the bounds we're iterating over and updates any
         * internal state required to efficiently determine if the key is within our bounds.
         *
         * Possible outcomes:
         *
         * 1. The key is in our bounds.  Returns VALID.  Caller can use the data associated with the
         * key.
         *
         * 2. The key is not in our bounds but has not exceeded the maximum value in our bounds.
         * Returns MUST_ADVANCE.  Caller must advance to the key provided in the out parameters and
         * call checkKey again.
         *
         * 3. The key is past our bounds.  Returns DONE.  No further keys will satisfy the bounds
         * and the caller should stop.
         *
         * keyEltsToUse, movePastKeyElts, out, and incOut must all be non-NULL.
         * out and incOut must already be resized to have as many elements as the key has fields.
         *
         * In parameters:
         * key is the index key.
         *
         * Out parameters, only valid if we return MUST_ADVANCE:
         *
         * keyEltsToUse: The key that the caller should advance to is made up of the first
         *               'keyEltsToUse' of the key that was provided.
         *
         * movePastKeyElts: If true, the caller must only use the first 'keyEltsToUse' of the
         *                  provided key to form its key.  It moves to the first key that is after
         *                  the key formed by only using those elements.
         *
         * out: If keyEltsToUse is less than the number of indexed fields in the key, the remaining
         *      fields are taken from here.  out is not filled from the start but from the position
         *      that the key corresponds to.  An example:  If keyEltsToUse is 1, movePastKeyElts is
         *      false, and the index we're iterating over has two fields, out[1] will have the value
         *      for the second field.
         *
         * incOut: If the i-th element is false, seek to the key *after* the i-th element of out.
         *         If the i-th element is true, seek to the i-th element of out.
         */
        KeyState checkKey(const BSONObj& key, int* keyEltsToUse, bool* movePastKeyElts,
                          vector<const BSONElement*>* out, vector<bool>* incOut);

    private:
        enum Location {
            BEHIND = -1,
            WITHIN = 0,
            AHEAD = 1,
        };

        /**
         * Find the first field in the key that isn't within the interval we think it is.  Returns
         * false if every field is in the interval we think it is.  Returns true and populates out
         * parameters if a field isn't in the interval we think it is.
         *
         * Out parameters set if we return true:
         * 'where' is the leftmost field that isn't in the interval we think it is.
         * 'what' is the orientation of the field with respect to that interval.
         */
        bool findLeftmostProblem(const vector<BSONElement>& keyValues, size_t* where,
                                 Location* what);

        /**
         * Returns true if it's possible to advance any of the first 'fieldsToCheck' fields of the
         * index key and still be within valid index bounds.
         *
         * keyValues are the elements of the index key in order.
         */
        bool spaceLeftToAdvance(size_t fieldsToCheck, const vector<BSONElement>& keyValues);

        /**
         * Returns BEHIND if the key is behind the interval.
         * Returns WITHIN if the key is within the interval.
         * Returns AHEAD if the key is ahead the interval.
         *
         * All directions are oriented along 'direction'.
         */
        static Location intervalCmp(const Interval& interval, const BSONElement& key,
                                    const int expectedDirection);

        /**
         * If 'elt' is in any interval, return WITHIN and set 'newIntervalIndex' to the index of the
         * interval in the ordered interval list.
         *
         * If 'elt' is not in any interval but could be advanced to be in one, return BEHIND and set
         * 'newIntervalIndex' to the index of the interval that 'elt' could be advanced to.
         *
         * If 'elt' cannot be advanced to any interval, return AHEAD.
         *
         * TODO(efficiency): Start search from a given index.
         * TODO(efficiency): Binary search for the answer.
         */
        static Location findIntervalForField(const BSONElement &elt, const OrderedIntervalList& oil,
                                             const int expectedDirection, size_t* newIntervalIndex);

        // The actual bounds.  Must outlive this object.  Not owned by us.
        const IndexBounds* _bounds;

        // For each field, which interval are we currently in?
        vector<size_t> _curInterval;

        // Direction of scan * direction of indexing.
        vector<int> _expectedDirection;
    };

}  // namespace mongo
