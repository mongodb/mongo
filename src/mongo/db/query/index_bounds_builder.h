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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/hasher.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/interval.h"
#include "mongo/db/query/interval_evaluation_tree.h"

namespace mongo {

class CollatorInterface;

/**
 * Translates expressions over fields into bounds on an index.
 */
class IndexBoundsBuilder {
public:
    static const Interval kUndefinedPointInterval;
    static const Interval kNullPointInterval;
    static const Interval kEmptyArrayPointInterval;

    /**
     * Describes various degrees of precision with which predicates can be evaluated based
     * on the index bounds.
     *
     * Exact vs. inexact is about whether or not you will need to apply a predicate after scanning,
     * and fetch vs. not fetch is whether you need data which is not in the index to answer the
     * query. Often if you have inexact bounds it causes you to need more than the index data, but
     * not always. For example, to correctly match null predicates like {a: {$eq: null}} you may
     * need to fetch the data to distinguish between a null and missing 'a' value (the index stores
     * both with a literal null), so would need INEXACT_FETCH bounds. And as an INEXACT_COVERED
     * example you could think of something like $mod where you can apply the predicate to the data
     * directly in the index, but $mod won't generate tight bounds so you still need to apply the
     * predicate.
     *
     * The integer values of the enum are significant, and are assigned in order of
     * increasing tightness. These values are used when we need to do comparison between two
     * BoundsTightness values. Such comparisons can answer questions such as "Does predicate
     * X have tighter or looser bounds than predicate Y?".
     *
     * These enum values are ordered from loosest to tightest.
     */
    enum BoundsTightness {
        // Index bounds are inexact, and a fetch is required.
        INEXACT_FETCH = 0,

        // Index bounds are inexact, and a fetch may be required depending on the projection.
        // For example, a count $in query on null + a regex can be covered, but a find query with
        // the same filter and no projection cannot.
        INEXACT_MAYBE_COVERED = 1,

        // Index bounds are exact, but a fetch may be required depending on the projection.
        // For example, a find query on null may be covered, depending on which fields we project
        // out.
        EXACT_MAYBE_COVERED = 2,

        // Index bounds are inexact, but no fetch is required.
        INEXACT_COVERED = 3,

        // Index bounds are exact.
        EXACT = 4
    };

    /**
     * Populate the provided O.I.L. with one interval goes from MinKey to MaxKey (or vice-versa
     * depending on the index direction).
     */
    static void allValuesForField(const BSONElement& elt, OrderedIntervalList* out);

    /**
     * Returns true if 'expr' can correctly be assigned as an INEXACT_COVERED predicate to an index
     * scan over 'index'.
     *
     * The result of this function is not meaningful when the predicate applies to special fields
     * such as "hashed", "2d", or "2dsphere". That is, the caller is responsible for ensuring that
     * 'expr' is a candidate for covered matching over a regular ascending/descending field of the
     * index.
     */
    static bool canUseCoveredMatching(const MatchExpression* expr, const IndexEntry& index);

    /**
     * Turn the MatchExpression in 'expr' into a set of index bounds.  The field that 'expr' is
     * concerned with is indexed according to the keypattern element 'elt' from index 'index'.
     *
     * If 'expr' is elemMatch, the index tag is affixed to a child.
     *
     * The expression must be a predicate over one field.  That is, expression category must be
     * kLeaf or kArrayMatching.
     *
     * If 'ietBuilder' is not null the given `expr` is turned into a Interval Evaluation Tree which
     * might be used to restore index bounds from a cached plan.
     */
    static void translate(const MatchExpression* expr,
                          const BSONElement& elt,
                          const IndexEntry& index,
                          OrderedIntervalList* oilOut,
                          BoundsTightness* tightnessOut,
                          interval_evaluation_tree::Builder* ietBuilder);

    /**
     * Turn the MatchExpression in 'expr' into a set of index bounds.  The field that 'expr' is
     * concerned with is indexed according to the keypattern element 'elt' from index 'index'. This
     * function is used to evaluate index bounds from cached Interval Evaluation Trees.
     */
    static void translate(const MatchExpression* expr,
                          const BSONElement& elt,
                          const IndexEntry& index,
                          OrderedIntervalList* oilOut);
    /**
     * Creates bounds for 'expr' (indexed according to 'elt').  Intersects those bounds
     * with the bounds in oilOut, which is an in/out parameter.
     *
     * If 'ietBuilder' is not null the given `expr` is turned into a Interval Evaluation Tree which
     * might be used to restore index bounds from a cached plan.
     */
    static void translateAndIntersect(const MatchExpression* expr,
                                      const BSONElement& elt,
                                      const IndexEntry& index,
                                      OrderedIntervalList* oilOut,
                                      BoundsTightness* tightnessOut,
                                      interval_evaluation_tree::Builder* ietBuilder);

    /**
     * Creates bounds for 'expr' (indexed according to 'elt').  Unions those bounds
     * with the bounds in oilOut, which is an in/out parameter.
     *
     * If 'ietBuilder' is not null the given `expr` is turned into a Interval Evaluation Tree which
     * might be used to restore index bounds from a cached plan.
     */
    static void translateAndUnion(const MatchExpression* expr,
                                  const BSONElement& elt,
                                  const IndexEntry& index,
                                  OrderedIntervalList* oilOut,
                                  BoundsTightness* tightnessOut,
                                  interval_evaluation_tree::Builder* ietBuilder);

    /**
     * Make a range interval from the provided object.
     * The object must have exactly two fields.  The first field is the start, the second the
     * end.
     * The BoundInclusion indicates whether or not the start/end fields are included in the
     * interval (closed interval if included, open if not).
     */
    static Interval makeRangeInterval(const BSONObj& obj, BoundInclusion boundInclusion);

    static Interval makeRangeInterval(const std::string& start,
                                      const std::string& end,
                                      BoundInclusion boundInclusion);

    /**
     * Make a point interval from the provided object.
     * The object must have exactly one field which is the value of the point interval.
     */
    static Interval makePointInterval(const BSONObj& obj);
    static Interval makePointInterval(StringData str);
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

    /**
     * Returns an Interval from minKey to maxKey, preserving the specified inclusion.
     */
    static Interval allValuesRespectingInclusion(BoundInclusion bi);

    static void translateRegex(const RegexMatchExpression* rme,
                               const IndexEntry& index,
                               OrderedIntervalList* oil,
                               BoundsTightness* tightnessOut);

    /**
     * Convert the value at 'data' to an Interval. Populate the out-params, 'oil' and 'tightnessOut'
     * accordingly. If 'holder' is specified, use that BSONObj as the '_intervalData' for the
     * construted Interval, otherwise construct a new BSONObj from 'data'.
     */
    static void translateEquality(const BSONElement& data,
                                  const BSONObj* holder,
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
    static void allValuesBounds(const BSONObj& keyPattern,
                                IndexBounds* bounds,
                                bool hasNonSimpleCollation);

    /**
     * Assumes each OIL in 'bounds' is increasing.
     *
     * Aligns OILs (and bounds) according to the 'kp' direction * the scanDir.
     */
    static void alignBounds(IndexBounds* bounds,
                            const BSONObj& kp,
                            bool hasNonSimpleCollation,
                            int scanDir = 1);

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

    /**
     * Returns 'true' if the ordered intervals 'oil' represent a strict null equality predicate.
     * Returns 'false' otherwise.
     */
    static bool isNullInterval(const OrderedIntervalList& oil);

    /**
     * Returns 'true' if the ordered intervals 'oil' represent a strict equality predicate matching
     * null and the empty list. Returns 'false' otherwise.
     */
    static bool isNullAndEmptyArrayInterval(const OrderedIntervalList& oil);

    /**
     * Appends the startKey and endKey of the given "all values" 'interval' (which is either
     * [MinKey, MaxKey] or [MaxKey, MinKey] interval) to the 'startBob' and 'endBob' respectively,
     * handling inclusivity of each bound through the relevant '*KeyInclusive' parameter.
     *
     * If the 'interval' is not an "all values" interval, does nothing.
     *
     * Precondition: startBob and endBob should contain one or more leading intervals which are not
     * "all values" intervals, to make the constructed interval valid.
     *
     * The decision whether to append MinKey or MaxKey value either to startBob or endBob is based
     * on the interval type (min -> max or max -> min), and inclusivity flags.
     *
     * As an example, consider the index {a:1, b:1} and a count for {a: {$gt: 2}}. Our start key
     * isn't inclusive (as it's $gt: 2) and looks like {"":2} so far. Because {a: 2, b: MaxKey}
     * sorts *after* any real-world data pair {a: 2, b: anything}, setting it as the start value
     * ensures that the first index entry we encounter will be the smallest key with a > 2.
     *
     * Same logic applies if the end key is not inclusive. Consider the index {a:1, b:1} and a count
     * for {a: {$lt: 2}}. Our end key isn't inclusive as ($lt: 2) and looks like {"":2} so far.
     * Because {a: 2, b: MinKey} sorts *before* any real-world data pair {a: 2, b: anything},
     * setting it as the end value ensures that the final index entry we encounter will be the last
     * key with a < 2.
     */
    static void appendTrailingAllValuesInterval(const Interval& interval,
                                                bool startKeyInclusive,
                                                bool endKeyInclusive,
                                                BSONObjBuilder* startBob,
                                                BSONObjBuilder* endBob);

private:
    /**
     * Performs the heavy lifting for IndexBoundsBuilder::translate().
     */
    static void _translatePredicate(const MatchExpression* expr,
                                    const BSONElement& elt,
                                    const IndexEntry& index,
                                    OrderedIntervalList* oilOut,
                                    BoundsTightness* tightnessOut,
                                    interval_evaluation_tree::Builder* ietBuilder);

    /**
     * Helper method for merging interval tightness for $in expressions.
     */
    static void _mergeTightness(const BoundsTightness& tightness, BoundsTightness& tightnessOut);
};

}  // namespace mongo
