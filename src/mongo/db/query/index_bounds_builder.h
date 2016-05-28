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

#include "mongo/db/hasher.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_entry.h"

namespace mongo {

class CollatorInterface;

/**
 * Translates expressions over fields into bounds on an index.
 */
class IndexBoundsBuilder {
public:
    /**
     * Describes various degrees of precision with which predicates can be evaluated based
     * on the index bounds.
     *
     * The integer values of the enum are significant, and are assigned in order of
     * increasing tightness. These values are used when we need to do comparison between two
     * BoundsTightness values. Such comparisons can answer questions such as "Does predicate
     * X have tighter or looser bounds than predicate Y?".
     */
    enum BoundsTightness {
        // Index bounds are inexact, and a fetch is required.
        INEXACT_FETCH = 0,

        // Index bounds are inexact, but no fetch is required
        INEXACT_COVERED = 1,

        // Index bounds are exact.
        EXACT = 2
    };

    /**
     * Populate the provided O.I.L. with one interval goes from MinKey to MaxKey (or vice-versa
     * depending on the index direction).
     */
    static void allValuesForField(const BSONElement& elt, OrderedIntervalList* out);

    /**
     * Turn the MatchExpression in 'expr' into a set of index bounds.  The field that 'expr' is
     * concerned with is indexed according to the keypattern element 'elt' from index 'index'.
     *
     * If 'expr' is elemMatch, the index tag is affixed to a child.
     *
     * The expression must be a predicate over one field.  That is, expr->isLeaf() or
     * expr->isArray() must be true, and expr->isLogical() must be false.
     */
    static void translate(const MatchExpression* expr,
                          const BSONElement& elt,
                          const IndexEntry& index,
                          OrderedIntervalList* oilOut,
                          BoundsTightness* tightnessOut);

    /**
     * Creates bounds for 'expr' (indexed according to 'elt').  Intersects those bounds
     * with the bounds in oilOut, which is an in/out parameter.
     */
    static void translateAndIntersect(const MatchExpression* expr,
                                      const BSONElement& elt,
                                      const IndexEntry& index,
                                      OrderedIntervalList* oilOut,
                                      BoundsTightness* tightnessOut);

    /**
     * Creates bounds for 'expr' (indexed according to 'elt').  Unions those bounds
     * with the bounds in oilOut, which is an in/out parameter.
     */
    static void translateAndUnion(const MatchExpression* expr,
                                  const BSONElement& elt,
                                  const IndexEntry& index,
                                  OrderedIntervalList* oilOut,
                                  BoundsTightness* tightnessOut);

    /**
     * Make a range interval from the provided object.
     * The object must have exactly two fields.  The first field is the start, the second the
     * end.
     * The two inclusive flags indicate whether or not the start/end fields are included in the
     * interval (closed interval if included, open if not).
     */
    static Interval makeRangeInterval(const BSONObj& obj, bool startInclusive, bool endInclusive);

    static Interval makeRangeInterval(const std::string& start,
                                      const std::string& end,
                                      bool startInclusive,
                                      bool endInclusive);

    /**
     * Make a point interval from the provided object.
     * The object must have exactly one field which is the value of the point interval.
     */
    static Interval makePointInterval(const BSONObj& obj);
    static Interval makePointInterval(const std::string& str);
    static Interval makePointInterval(double d);

    /**
     * Wraps 'elt' in a BSONObj with an empty field name and returns the result. If 'elt' is a
     * string, and 'collator' is non-null, the result contains the collator-generated comparison key
     * rather than the original string.
     */
    static BSONObj objFromElement(const BSONElement& elt, const CollatorInterface* collator);

    /**
     * Swap start/end in the provided interval.
     */
    static void reverseInterval(Interval* ival);

    /**
     * Returns a std::string that when used as a matcher, would match a superset of regex. Used to
     * optimize queries in some simple regex cases that start with '^'.
     *
     * Returns "" for complex regular expressions that cannot use tight index bounds.
     */
    static std::string simpleRegex(const char* regex,
                                   const char* flags,
                                   const IndexEntry& index,
                                   BoundsTightness* tightnessOut);

    /**
     * Returns an Interval from minKey to maxKey
     */
    static Interval allValues();

    static void translateRegex(const RegexMatchExpression* rme,
                               const IndexEntry& index,
                               OrderedIntervalList* oil,
                               BoundsTightness* tightnessOut);

    static void translateEquality(const BSONElement& data,
                                  const IndexEntry& index,
                                  bool isHashed,
                                  OrderedIntervalList* oil,
                                  BoundsTightness* tightnessOut);

    static void unionize(OrderedIntervalList* oilOut);
    static void intersectize(const OrderedIntervalList& arg, OrderedIntervalList* oilOut);

    /**
     * Fills out 'bounds' with the bounds for an index scan over all values of the
     * index described by 'keyPattern' in the default forward direction.
     */
    static void allValuesBounds(const BSONObj& keyPattern, IndexBounds* bounds);

    /**
     * Assumes each OIL in 'bounds' is increasing.
     *
     * Aligns OILs (and bounds) according to the 'kp' direction * the scanDir.
     */
    static void alignBounds(IndexBounds* bounds, const BSONObj& kp, int scanDir = 1);

    /**
     * Returns 'true' if the bounds 'bounds' can be represented as one interval between
     * 'startKey' and 'endKey'.  Inclusivity of each bound is set through the relevant
     * (name)KeyInclusive parameter.  Returns 'false' if otherwise.
     */
    static bool isSingleInterval(const IndexBounds& bounds,
                                 BSONObj* startKey,
                                 bool* startKeyInclusive,
                                 BSONObj* endKey,
                                 bool* endKeyInclusive);
};

}  // namespace mongo
