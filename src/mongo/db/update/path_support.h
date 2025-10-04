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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/util/ctype.h"
#include "mongo/util/string_map.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>

namespace mongo {

namespace pathsupport {

// Cap on the number of nulls we'll add to an array if we're inserting to an index that
// doesn't exist.
static const size_t kMaxPaddingAllowed = 1500000;

// Convenience type to hold equality matches at particular paths from a MatchExpression
using EqualityMatches = StringDataMap<const EqualityMatchExpression*>;

struct cmpPathsAndArrayIndexes {
    // While there is a string to number parser in the codebase, we use this for performance
    // reasons. This code allows us to only look at the characters/digits that are relevant for our
    // comparisons.
    bool operator()(const std::string& a, const std::string& b) const {
        auto aLength = a.size();
        auto bLength = b.size();
        // If these are not numbers, do a normal string comparison. Treat zero padded numbers as
        // strings.
        if (aLength == 0 || bLength == 0 || !ctype::isDigit(a[0]) || !ctype::isDigit(b[0]) ||
            (a[0] == '0' && aLength > 1) || (b[0] == '0' && bLength > 1)) {
            return a < b;
        }

        // At this point we are only seeing strings that are prefixed with at least one digit. If
        // they are numbers, the longer one is bigger because it has a larger order of magnitude.
        // If they are not numbers (ex: 123qyz) we would not be able to distinguish which is larger
        // without inspecting every character. To avoid this work we choose to order by length
        // because the actual order is not important but needs to be consistent.
        if (aLength != bLength) {
            return aLength < bLength;
        }
        // Find the larger number. Return immediately if a high order digit doesnt match, or we find
        // a non-number.
        size_t aIndex = 0;
        size_t bIndex = 0;
        while (ctype::isDigit(a[aIndex]) && ctype::isDigit(b[bIndex]) && aIndex < aLength &&
               bIndex < bLength) {
            if (a[aIndex] == b[bIndex]) {
                ++aIndex;
                ++bIndex;
                continue;
            }
            return a[aIndex] < b[bIndex];
        }
        // We found a non-number, perform normal string comparison.
        return a < b;
    }
};

/**
 * Finds the longest portion of 'prefix' that exists in document rooted at 'root' and is
 * "viable." A viable path is one that, if fully created on a given doc, would not
 * change the existing types of any fields in that doc. (See examples below.)
 *
 * If a prefix indeed exists, 'idxFound' is set to indicate how many parts in common
 * 'prefix' and 'doc' have. 'elemFound' would point to the Element corresponding to
 * prefix[idxFound] in 'doc'. The call would return an OK status in this case.
 *
 * If a prefix is not viable, returns a status "PathNotViable". 'idxFound' is set to
 * indicate the part in the document that caused the path to be not viable. 'elemFound'
 * would point to the Element corresponding to prefix[idxFound] in 'doc'.
 *
 * If a prefix does not exist, the call returns "NonExistentPath". 'elemFound' and
 * 'idxFound' are indeterminate in this case.
 *
 * Definition of a "Viable Path":
 *
 *   A field reference 'p_1.p_2.[...].p_n', where 'p_i' is a field part, is said to be
 *   a viable path in a given document D if the creation of each part 'p_i', 0 <= i < n
 *   in D does not force 'p_i' to change types. In other words, no existing 'p_i' in D
 *   may have a different type, other than the 'p_n'.
 *
 *   'a.b.c' is a viable path in {a: {b: {c: 1}}}
 *   'a.b.c' is a viable path in {a: {b: {c: {d: 1}}}}
 *   'a.b.c' is NOT a viable path in {a: {b: 1}}, because b would have changed types
 *   'a.0.b' is a viable path in {a: [{b: 1}, {c: 1}]}
 *   'a.0.b' is a viable path in {a: {"0": {b: 1}}}
 *   'a.0.b' is NOT a viable path in {a: 1}, because a would have changed types
 *   'a.5.b' is a viable path in in {a: []} (padding would occur)
 */
StatusWith<bool> findLongestPrefix(const FieldRef& prefix,
                                   mutablebson::Element root,
                                   FieldIndex* idxFound,
                                   mutablebson::Element* elemFound);

/**
 * Creates the parts 'prefix[idxRoot]', 'prefix[idxRoot+1]', ..., 'prefix[<numParts>-1]' under
 * 'elemFound' and adds 'newElem' as a child of that path. Returns the first element that was
 * created along the path, if successful, or an error code describing why not, otherwise.
 *
 * createPathAt is designed to work with 'findLongestPrefix' in that it can create the
 * field parts in 'prefix' that are missing from a given document. 'elemFound' points
 * to the element in the doc that is the parent of prefix[idxRoot].
 */
StatusWith<mutablebson::Element> createPathAt(const FieldRef& prefix,
                                              FieldIndex idxRoot,
                                              mutablebson::Element elemFound,
                                              mutablebson::Element newElem);

/**
 * Uses the above methods to set the given value at the specified path in a mutable
 * Document, creating parents of the path if necessary.
 *
 * Returns PathNotViable if the path cannot be created without modifying the type of another
 * element, see above.
 */
Status setElementAtPath(const FieldRef& path, const BSONElement& value, mutablebson::Document* doc);

/**
 * Finds and returns-by-path all the equality matches in a particular MatchExpression.
 *
 * This method is meant to be used with the methods below, which allow efficient use of the
 * equality matches without needing to serialize to a BSONObj.
 *
 * Returns NotSingleValueField if the match expression has equality operators for
 * conflicting paths - equality paths conflict if they are the same or one path is a prefix
 * of the other.
 *
 * Ex:
 *   { a : 1, b : 1 } -> no conflict
 *   { a : 1, a.b : 1 } -> conflict
 *   { _id : { x : 1 }, _id.y : 1 } -> conflict
 *   { a : 1, a : 1 } -> conflict
 */
Status extractEqualityMatches(const MatchExpression& root, EqualityMatches* equalities);

/**
 * Same as the above, but ignores all paths except for paths in a specified set.
 * Equality matches with paths completely distinct from these paths are ignored.
 *
 * For a full equality match, the path of an equality found must not be a suffix of one of
 * the specified path - otherwise it isn't clear how to construct a full value for a field
 * at that path.
 *
 * Generally this is useful for shard keys and _ids which need unambiguous extraction from
 * queries.
 *
 * Ex:
 *   { a : 1 }, full path 'a' -> a $eq 1 extracted
 *   { a : 1 }, full path 'a.b' -> a $eq 1 extracted
 *   { 'a.b' : 1 }, full path 'a' -> NotExactValueField error
 *                                   ('a.b' doesn't specify 'a' fully)
 *   { 'a.b' : 1 }, full path 'a.b' -> 'a.b' $eq 1 extracted
 *   { '_id' : 1 }, full path '_id' -> '_id' $eq 1 extracted
 *   { '_id.x' : 1 }, full path '_id' -> NotExactValueFieldError
 */
Status extractFullEqualityMatches(const MatchExpression& root,
                                  const FieldRefSet& fullPathsToExtract,
                                  EqualityMatches* equalities);

/**
 * Returns the equality match which is at or a parent of the specified path string.  The
 * path string must be a valid dotted path.
 *
 * If a parent equality is found, returns the BSONElement data from that equality (which
 * includes the BSON value), the path of the parent element (prefixStr), and the remainder
 * of the path (which may be empty).
 *
 * EOO() is returned if there were no equalities at any point along the path.
 *
 * Ex:
 *   Given equality matches of:
 *      'a.b' : 1, 'c' : 2
 *   Path 'a' has no equality match parent (EOO)
 *   Path 'c' has an eqmatch parent of 'c' : 2
 *   Path 'c.d' has an eqmatch parent of 'c' : 2
 *   Path 'a.b' has an eqmatch parent of 'a.b' : 1
 *   Path 'a.b.c' has an eqmatch parent of 'a.b' : 1
 *
 */
BSONElement findParentEqualityElement(const EqualityMatches& equalities,
                                      const FieldRef& path,
                                      int* parentPathParts);

/**
 * Adds the BSON values from equality matches into the given document at the equality match
 * paths.
 *
 * Returns PathNotViable similar to setElementAtPath above.  If equality paths do not
 * conflict, as is enforced by extractEqualityMatches, this function should return OK.
 */
Status addEqualitiesToDoc(const EqualityMatches& equalities, mutablebson::Document* doc);

}  // namespace pathsupport

}  // namespace mongo
