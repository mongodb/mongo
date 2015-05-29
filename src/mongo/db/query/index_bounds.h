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
#include "mongo/db/storage/index_entry_comparison.h"

namespace mongo {

/**
 * An ordered list of intervals for one field.
 */
struct OrderedIntervalList {
    OrderedIntervalList() {}
    OrderedIntervalList(const std::string& n) : name(n) {}

    // Must be ordered according to the index order.
    std::vector<Interval> intervals;

    std::string name;

    bool isValidFor(int expectedOrientation) const;
    std::string toString() const;

    /**
     * Complements the OIL. Used by the index bounds builder in order
     * to create index bounds for $not predicates.
     *
     * Assumes the OIL is increasing, and therefore must be called prior to
     * alignBounds(...).
     *
     * Example:
     *   The complement of [3, 6), [8, 10] is [MinKey, 3), [6, 8), (20, MaxKey],
     *   where this OIL has direction==1.
     */
    void complement();

    bool operator==(const OrderedIntervalList& other) const;
    bool operator!=(const OrderedIntervalList& other) const;
};

/**
 * Tied to an index.  Permissible values for all fields in the index.  Requires the index to
 * interpret.  Previously known as FieldRangeVector.
 */
struct IndexBounds {
    IndexBounds() : isSimpleRange(false), endKeyInclusive(false) {}

    // For each indexed field, the values that the field is allowed to take on.
    std::vector<OrderedIntervalList> fields;

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

    bool operator==(const IndexBounds& other) const;
    bool operator!=(const IndexBounds& other) const;

    /**
     * BSON format for explain. The format is an array of strings for each field.
     * Each string represents an interval. The strings use "[" and "]" if the interval
     * bounds are inclusive, and "(" / ")" if exclusive.
     *
     * Ex.
     *  {a: ["[1, 1]", "(3, 10)"], b: ["[Infinity, 10)"] }
     */
    BSONObj toBSON() const;

    // TODO: we use this for max/min scan.  Consider migrating that.
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
     * Get the IndexSeekPoint that we should with.
     *
     * Returns false if there are no possible index entries that match the bounds. In this case
     * there is no valid start point to seek to so out will not be filled out and the caller
     * should emit no results.
     */
    bool getStartSeekPoint(IndexSeekPoint* out);

    /**
     * The states of a key from an index scan.  See checkKey below.
     */
    enum KeyState {
        VALID,
        MUST_ADVANCE,
        DONE,
    };

    /**
     * Is 'key' a valid key?  Note that this differs from checkKey, which assumes that it
     * receives keys in sorted order.
     */
    bool isValidKey(const BSONObj& key);

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
     * Returns MUST_ADVANCE.  Caller must advance to the query provided in the out parameters
     * and call checkKey again.
     *
     * 3. The key is past our bounds.  Returns DONE.  No further keys will satisfy the bounds
     * and the caller should stop.
     *
     * keyEltsToUse, movePastKeyElts, out, and incOut must all be non-NULL.
     * out and incOut must already be resized to have as many elements as the key has fields.
     *
     * In parameters:
     * currentKey is the index key.
     *
     * Out parameter only valid if we return MUST_ADVANCE.
     */
    KeyState checkKey(const BSONObj& currentKey, IndexSeekPoint* query);

    /**
     * Relative position of a key to an interval.
     * Exposed for testing only.
     */
    enum Location {
        BEHIND = -1,
        WITHIN = 0,
        AHEAD = 1,
    };

    /**
     * If 'elt' is in any interval, return WITHIN and set 'newIntervalIndex' to the index of the
     * interval in the ordered interval list.
     *
     * If 'elt' is not in any interval but could be advanced to be in one, return BEHIND and set
     * 'newIntervalIndex' to the index of the interval that 'elt' could be advanced to.
     *
     * If 'elt' cannot be advanced to any interval, return AHEAD.
     *
     * Exposed for testing only.
     *
     * TODO(efficiency): Start search from a given index.
     */
    static Location findIntervalForField(const BSONElement& elt,
                                         const OrderedIntervalList& oil,
                                         const int expectedDirection,
                                         size_t* newIntervalIndex);

private:
    /**
     * Find the first field in the key that isn't within the interval we think it is.  Returns
     * false if every field is in the interval we think it is.  Returns true and populates out
     * parameters if a field isn't in the interval we think it is.
     *
     * Out parameters set if we return true:
     * 'where' is the leftmost field that isn't in the interval we think it is.
     * 'what' is the orientation of the field with respect to that interval.
     */
    bool findLeftmostProblem(const std::vector<BSONElement>& keyValues,
                             size_t* where,
                             Location* what);

    /**
     * Returns true if it's possible to advance any of the first 'fieldsToCheck' fields of the
     * index key and still be within valid index bounds.
     *
     * keyValues are the elements of the index key in order.
     */
    bool spaceLeftToAdvance(size_t fieldsToCheck, const std::vector<BSONElement>& keyValues);

    // The actual bounds.  Must outlive this object.  Not owned by us.
    const IndexBounds* _bounds;

    // For each field, which interval are we currently in?
    std::vector<size_t> _curInterval;

    // Direction of scan * direction of indexing.
    std::vector<int> _expectedDirection;
};

}  // namespace mongo
