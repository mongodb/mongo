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

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/db/storage/index_entry_comparison.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mongo {

enum class BoundInclusion {
    kExcludeBothStartAndEndKeys,
    kIncludeStartKeyOnly,
    kIncludeEndKeyOnly,
    kIncludeBothStartAndEndKeys
};

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
    /**
     * Generates a debug string for an interval. If 'hasNonSimpleCollation' is true, then string
     * bounds are hex-encoded.
     */
    std::string toString(bool hasNonSimpleCollation) const;

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

    void reverse();

    /**
     * Return a clone of this OIL, that is reversed.
     */
    OrderedIntervalList reverseClone() const;

    Interval::Direction computeDirection() const;

    /**
     * Returns true if this OIL represents a single [MinKey, MaxKey] bound.
     */
    bool isMinToMax() const;

    /**
     * Returns true if this OIL represents a single [MaxKey, MinKey] bound.
     */
    bool isMaxToMin() const;

    /**
     * Returns true if this OIL has a single [MinKey, MaxKey] or [MaxKey, MinKey] interval.
     */
    bool isFullyOpen() const;

    /**
     * Returns true if this OIL represents a point predicate: [N, N].
     *
     * These predicates are interesting because if you have an index on {a:1, b:1},
     * and a point predicate on 'a', then the index provides a sort on {b: 1}.
     */
    bool isPoint() const;

    /**
     * Returns true if this OIL contains only point intervals (such as [N, N]).
     */
    bool containsOnlyPointIntervals() const;

    /**
     * Returns true is this OIL overlaps the type bracket containing objects.
     */
    bool boundsOverlapObjectTypeBracket() const;
};

/**
 * Tied to an index.  Permissible values for all fields in the index.  Requires the index to
 * interpret.  Previously known as FieldRangeVector.
 */
struct IndexBounds {
    IndexBounds() : isSimpleRange(false), boundInclusion(BoundInclusion::kIncludeStartKeyOnly) {}

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
    bool isValidFor(const BSONObj& keyPattern, int direction, bool hasNonSimpleCollation);

    // Methods below used for debugging purpose only. Do not use outside testing code.
    size_t size() const;
    std::string getFieldName(size_t i) const;
    size_t getNumIntervals(size_t i) const;
    Interval getInterval(size_t i, size_t j) const;
    /**
     * Generates a debug string displaying the index bounds. If 'hasNonSimpleCollation' is true,
     * then string bounds are hex-encoded.
     */
    std::string toString(bool hasNonSimpleCollation) const;

    bool operator==(const IndexBounds& other) const;
    bool operator!=(const IndexBounds& other) const;

    /**
     * Returns if the start key should be included in the bounds specified by the given
     * BoundInclusion.
     */
    static bool isStartIncludedInBound(BoundInclusion boundInclusion);

    /**
     * Returns if the end key should be included in the bounds specified by the given
     * BoundInclusion.
     */
    static bool isEndIncludedInBound(BoundInclusion boundInclusion);

    /**
     * Returns a BoundInclusion given two booleans of whether to included the start key and the end
     * key.
     */
    static BoundInclusion makeBoundInclusionFromBoundBools(bool startKeyInclusive,
                                                           bool endKeyInclusive);

    /**
     * Reverse the BoundInclusion.
     */
    static BoundInclusion reverseBoundInclusion(BoundInclusion b);


    /**
     * BSON format for explain. The format is an array of strings for each field.
     * Each string represents an interval. The strings use "[" and "]" if the interval
     * bounds are inclusive, and "(" / ")" if exclusive.
     *
     * Ex.
     *  {a: ["[1, 1]", "(3, 10)"], b: ["[Infinity, 10)"] }
     *
     * If the index bounds are associated with a collation ('hasNonSimpleCollation'), then we will
     * hex-encode the collation keys.
     */
    BSONObj toBSON(bool hasNonSimpleCollation) const;

    /**
     * Return a copy of the index bounds, but with each of the OILs going in the ascending
     * direction.
     */
    IndexBounds forwardize() const;

    /**
     * Return a copy of the index bounds that has each OIL going in the direction opposite to its
     * direction in this IndexBounds.
     */
    IndexBounds reverse() const;

    /**
     * Returns whether these index bounds represent being unbounded.
     */
    bool isUnbounded() const;

    // TODO: we use this for max/min scan.  Consider migrating that.
    bool isSimpleRange;
    BSONObj startKey;
    BSONObj endKey;
    BoundInclusion boundInclusion;
};

class IndexBoundsChecker;
namespace sbe::size_estimator {
size_t estimate(const IndexBoundsChecker&);
}  // namespace sbe::size_estimator

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
     * The function is same as above `checkKey` plus returning the end key position when the last
     * field of the index is a range interval.
     *
     * The end key will be set to empty if KeyState is not VALID or the last field is not a range
     * interval.
     */
    KeyState checkKeyWithEndPosition(const BSONObj& currentKey,
                                     IndexSeekPoint* query,
                                     key_string::Builder& endKey,
                                     Ordering ord,
                                     bool forward);

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
                                         int expectedDirection,
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

    std::vector<BSONElement> _keyValues;

    friend size_t sbe::size_estimator::estimate(const IndexBoundsChecker&);
};

/**
 * Returns true if the value can serve as a type lower bound for the purposes of type bracketing.
 * The function is designed to work with the 'interesting' for index prefix heuristic types only:
 * Number, String, Date, Timestamp, Boolean, Object, Array, ObjectId. For other types it may return
 * false positive results. The code of the function is based on index bounds build logic from
 * 'index_bounds_builder.cpp'.
 */
bool isLowerBound(const BSONElement& value, bool isInclusive);

/**
 * Returns true if the value can serve as a type upper bound for the purposes of type bracketing.
 * The function is designed to work with the 'interesting' for index prefix heuristic types only:
 * Number, String, Date, Timestamp, Boolean, Object, Array, ObjectId. For other types it may return
 * false positive results. The code of the function is based on index bounds build logic from
 * 'index_bounds_builder.cpp'.
 */
bool isUpperBound(const BSONElement& value, bool isInclusive);

}  // namespace mongo
