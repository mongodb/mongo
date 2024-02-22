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


#include "mongo/db/query/index_bounds_builder.h"

#include <cstddef>
#include <s2cellid.h>
#include <s2region.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/geo/geometry_container.h"
#include "mongo/db/geo/shapes.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/s2_common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/query/analyze_regex.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/expression_index.h"
#include "mongo/db/query/expression_index_knobs_gen.h"
#include "mongo/db/query/indexability.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/planner_wildcard_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {

namespace {

namespace wcp = ::mongo::wildcard_planning;

// Helper for checking that an OIL "appears" to be ascending given one interval.
void assertOILIsAscendingLocally(const vector<Interval>& intervals, size_t idx) {
    // Each individual interval being examined should be ascending or none.
    const auto dir = intervals[idx].getDirection();

    // Should be either ascending, or have no direction (be a point/null/empty interval).
    invariant(dir == Interval::Direction::kDirectionAscending ||
              dir == Interval::Direction::kDirectionNone);

    // The previous OIL's end value should be <= the next OIL's start value.
    if (idx > 0) {
        // Pass 'false' to avoid comparing the field names.
        const int res = intervals[idx - 1].end.woCompare(intervals[idx].start, false);
        invariant(res <= 0);
    }
}

// Tightness rules are shared for $lt, $lte, $gt, $gte.
IndexBoundsBuilder::BoundsTightness getInequalityPredicateTightness(const Interval& interval,
                                                                    const BSONElement& dataElt,
                                                                    const IndexEntry& index) {
    if (interval.isNull()) {
        // Any time the bounds are empty, we consider them to be exact.
        return IndexBoundsBuilder::EXACT;
    }

    return Indexability::isExactBoundsGenerating(dataElt) ? IndexBoundsBuilder::EXACT
                                                          : IndexBoundsBuilder::INEXACT_FETCH;
}

const BSONObj kUndefinedElementObj = BSON("" << BSONUndefined);
const BSONObj kNullElementObj = BSON("" << BSONNULL);
const BSONObj kEmptyArrayElementObj = BSON("" << BSONArray());

const Interval kHashedUndefinedInterval = IndexBoundsBuilder::makePointInterval(
    ExpressionMapping::hash(kUndefinedElementObj.firstElement()));
const Interval kHashedNullInterval =
    IndexBoundsBuilder::makePointInterval(ExpressionMapping::hash(kNullElementObj.firstElement()));

Interval makeUndefinedPointInterval(bool isHashed) {
    return isHashed ? kHashedUndefinedInterval : IndexBoundsBuilder::kUndefinedPointInterval;
}
Interval makeNullPointInterval(bool isHashed) {
    return isHashed ? kHashedNullInterval : IndexBoundsBuilder::kNullPointInterval;
}

/**
 * This helper updates the query bounds tightness for the limited set of conditions where we see a
 * null query that can be covered.
 */
void updateTightnessForNullQuery(const PathMatchExpression* matchExpr,
                                 const IndexEntry& index,
                                 IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    if (index.sparse || index.type == IndexType::INDEX_HASHED) {
        // Sparse indexes and hashed indexes require a FETCH stage with a filter for null queries.
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        return;
    }

    if (index.multikey && matchExpr->fieldRef() && matchExpr->fieldRef()->numParts() > 1) {
        // Documents {"a.b": null} and {a: [1,2,3]} will generate null index keys for index {"a.b":
        // 1}. However, a query like {"a.b": {$eq: null}} should not match {a: [1, 2, 3]}. So, we
        // have inexact bounds, because we will need a residual filter after the fetch.
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        return;
    }

    // The query may be fully covered by the index if the projection allows it, since the case above
    // about the empty array can only become an issue if there is an empty array present, which
    // would mark the index as multikey.
    *tightnessOut = IndexBoundsBuilder::EXACT_MAYBE_COVERED;
}

void makeNullEqualityBounds(const PathMatchExpression* matchExpr,
                            const IndexEntry& index,
                            bool isHashed,
                            OrderedIntervalList* oil,
                            IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    updateTightnessForNullQuery(matchExpr, index, tightnessOut);

    oil->intervals.push_back(makeNullPointInterval(isHashed));
}

bool isEqualityOrInNull(MatchExpression* me) {
    // Because of type-bracketing, {$gte: null} and {$lte: null} are equivalent to {$eq: null}.
    if (MatchExpression::EQ == me->matchType() || MatchExpression::GTE == me->matchType() ||
        MatchExpression::LTE == me->matchType()) {
        return static_cast<ComparisonMatchExpression*>(me)->getData().type() == BSONType::jstNULL;
    }

    if (me->matchType() == MatchExpression::MATCH_IN) {
        const InMatchExpression* in = static_cast<const InMatchExpression*>(me);
        return in->hasNull();
    }

    return false;
}

}  // namespace

string IndexBoundsBuilder::simpleRegex(const char* regex,
                                       const char* flags,
                                       const IndexEntry& index,
                                       BoundsTightness* tightnessOut) {
    if (index.collator) {
        // Bounds building for simple regular expressions assumes that the index is in ASCII order,
        // which is not necessarily true for an index with a collator.  Therefore, a regex can never
        // use tight bounds if the index has a non-null collator. In this case, the regex must be
        // applied to the fetched document rather than the index key, so the tightness is
        // INEXACT_FETCH.
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        return "";
    }

    auto [prefixStr, isExact] = analyze_regex::getRegexPrefixMatch(regex, flags);
    *tightnessOut = isExact ? IndexBoundsBuilder::EXACT : IndexBoundsBuilder::INEXACT_COVERED;
    return prefixStr;
}

void IndexBoundsBuilder::allValuesForField(const BSONElement& elt, OrderedIntervalList* out) {
    // ARGH, BSONValue would make this shorter.
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    out->name = elt.fieldName();
    out->intervals.push_back(
        makeRangeInterval(bob.obj(), BoundInclusion::kIncludeBothStartAndEndKeys));
}

Interval IndexBoundsBuilder::allValuesRespectingInclusion(BoundInclusion bi) {
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    return makeRangeInterval(bob.obj(), bi);
}

Interval IndexBoundsBuilder::allValues() {
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    return makeRangeInterval(bob.obj(), BoundInclusion::kIncludeBothStartAndEndKeys);
}

bool IntervalComparison(const Interval& lhs, const Interval& rhs) {
    int wo = lhs.start.woCompare(rhs.start, false);
    if (0 != wo) {
        return wo < 0;
    }

    // The start and end are equal.
    // Strict weak requires irreflexivity which implies that equivalence returns false.
    if (lhs.startInclusive == rhs.startInclusive) {
        return false;
    }

    // Put the bound that's inclusive to the left.
    return lhs.startInclusive;
}

// static
void IndexBoundsBuilder::translateAndIntersect(const MatchExpression* expr,
                                               const BSONElement& elt,
                                               const IndexEntry& index,
                                               OrderedIntervalList* oilOut,
                                               BoundsTightness* tightnessOut,
                                               interval_evaluation_tree::Builder* ietBuilder) {
    OrderedIntervalList arg;
    translate(expr, elt, index, &arg, tightnessOut, ietBuilder);

    // translate outputs arg in sorted order.  intersectize assumes that its arguments are
    // sorted.
    intersectize(arg, oilOut);

    if (ietBuilder != nullptr) {
        ietBuilder->addIntersect();
    }
}

// static
void IndexBoundsBuilder::translateAndUnion(const MatchExpression* expr,
                                           const BSONElement& elt,
                                           const IndexEntry& index,
                                           OrderedIntervalList* oilOut,
                                           BoundsTightness* tightnessOut,
                                           interval_evaluation_tree::Builder* ietBuilder) {
    OrderedIntervalList arg;
    translate(expr, elt, index, &arg, tightnessOut, ietBuilder);

    // Append the new intervals to oilOut.
    oilOut->intervals.insert(oilOut->intervals.end(), arg.intervals.begin(), arg.intervals.end());

    // Union the appended intervals with the existing ones.
    unionize(oilOut);

    if (ietBuilder != nullptr) {
        ietBuilder->addUnion();
    }
}

bool typeMatch(const BSONObj& obj) {
    BSONObjIterator it(obj);
    MONGO_verify(it.more());
    BSONElement first = it.next();
    MONGO_verify(it.more());
    BSONElement second = it.next();
    return first.canonicalType() == second.canonicalType();
}

bool IndexBoundsBuilder::canUseCoveredMatching(const MatchExpression* expr,
                                               const IndexEntry& index) {
    IndexBoundsBuilder::BoundsTightness tightness;
    OrderedIntervalList oil;
    translate(expr, BSONElement{}, index, &oil, &tightness, /* iet::Builder */ nullptr);
    // We have additional tightness values (MAYBE_COVERED), but we cannot generally cover those
    // cases unless we have an appropriate projection.
    return tightness == IndexBoundsBuilder::INEXACT_COVERED ||
        tightness == IndexBoundsBuilder::EXACT;
}

// static
void IndexBoundsBuilder::translate(const MatchExpression* expr,
                                   const BSONElement& elt,
                                   const IndexEntry& index,
                                   OrderedIntervalList* oilOut,
                                   BoundsTightness* tightnessOut,
                                   interval_evaluation_tree::Builder* ietBuilder) {
    // Fill out the bounds and tightness appropriate for the given predicate.
    _translatePredicate(expr, elt, index, oilOut, tightnessOut, ietBuilder);

    // Under certain circumstances, queries on a $** index require that the bounds' tightness be
    // adjusted regardless of the predicate. Having filled out the initial bounds, we apply any
    // necessary changes to the tightness here.
    if (index.type == IndexType::INDEX_WILDCARD) {
        // Check if 'elt' is the wildcard field.
        BSONElement wildcardElt = wcp::getWildcardField(index);

        // Adjust index bounds and tightness only if the index bounds generated are for the wildcard
        // field.
        if (wildcardElt.fieldNameStringData() == elt.fieldNameStringData()) {
            *tightnessOut = wcp::translateWildcardIndexBoundsAndTightness(
                index, *tightnessOut, oilOut, ietBuilder);
        }
    }
}

void IndexBoundsBuilder::translate(const MatchExpression* expr,
                                   const BSONElement& elt,
                                   const IndexEntry& index,
                                   OrderedIntervalList* oilOut) {
    BoundsTightness tightnessOut;
    _translatePredicate(expr, elt, index, oilOut, &tightnessOut, /*ietBuilder*/ nullptr);
}

namespace {
IndexBoundsBuilder::BoundsTightness computeTightnessForTypeSet(const MatcherTypeSet& typeSet,
                                                               const IndexEntry& index) {
    // The Array case will not be handled because a typeSet with Array should not reach this
    // function
    invariant(!typeSet.hasType(BSONType::Array));

    // The String and Object types with collation require an inexact fetch.
    if (index.collator != nullptr &&
        (typeSet.hasType(BSONType::String) || typeSet.hasType(BSONType::Object))) {
        return IndexBoundsBuilder::INEXACT_FETCH;
    }

    // Mark both null and undefined as inexact. Null and undefined must be differentiated, and
    // undefined and [] must be differentiated.
    if (typeSet.hasType(BSONType::jstNULL) || typeSet.hasType(BSONType::Undefined)) {
        return IndexBoundsBuilder::INEXACT_FETCH;
    }

    const auto numberTypesIncluded = static_cast<int>(typeSet.hasType(BSONType::NumberInt)) +
        static_cast<int>(typeSet.hasType(BSONType::NumberLong)) +
        static_cast<int>(typeSet.hasType(BSONType::NumberDecimal)) +
        static_cast<int>(typeSet.hasType(BSONType::NumberDouble));

    // Checks that either all the number types are present or "number" is present in the type set.
    const bool hasAllNumbers = (numberTypesIncluded == 4) || typeSet.allNumbers;
    const bool hasAnyNumbers = numberTypesIncluded > 0;

    if (hasAnyNumbers && !hasAllNumbers) {
        return IndexBoundsBuilder::INEXACT_COVERED;
    }

    // This check is effectively typeSet.hasType(BSONType::String) XOR
    // typeSet.hasType(BSONType::Symbol).
    if ((typeSet.hasType(BSONType::String) != typeSet.hasType(BSONType::Symbol))) {
        return IndexBoundsBuilder::INEXACT_COVERED;
    }

    return IndexBoundsBuilder::EXACT;
}

// Contains all the logic for determining bounds of a LT or LTE query.
void buildBoundsForQueryElementForLT(BSONElement dataElt,
                                     const mongo::CollatorInterface* collator,
                                     BSONObjBuilder* bob) {
    // Use -infinity for one-sided numerical bounds
    if (dataElt.isNumber()) {
        bob->appendNumber("", -std::numeric_limits<double>::infinity());
    } else if (dataElt.type() == BSONType::Array) {
        // For comparison to an array, we do lexicographic comparisons. In a multikey index, the
        // index entries are the array elements themselves. We must therefore look at all types, and
        // all values between MinKey and the first element in the array.
        bob->appendMinKey("");
    } else {
        bob->appendMinForType("", dataElt.type());
    }
    if (dataElt.type() != BSONType::Array) {
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, collator, bob);
        return;
    }

    auto eltArr = dataElt.Array();
    if (eltArr.empty()) {
        // The empty array is the lowest array.
        bob->appendMinForType("", dataElt.type());
    } else {
        // If the type of the element is greater than the type of the array, the bounds have to
        // include that element. Otherwise the array type, and therefore `dataElt` is
        // sufficiently large to include all relevant keys.
        if (canonicalizeBSONType(eltArr[0].type()) > canonicalizeBSONType(BSONType::Array)) {
            CollationIndexKey::collationAwareIndexKeyAppend(eltArr[0], collator, bob);
        } else {
            CollationIndexKey::collationAwareIndexKeyAppend(dataElt, collator, bob);
        }
    }
}

void buildBoundsForQueryElementForGT(BSONElement dataElt,
                                     const mongo::CollatorInterface* collator,
                                     BSONObjBuilder* bob) {
    if (dataElt.type() == BSONType::Array) {
        auto eltArr = dataElt.Array();
        if (eltArr.empty()) {
            // If the array is empty, we need bounds that will match all arrays. Unfortunately,
            // this means that we have to check the entire index, as any array could have a key
            // anywhere in the multikey index.
            bob->appendMinKey("");
        } else {
            // If the type of the element is smaller than the type of the array, the bounds need
            // to extend to that element. Otherwise the array type, and therefore `dataElt` is
            // sufficiently large include all relevant keys.
            if (canonicalizeBSONType(eltArr[0].type()) < canonicalizeBSONType(BSONType::Array)) {
                CollationIndexKey::collationAwareIndexKeyAppend(eltArr[0], collator, bob);
            } else {
                CollationIndexKey::collationAwareIndexKeyAppend(dataElt, collator, bob);
            }
        }
    } else {
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, collator, bob);
    }

    if (dataElt.isNumber()) {
        bob->appendNumber("", std::numeric_limits<double>::infinity());
        // For comparison to an array, we do lexicographic comparisons. In a multikey index, the
        // index entries are the array elements themselves. We must therefore look at all types, and
        // all values between the first element in the array and MaxKey.
    } else if (dataElt.type() == BSONType::Array) {
        bob->appendMaxKey("");
    } else {
        bob->appendMaxForType("", dataElt.type());
    }
}

}  // namespace

const Interval IndexBoundsBuilder::kUndefinedPointInterval =
    IndexBoundsBuilder::makePointInterval(kUndefinedElementObj);
const Interval IndexBoundsBuilder::kNullPointInterval =
    IndexBoundsBuilder::makePointInterval(kNullElementObj);
const Interval IndexBoundsBuilder::kEmptyArrayPointInterval =
    IndexBoundsBuilder::makePointInterval(kEmptyArrayElementObj);

bool detectIfEntireNullIntervalMatchesPredicate(const InMatchExpression* ime,
                                                const IndexEntry& index) {
    if (!ime->hasNull()) {
        // This isn't a null query.
        return false;
    }

    if (index.sparse || (IndexType::INDEX_HASHED == index.type)) {
        // Sparse indexes and hashed indexes still require a FETCH stage with a filter for null
        // queries.
        return false;
    }

    // Given the context of having a null $in query with eligible indexes, we may be able to cover
    // some combinations of intervals that we could not cover individually.
    if (index.multikey) {
        // If the path has multiple components and we have a multikey index, we still need a FETCH
        // in order to defend against cases where we have a multikey index on "a". These documents
        // will generate null index keys: {"a.b": null} and {a: [1,2,3]}. However, a query like
        // {"a.b": {$in: [null, []]}} should not match {a: [1, 2, 3]}.
        // TODO SERVER-71021: it may be possible to cover more cases here.
        if (ime->fieldRef()->numParts() > 1) {
            return false;
        }

        // When the in-list contains an empty array, the index bounds will include a point interval
        // for undefined values. Since a multikey index reuses the same entry for [] and undefined,
        // we will need a fetch to distinguish these two values (there cannot be an undefined value
        // in an in-list, so we are not matching undefined here).
        if (ime->hasEmptyArray()) {
            return false;
        }
    }

    return true;
}

void IndexBoundsBuilder::_mergeTightness(const BoundsTightness& tightness,
                                         BoundsTightness& tightnessOut) {
    // There is a special case where we may have a covered null query (EXACT_MAYBE_COVERED) and a
    // regex with inexact bounds that doesn't need a FETCH (INEXACT_COVERED). In this case, we want
    // to update the tightness to INEXACT_MAYBE_COVERED, to indicate that we need to check if the
    // projection allows us to cover the query, but ensure that we will have a filter on the index
    // if it turns out we can.
    if (((tightness == BoundsTightness::EXACT_MAYBE_COVERED) &&
         (tightnessOut == BoundsTightness::INEXACT_COVERED)) ||
        ((tightness == BoundsTightness::INEXACT_COVERED) &&
         (tightnessOut == BoundsTightness::EXACT_MAYBE_COVERED))) {
        tightnessOut = BoundsTightness::INEXACT_MAYBE_COVERED;
    } else if (tightness < tightnessOut) {
        // Otherwise, fallback to picking the new tightness if it is looser than the old tightness.
        tightnessOut = tightness;
    }
}

void IndexBoundsBuilder::_translatePredicate(const MatchExpression* expr,
                                             const BSONElement& elt,
                                             const IndexEntry& index,
                                             OrderedIntervalList* oilOut,
                                             BoundsTightness* tightnessOut,
                                             interval_evaluation_tree::Builder* ietBuilder) {
    // We expect that the OIL we are constructing starts out empty.
    invariant(oilOut->intervals.empty());

    oilOut->name = elt.fieldName();

    bool isHashed = false;
    if (elt.valueStringDataSafe() == "hashed") {
        isHashed = true;
    }

    // We should never be asked to translate an unsupported predicate for a hashed index.
    invariant(!isHashed || QueryPlannerIXSelect::nodeIsSupportedByHashedIndex(expr));

    if (MatchExpression::ELEM_MATCH_VALUE == expr->matchType()) {
        _translatePredicate(expr->getChild(0), elt, index, oilOut, tightnessOut, ietBuilder);

        for (size_t i = 1; i < expr->numChildren(); ++i) {
            OrderedIntervalList next;
            BoundsTightness tightness;
            _translatePredicate(expr->getChild(i), elt, index, &next, &tightness, ietBuilder);
            intersectize(next, oilOut);

            if (ietBuilder != nullptr) {
                ietBuilder->addIntersect();
            }
        }

        // $elemMatch value requires an array.
        // Scalars and directly nested objects are not matched with $elemMatch.
        // We can't tell if a multi-key index key is derived from an array field.
        // Therefore, a fetch is required.
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
    } else if (MatchExpression::NOT == expr->matchType()) {
        // A NOT is indexed by virtue of its child. If we're here then the NOT's child
        // must be a kind of node for which we can index negations. It can't be things like
        // $mod, $regex, or $type.
        MatchExpression* child = expr->getChild(0);

        // If we have a NOT -> EXISTS, we must handle separately.
        if (MatchExpression::EXISTS == child->matchType()) {
            // We should never try to use a sparse index for $exists:false.
            invariant(!index.sparse);
            // {$exists:false} is a point-interval on [null,null] that requires a fetch.
            oilOut->intervals.push_back(makeNullPointInterval(isHashed));
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
            if (ietBuilder != nullptr) {
                ietBuilder->addConst(*oilOut);
            }
            return;
        }

        if (MatchExpression::MATCH_IN == child->matchType()) {
            auto ime = static_cast<const InMatchExpression*>(child);
            if (QueryPlannerIXSelect::canUseIndexForNin(ime)) {
                makeNullEqualityBounds(ime, index, isHashed, oilOut, tightnessOut);
                oilOut->intervals.push_back(IndexBoundsBuilder::kEmptyArrayPointInterval);
                oilOut->complement();
                unionize(oilOut);

                if (ietBuilder != nullptr) {
                    // This is a special type of query of the following shape: {a: {$not: {$in:
                    // [null, []]}}}. We never auto-parameterize such query according to our
                    // encoding rules (due to presence of null and an array elements).
                    ietBuilder->addConst(*oilOut);
                }

                *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
                return;
            }
        }

        _translatePredicate(child, elt, index, oilOut, tightnessOut, ietBuilder);
        oilOut->complement();

        if (ietBuilder != nullptr) {
            ietBuilder->addComplement();
        }

        // Until the index distinguishes between missing values and literal null values, we cannot
        // build exact bounds for equality predicates on the literal value null. However, we _can_
        // build exact bounds for the inverse, for example the query {a: {$ne: null}}.
        if (isEqualityOrInNull(child)) {
            *tightnessOut = IndexBoundsBuilder::EXACT;
        }

        // Generally speaking inverting bounds can only be done for exact bounds. Any looser bounds
        // (like INEXACT_FETCH) would signal that inversion would be mistakenly excluding some
        // values. One exception is for collation, whose index bounds are tracked as INEXACT_FETCH,
        // but only because the index data is different than the user data, not because the range
        // is imprecise.
        tassert(4457011,
                "Cannot invert inexact bounds",
                *tightnessOut == IndexBoundsBuilder::EXACT || index.collator);

        // If the index is multikey on this path, it doesn't matter what the tightness of the child
        // is, we must return INEXACT_FETCH. Consider a multikey index on 'a' with document
        // {a: [1, 2, 3]} and query {a: {$ne: 3}}. If we treated the bounds [MinKey, 3), (3, MaxKey]
        // as exact, then we would erroneously return the document!
        if (index.pathHasMultikeyComponent(elt.fieldNameStringData())) {
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        }
    } else if (MatchExpression::EXISTS == expr->matchType()) {
        oilOut->intervals.push_back(allValues());
        if (ietBuilder != nullptr) {
            ietBuilder->addConst(*oilOut);
        }

        // We only handle the {$exists:true} case, as {$exists:false}
        // will have been translated to {$not:{ $exists:true }}.
        //
        // Documents with a missing value are stored *as if* they were
        // explicitly given the value 'null'.  Given:
        //    X = { b : 1 }
        //    Y = { a : null, b : 1 }
        // X and Y look identical from within a standard index on { a : 1 }.
        // HOWEVER a sparse index on { a : 1 } will treat X and Y differently,
        // storing Y and not storing X.
        //
        // We can safely use an index in the following cases:
        // {a:{ $exists:true }} - normal index helps, but we must still fetch
        // {a:{ $exists:true }} - hashed index helps, but we must still fetch
        // {a:{ $exists:true }} - sparse index is exact
        // {a:{ $exists:false }} - normal index requires a fetch
        // {a:{ $exists:false }} - hashed index requires a fetch
        // {a:{ $exists:false }} - sparse indexes cannot be used at all.
        //
        // Noted in SERVER-12869, in case this ever changes some day.
        if (index.sparse) {
            // A sparse, compound index on { a:1, b:1 } will include entries
            // for all of the following documents:
            //    { a:1 }, { b:1 }, { a:1, b:1 }
            // So we must use INEXACT bounds in this case.
            if (1 < index.keyPattern.nFields()) {
                *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
            } else {
                *tightnessOut = IndexBoundsBuilder::EXACT;
            }
        } else {
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        }
    } else if (ComparisonMatchExpressionBase::isEquality(expr->matchType())) {
        const auto* node = static_cast<const ComparisonMatchExpressionBase*>(expr);
        // There is no need to sort intervals or merge overlapping intervals here since the output
        // is from one element.

        translateEquality(node, node->getData(), nullptr, index, isHashed, oilOut, tightnessOut);
        if (ietBuilder != nullptr) {
            switch (expr->matchType()) {
                case MatchExpression::EQ:
                    ietBuilder->addEval(*expr, *oilOut);
                    break;
                // Adding const node here since we do not auto-parameterise comparisons expressed
                // using $expr.
                case MatchExpression::INTERNAL_EXPR_EQ:
                    ietBuilder->addConst(*oilOut);
                    break;
                default:
                    tasserted(6334920,
                              str::stream() << "unexpected MatchType " << expr->matchType());
            }
        }
    } else if (MatchExpression::LT == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, expr, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addEval(*expr, *oilOut);
            }
        });

        const LTMatchExpression* node = static_cast<const LTMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is < MaxKey, except for MaxKey. However the bounds need to be inclusive to
        // find the array [MaxKey] which is smaller for a comparison but equal in a multikey index.
        if (MaxKey == dataElt.type()) {
            oilOut->intervals.push_back(allValuesRespectingInclusion(
                IndexBounds::makeBoundInclusionFromBoundBools(true, index.multikey)));
            *tightnessOut = index.collator || index.multikey ? IndexBoundsBuilder::INEXACT_FETCH
                                                             : IndexBoundsBuilder::EXACT;
            return;
        }

        // Nothing is < NaN.
        if (std::isnan(dataElt.numberDouble())) {
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        BSONObjBuilder bob;
        buildBoundsForQueryElementForLT(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.done().getOwned();
        MONGO_verify(dataObj.isOwned());
        bool inclusiveBounds = dataElt.type() == BSONType::Array;
        Interval interval =
            makeRangeInterval(dataObj,
                              IndexBounds::makeBoundInclusionFromBoundBools(
                                  typeMatch(dataObj) || inclusiveBounds, inclusiveBounds));

        // If the operand to LT is equal to the lower bound X, the interval [X, X) is invalid
        // and should not be added to the bounds.
        if (!interval.isNull()) {
            oilOut->intervals.push_back(interval);
        }

        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::INTERNAL_EXPR_LT == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addConst(*oilOut);
            }
        });

        const auto* node = static_cast<const InternalExprLTMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Unlike the regular $lt match expression, $_internalExprLt does not make special checks
        // for when dataElt is MaxKey or NaN because it doesn't do type bracketing for any operand.
        // Another difference is that $internalExprLt predicates on multikey paths will not use an
        // index.
        tassert(3994304,
                "$expr comparison predicates on multikey paths cannot use an index",
                !index.pathHasMultikeyComponent(elt.fieldNameStringData()));

        BSONObjBuilder bob;
        bob.appendMinKey("");
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.obj();

        // Generally all intervals for $_internalExprLt will exclude the end key, however because
        // null and missing are conflated in the index but treated as distinct values for
        // expressions (with missing ordered as less than null), when dataElt is null we must build
        // index bounds [MinKey, null] to include missing values and filter out the literal null
        // values with an INEXACT_FETCH.
        Interval interval = makeRangeInterval(
            dataObj, IndexBounds::makeBoundInclusionFromBoundBools(true, dataElt.isNull()));

        // If the operand to $_internalExprLt is equal to the lower bound X, the interval [X, X) is
        // invalid and should not be added to the bounds. Because $_internalExprLt doesn't perform
        // type bracketing, here we need to avoid adding the interval [MinKey, MinKey).
        if (!interval.isNull()) {
            oilOut->intervals.push_back(interval);
        }
        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::LTE == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, expr, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addEval(*expr, *oilOut);
            }
        });

        const LTEMatchExpression* node = static_cast<const LTEMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is <= MaxKey.
        if (MaxKey == dataElt.type()) {
            oilOut->intervals.push_back(allValues());
            *tightnessOut =
                index.collator ? IndexBoundsBuilder::INEXACT_FETCH : IndexBoundsBuilder::EXACT;
            return;
        }

        // Only NaN is <= NaN.
        if (std::isnan(dataElt.numberDouble())) {
            double nan = dataElt.numberDouble();
            oilOut->intervals.push_back(makePointInterval(nan));
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        if (BSONType::jstNULL == dataElt.type()) {
            // Because of type-bracketing, $lte null is equivalent to $eq null.
            makeNullEqualityBounds(node, index, isHashed, oilOut, tightnessOut);
            return;
        }

        BSONObjBuilder bob;
        buildBoundsForQueryElementForLT(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.done().getOwned();
        MONGO_verify(dataObj.isOwned());

        bool inclusiveBounds = dataElt.type() == BSONType::Array || typeMatch(dataObj);
        const Interval interval = makeRangeInterval(
            dataObj, IndexBounds::makeBoundInclusionFromBoundBools(inclusiveBounds, true));
        oilOut->intervals.push_back(interval);

        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::INTERNAL_EXPR_LTE == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addConst(*oilOut);
            }
        });

        const auto* node = static_cast<const InternalExprLTEMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Unlike the regular $lte match expression, $_internalExprLte does not make special checks
        // for when dataElt is MaxKey or NaN because it doesn't do type bracketing for any operand.
        // Another difference is that $internalExprLte predicates on multikey paths will not use an
        // index.
        tassert(3994305,
                "$expr comparison predicates on multikey paths cannot use an index",
                !index.pathHasMultikeyComponent(elt.fieldNameStringData()));

        BSONObjBuilder bob;
        bob.appendMinKey("");
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.obj();

        Interval interval = makeRangeInterval(dataObj, BoundInclusion::kIncludeBothStartAndEndKeys);
        oilOut->intervals.push_back(interval);

        // Expressions treat null and missing as distinct values, with missing ordered as less than
        // null. Thus for $_internalExprLte when dataElt is null we can treat the bounds as EXACT,
        // since both null and missing values should be included.
        if (dataElt.isNull()) {
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::GT == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, expr, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addEval(*expr, *oilOut);
            }
        });

        const GTMatchExpression* node = static_cast<const GTMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is > MinKey, except MinKey. However the bounds need to be inclusive to find
        // the array [MinKey], which is larger for a comparison but equal in a multikey index.
        if (MinKey == dataElt.type()) {
            oilOut->intervals.push_back(allValuesRespectingInclusion(
                IndexBounds::makeBoundInclusionFromBoundBools(index.multikey, true)));
            *tightnessOut = index.collator || index.multikey ? IndexBoundsBuilder::INEXACT_FETCH
                                                             : IndexBoundsBuilder::EXACT;
            return;
        }

        // Nothing is > NaN.
        if (std::isnan(dataElt.numberDouble())) {
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        BSONObjBuilder bob;
        buildBoundsForQueryElementForGT(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.done().getOwned();
        MONGO_verify(dataObj.isOwned());
        bool inclusiveBounds = dataElt.type() == BSONType::Array;
        Interval interval =
            makeRangeInterval(dataObj,
                              IndexBounds::makeBoundInclusionFromBoundBools(
                                  inclusiveBounds, inclusiveBounds || typeMatch(dataObj)));

        // If the operand to GT is equal to the upper bound X, the interval (X, X] is invalid
        // and should not be added to the bounds.
        if (!interval.isNull()) {
            oilOut->intervals.push_back(interval);
        }
        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::INTERNAL_EXPR_GT == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addConst(*oilOut);
            }
        });

        const auto* node = static_cast<const InternalExprGTMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Unlike the regular $gt match expression, $_internalExprGt does not make special checks
        // for when dataElt is MinKey or NaN because it doesn't do type bracketing for any operand.
        // Another difference is that $internalExprGt predicates on multikey paths will not use an
        // index.
        tassert(3994302,
                "$expr comparison predicates on multikey paths cannot use an index",
                !index.pathHasMultikeyComponent(elt.fieldNameStringData()));

        BSONObjBuilder bob;
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        bob.appendMaxKey("");
        BSONObj dataObj = bob.obj();

        Interval interval = makeRangeInterval(dataObj, BoundInclusion::kIncludeEndKeyOnly);

        // If the operand to $_internalExprGt is equal to the upper bound X, the interval (X, X] is
        // invalid and should not be added to the bounds. Because $_internalExprGt doesn't perform
        // type bracketing, here we need to avoid adding the interval (MaxKey, MaxKey].
        if (!interval.isNull()) {
            oilOut->intervals.push_back(interval);
        }

        // Expressions treat null and missing as distinct values, with missing ordered as less than
        // null. Thus for $_internalExprGt when dataElt is null we can treat the bounds as EXACT,
        // since both null and missing values should be excluded.
        if (dataElt.isNull()) {
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::GTE == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, expr, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addEval(*expr, *oilOut);
            }
        });

        const GTEMatchExpression* node = static_cast<const GTEMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Everything is >= MinKey.
        if (MinKey == dataElt.type()) {
            oilOut->intervals.push_back(allValues());
            *tightnessOut =
                index.collator ? IndexBoundsBuilder::INEXACT_FETCH : IndexBoundsBuilder::EXACT;
            return;
        }

        // Only NaN is >= NaN.
        if (std::isnan(dataElt.numberDouble())) {
            double nan = dataElt.numberDouble();
            oilOut->intervals.push_back(makePointInterval(nan));
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        if (BSONType::jstNULL == dataElt.type()) {
            // Because of type-bracketing, $gte null is equivalent to $eq null.
            makeNullEqualityBounds(node, index, isHashed, oilOut, tightnessOut);
            return;
        }
        BSONObjBuilder bob;
        buildBoundsForQueryElementForGT(dataElt, index.collator, &bob);
        BSONObj dataObj = bob.done().getOwned();
        MONGO_verify(dataObj.isOwned());
        bool inclusiveBounds = dataElt.type() == BSONType::Array || typeMatch(dataObj);
        const Interval interval = makeRangeInterval(
            dataObj, IndexBounds::makeBoundInclusionFromBoundBools(true, inclusiveBounds));
        oilOut->intervals.push_back(interval);

        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::INTERNAL_EXPR_GTE == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addConst(*oilOut);
            }
        });

        const auto* node = static_cast<const InternalExprGTEMatchExpression*>(expr);
        BSONElement dataElt = node->getData();

        // Unlike the regular $gte match expression, $_internalExprGte does not make special checks
        // for when dataElt is MinKey or NaN because it doesn't do type bracketing for any operand.
        // Another difference is that $internalExprGte predicates on multikey paths will not use an
        // index.
        tassert(3994303,
                "$expr comparison predicates on multikey paths cannot use an index",
                !index.pathHasMultikeyComponent(elt.fieldNameStringData()));

        BSONObjBuilder bob;
        CollationIndexKey::collationAwareIndexKeyAppend(dataElt, index.collator, &bob);
        bob.appendMaxKey("");
        BSONObj dataObj = bob.obj();

        Interval interval = makeRangeInterval(dataObj, BoundInclusion::kIncludeBothStartAndEndKeys);
        oilOut->intervals.push_back(interval);
        *tightnessOut = getInequalityPredicateTightness(interval, dataElt, index);
    } else if (MatchExpression::INTERNAL_EQ_HASHED_KEY == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addConst(*oilOut);
            }
        });

        tassert(7281403, "Expected a hashed index", index.type == INDEX_HASHED);

        const auto* node = static_cast<const InternalEqHashedKey*>(expr);
        BSONObj dataObj = BSON("" << node->getData());

        Interval interval = makePointInterval(dataObj);
        oilOut->intervals.push_back(interval);

        // Technically this could be EXACT_FETCH, if such a thing existed. But we don't need to
        // optimize this that much.
        *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
    } else if (MatchExpression::REGEX == expr->matchType()) {
        const RegexMatchExpression* rme = static_cast<const RegexMatchExpression*>(expr);
        translateRegex(rme, index, oilOut, tightnessOut);

        if (ietBuilder != nullptr) {
            ietBuilder->addEval(*expr, *oilOut);
        }
    } else if (MatchExpression::MOD == expr->matchType()) {
        BSONObjBuilder bob;
        bob.appendMinForType("", NumberDouble);
        bob.appendMaxForType("", NumberDouble);
        BSONObj dataObj = bob.obj();
        MONGO_verify(dataObj.isOwned());
        oilOut->intervals.push_back(
            makeRangeInterval(dataObj, BoundInclusion::kIncludeBothStartAndEndKeys));
        *tightnessOut = IndexBoundsBuilder::INEXACT_COVERED;

        if (ietBuilder != nullptr) {
            ietBuilder->addConst(*oilOut);
        }
    } else if (MatchExpression::TYPE_OPERATOR == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, expr, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addEval(*expr, *oilOut);
            }
        });

        const TypeMatchExpression* tme = static_cast<const TypeMatchExpression*>(expr);

        if (tme->typeSet().hasType(BSONType::Array)) {
            // We have $type:"array". Since arrays are indexed by creating a key for each element,
            // we have to fetch all indexed documents and check whether the full document contains
            // an array.
            oilOut->intervals.push_back(allValues());
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
            return;
        }

        // If we are matching all numbers, we just use the bounds for NumberInt, as these bounds
        // also include all NumberDouble and NumberLong values.
        if (tme->typeSet().allNumbers) {
            BSONObjBuilder bob;
            bob.appendMinForType("", BSONType::NumberInt);
            bob.appendMaxForType("", BSONType::NumberInt);
            oilOut->intervals.push_back(
                makeRangeInterval(bob.obj(), BoundInclusion::kIncludeBothStartAndEndKeys));
        }

        for (auto type : tme->typeSet().bsonTypes) {
            BSONObjBuilder bob;
            bob.appendMinForType("", type);
            bob.appendMaxForType("", type);

            // Types with variable width use the smallest value of the next type as their upper
            // bound, so the upper bound needs to be excluded.
            auto boundInclusionRule = BoundInclusion::kIncludeBothStartAndEndKeys;
            if (isVariableWidthType(type)) {
                boundInclusionRule = BoundInclusion::kIncludeStartKeyOnly;
            }
            oilOut->intervals.push_back(makeRangeInterval(bob.obj(), boundInclusionRule));
        }

        *tightnessOut = computeTightnessForTypeSet(tme->typeSet(), index);

        // Sort the intervals, and merge redundant ones.
        unionize(oilOut);
    } else if (MatchExpression::MATCH_IN == expr->matchType()) {
        ON_BLOCK_EXIT([ietBuilder, expr, oilOut] {
            if (ietBuilder != nullptr) {
                ietBuilder->addEval(*expr, *oilOut);
            }
        });

        const InMatchExpression* ime = static_cast<const InMatchExpression*>(expr);
        *tightnessOut = IndexBoundsBuilder::EXACT;

        // Create our various intervals.

        IndexBoundsBuilder::BoundsTightness tightness;
        // We check if the $in predicate satisfies conditions to be a covered null predicate on the
        // basis of indexes, null intervals, and array intervals.
        const bool entireNullIntervalMatchesPredicate =
            detectIfEntireNullIntervalMatchesPredicate(ime, index);

        // Ensure that we own the BSON buffer containing the $in array. This will allow us to create
        // Interval objects which point to this BSON without having to make copies just to strip out
        // the field name as is usually done in IndexBoundsBuilder::translateEquality.
        // If the InList has already been prepared, as will be the case if we reach this codepath
        // from IntervalEvalutionTree evaluation (SBE plan was retrieved from the cache and
        // parameters are being bound), then we cannot make a copy of the BSON buffer. In this case,
        // we clone() the InList, which will share the underlying BSON but the copy will not be in
        // the prepared state, allowing us to make a copy of the BSON.
        auto inList = ime->getInList();
        if (!inList->isBSONOwned()) {
            if (inList->isPrepared()) {
                inList = inList->clone();
            }
            inList->makeBSONOwned();
        }

        for (auto&& equality : ime->getEqualities()) {
            // First, we generate the bounds the same way that we would do for an individual
            // equality. This will set tightness to the value it should be if this equality is being
            // considered in isolation.
            IndexBoundsBuilder::translateEquality(
                ime, equality, &inList->getOwnedBSONStorage(), index, isHashed, oilOut, &tightness);

            if (entireNullIntervalMatchesPredicate && BSONType::jstNULL == equality.type()) {
                // We may have a covered null query. In this case, we update null interval
                // tightness to EXACT_MAYBE_COVERED, as individually it would have a tightness of
                // INEXACT_FETCH. However, we already know we will be able to cover this interval
                // if we have appropriate projections. Note that any other intervals that
                // cannot be covered may still require the query to use a FETCH.
                tightness = IndexBoundsBuilder::EXACT_MAYBE_COVERED;
            }
            IndexBoundsBuilder::_mergeTightness(tightness, *tightnessOut);
        }

        for (auto&& regex : ime->getRegexes()) {
            translateRegex(regex.get(), index, oilOut, &tightness);
            IndexBoundsBuilder::_mergeTightness(tightness, *tightnessOut);
        }

        // Equalities are already sorted and deduped so unionize is unneccesary if no regexes
        // are present. Hashed indexes may also cause the bounds to be out-of-order.
        // Arrays and nulls introduce multiple elements that necessitate a sort and deduping.
        if (ime->hasNonScalarOrNonEmptyValues() || index.type == IndexType::INDEX_HASHED) {
            unionize(oilOut);
        }

    } else if (MatchExpression::GEO == expr->matchType()) {
        const GeoMatchExpression* gme = static_cast<const GeoMatchExpression*>(expr);
        if ("2dsphere" == elt.valueStringDataSafe()) {
            MONGO_verify(gme->getGeoExpression().getGeometry().hasS2Region());
            const S2Region& region = gme->getGeoExpression().getGeometry().getS2Region();
            S2IndexingParams indexParams;
            ExpressionParams::initialize2dsphereParams(index.infoObj, index.collator, &indexParams);
            ExpressionMapping::cover2dsphere(region, indexParams, oilOut);
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        } else if ("2d" == elt.valueStringDataSafe()) {
            MONGO_verify(gme->getGeoExpression().getGeometry().hasR2Region());
            const R2Region& region = gme->getGeoExpression().getGeometry().getR2Region();

            ExpressionMapping::cover2d(
                region, index.infoObj, gInternalGeoPredicateQuery2DMaxCoveringCells.load(), oilOut);

            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        } else {
            LOGV2_WARNING(20934,
                          "Planner error trying to build geo bounds for an index element",
                          "element"_attr = elt.toString());
            MONGO_verify(0);
        }
    } else if (MatchExpression::INTERNAL_BUCKET_GEO_WITHIN == expr->matchType()) {
        const InternalBucketGeoWithinMatchExpression* ibgwme =
            static_cast<const InternalBucketGeoWithinMatchExpression*>(expr);
        if ("2dsphere_bucket"_sd == elt.valueStringDataSafe()) {
            tassert(5837101,
                    "A geo query on a sphere must have an S2 region",
                    ibgwme->getGeoContainer().hasS2Region());
            const S2Region& region = ibgwme->getGeoContainer().getS2Region();
            S2IndexingParams indexParams;
            ExpressionParams::initialize2dsphereParams(index.infoObj, index.collator, &indexParams);
            ExpressionMapping::cover2dsphere(region, indexParams, oilOut);
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        } else {
            LOGV2_WARNING(5837102,
                          "Planner error trying to build bucketed geo bounds for an index element",
                          "element"_attr = elt.toString());
            MONGO_UNREACHABLE_TASSERT(5837103);
        }
    } else {
        LOGV2_WARNING(20935,
                      "Planner error while trying to build bounds for expression",
                      "expression"_attr = redact(expr->debugString()));
        MONGO_verify(0);
    }
}

// static
Interval IndexBoundsBuilder::makeRangeInterval(const BSONObj& obj, BoundInclusion boundInclusion) {
    Interval ret;
    ret._intervalData = obj;
    ret.startInclusive = IndexBounds::isStartIncludedInBound(boundInclusion);
    ret.endInclusive = IndexBounds::isEndIncludedInBound(boundInclusion);
    BSONObjIterator it(obj);
    MONGO_verify(it.more());
    ret.start = it.next();
    MONGO_verify(it.more());
    ret.end = it.next();
    return ret;
}

// static
void IndexBoundsBuilder::intersectize(const OrderedIntervalList& oilA, OrderedIntervalList* oilB) {
    invariant(oilB);
    invariant(oilA.name == oilB->name);

    size_t oilAIdx = 0;
    const vector<Interval>& oilAIntervals = oilA.intervals;

    size_t oilBIdx = 0;
    vector<Interval>& oilBIntervals = oilB->intervals;

    vector<Interval> result;

    while (oilAIdx < oilAIntervals.size() && oilBIdx < oilBIntervals.size()) {
        if (kDebugBuild) {
            // Ensure that both OILs are ascending.
            assertOILIsAscendingLocally(oilAIntervals, oilAIdx);
            assertOILIsAscendingLocally(oilBIntervals, oilBIdx);
        }

        Interval::IntervalComparison cmp = oilAIntervals[oilAIdx].compare(oilBIntervals[oilBIdx]);
        MONGO_verify(Interval::INTERVAL_UNKNOWN != cmp);

        if (cmp == Interval::INTERVAL_PRECEDES || cmp == Interval::INTERVAL_PRECEDES_COULD_UNION) {
            // oilAIntervals is before oilBIntervals. move oilAIntervals forward.
            ++oilAIdx;
        } else if (cmp == Interval::INTERVAL_SUCCEEDS) {
            // oilBIntervals is before oilAIntervals. move oilBIntervals forward.
            ++oilBIdx;
        } else {
            Interval newInt = oilAIntervals[oilAIdx];
            newInt.intersect(oilBIntervals[oilBIdx], cmp);
            result.push_back(newInt);

            if (Interval::INTERVAL_EQUALS == cmp) {
                ++oilAIdx;
                ++oilBIdx;
            } else if (Interval::INTERVAL_WITHIN == cmp) {
                ++oilAIdx;
            } else if (Interval::INTERVAL_CONTAINS == cmp) {
                ++oilBIdx;
            } else if (Interval::INTERVAL_OVERLAPS_BEFORE == cmp) {
                ++oilAIdx;
            } else if (Interval::INTERVAL_OVERLAPS_AFTER == cmp) {
                ++oilBIdx;
            } else {
                MONGO_UNREACHABLE;
            }
        }
    }

    oilB->intervals.swap(result);
}

// static
void IndexBoundsBuilder::unionize(OrderedIntervalList* oilOut) {
    vector<Interval>& iv = oilOut->intervals;

    // This can happen.
    if (iv.empty()) {
        return;
    }

    // Step 1: sort.
    std::sort(iv.begin(), iv.end(), IntervalComparison);

    // Step 2: Walk through and merge.
    size_t i = 0;
    while (i < iv.size() - 1) {
        // Compare i with i + 1.
        Interval::IntervalComparison cmp = iv[i].compare(iv[i + 1]);

        // This means our sort didn't work.
        MONGO_verify(Interval::INTERVAL_SUCCEEDS != cmp);

        // Intervals are correctly ordered.
        if (Interval::INTERVAL_PRECEDES == cmp) {
            // We can move to the next pair.
            ++i;
        } else if (Interval::INTERVAL_EQUALS == cmp || Interval::INTERVAL_WITHIN == cmp) {
            // Interval 'i' is equal to i+1, or is contained within i+1.
            // Remove interval i and don't move to the next value of 'i'.
            iv.erase(iv.begin() + i);
        } else if (Interval::INTERVAL_CONTAINS == cmp) {
            // Interval 'i' contains i+1, remove i+1 and don't move to the next value of 'i'.
            iv.erase(iv.begin() + i + 1);
        } else if (Interval::INTERVAL_OVERLAPS_BEFORE == cmp ||
                   Interval::INTERVAL_PRECEDES_COULD_UNION == cmp) {
            // We want to merge intervals i and i+1.
            // Interval 'i' starts before interval 'i+1'.
            BSONObjBuilder bob;
            bob.appendAs(iv[i].start, "");
            bob.appendAs(iv[i + 1].end, "");
            BSONObj data = bob.obj();
            bool startInclusive = iv[i].startInclusive;
            bool endInclusive = iv[i + 1].endInclusive;
            iv.erase(iv.begin() + i);
            // iv[i] is now the former iv[i + 1]
            iv[i] = makeRangeInterval(
                data, IndexBounds::makeBoundInclusionFromBoundBools(startInclusive, endInclusive));
            // Don't increment 'i'.
        }
    }
}

// static
Interval IndexBoundsBuilder::makeRangeInterval(const string& start,
                                               const string& end,
                                               BoundInclusion boundInclusion) {
    BSONObjBuilder bob;
    bob.append("", start);
    bob.append("", end);
    return makeRangeInterval(bob.obj(), boundInclusion);
}

// static
Interval IndexBoundsBuilder::makePointInterval(const BSONObj& obj) {
    Interval ret;
    ret._intervalData = obj;
    ret.startInclusive = ret.endInclusive = true;
    ret.start = ret.end = obj.firstElement();
    return ret;
}

// static
Interval IndexBoundsBuilder::makePointInterval(StringData str) {
    BSONObjBuilder bob;
    bob.append("", str);
    return makePointInterval(bob.obj());
}

// static
Interval IndexBoundsBuilder::makePointInterval(double d) {
    BSONObjBuilder bob;
    bob.append("", d);
    return makePointInterval(bob.obj());
}

// static
BSONObj IndexBoundsBuilder::objFromElement(const BSONElement& elt,
                                           const CollatorInterface* collator) {
    BSONObjBuilder bob;
    CollationIndexKey::collationAwareIndexKeyAppend(elt, collator, &bob);
    return bob.obj();
}

// static
void IndexBoundsBuilder::reverseInterval(Interval* ival) {
    BSONElement tmp = ival->start;
    ival->start = ival->end;
    ival->end = tmp;

    bool tmpInc = ival->startInclusive;
    ival->startInclusive = ival->endInclusive;
    ival->endInclusive = tmpInc;
}

// static
void IndexBoundsBuilder::translateRegex(const RegexMatchExpression* rme,
                                        const IndexEntry& index,
                                        OrderedIntervalList* oilOut,
                                        BoundsTightness* tightnessOut) {
    const string start =
        simpleRegex(rme->getString().c_str(), rme->getFlags().c_str(), index, tightnessOut);

    // Note that 'tightnessOut' is set by simpleRegex above.
    if (!start.empty()) {
        string end = start;
        end[end.size() - 1]++;
        oilOut->intervals.push_back(
            makeRangeInterval(start, end, BoundInclusion::kIncludeStartKeyOnly));
    } else {
        BSONObjBuilder bob;
        bob.appendMinForType("", String);
        bob.appendMaxForType("", String);
        BSONObj dataObj = bob.obj();
        MONGO_verify(dataObj.isOwned());
        oilOut->intervals.push_back(
            makeRangeInterval(dataObj, BoundInclusion::kIncludeStartKeyOnly));
    }

    // Regexes are after strings.
    BSONObjBuilder bob;
    bob.appendRegex("", rme->getString(), rme->getFlags());
    oilOut->intervals.push_back(makePointInterval(bob.obj()));
}

// static
void IndexBoundsBuilder::translateEquality(const PathMatchExpression* matchExpr,
                                           const BSONElement& data,
                                           const BSONObj* holder,
                                           const IndexEntry& index,
                                           bool isHashed,
                                           OrderedIntervalList* oil,
                                           BoundsTightness* tightnessOut) {
    if (BSONType::jstNULL == data.type()) {
        // An equality to null query is special. It has different tightness constraints.
        return makeNullEqualityBounds(matchExpr, index, isHashed, oil, tightnessOut);
    }

    if (BSONType::Array != data.type()) {
        // Reuse the BSON from the parse tree if possible to avoid allocating a BSONObj.
        // A hashed index or collation means we have to create a copy to construct the bounds.
        if (!isHashed && index.collator == nullptr && holder != nullptr) {
            oil->intervals.emplace_back(*holder, data, true, data, true);
            *tightnessOut = IndexBoundsBuilder::EXACT;
            return;
        }

        BSONObj dataObj = objFromElement(data, index.collator);
        if (isHashed) {
            dataObj = ExpressionMapping::hash(dataObj.firstElement());
        }

        MONGO_verify(dataObj.isOwned());
        oil->intervals.push_back(makePointInterval(dataObj));

        if (isHashed) {
            *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
        } else {
            *tightnessOut = IndexBoundsBuilder::EXACT;
        }
        return;
    }

    // If we're here, Array == data.type().
    //
    // Using arrays with hashed indices is currently not supported, so we don't have to worry
    // about that case.
    //
    // Arrays are indexed by either:
    //
    // 1. the first element if there is one.  Note that using the first is arbitrary; we could
    // just as well use any array element.). If the query is {a: [1, 2, 3]}, for example, then
    // using the bounds [1, 1] for the multikey index will pick up every document containing the
    // array [1, 2, 3].
    //
    // 2. undefined if the array is empty.
    //
    // Also, arrays are indexed by:
    //
    // 3. the full array if it's inside of another array.  We check for this so that the query
    // {a: [1, 2, 3]} will match documents like {a: [[1, 2, 3], 4, 5]}.

    // Case 3.
    oil->intervals.push_back(makePointInterval(objFromElement(data, index.collator)));

    if (data.Obj().isEmpty()) {
        // Case 2.
        BSONObjBuilder undefinedBob;
        undefinedBob.appendUndefined("");
        oil->intervals.push_back(makePointInterval(undefinedBob.obj()));
    } else {
        // Case 1.
        BSONElement firstEl = data.Obj().firstElement();
        oil->intervals.push_back(makePointInterval(objFromElement(firstEl, index.collator)));
    }

    std::sort(oil->intervals.begin(), oil->intervals.end(), IntervalComparison);

    *tightnessOut = IndexBoundsBuilder::INEXACT_FETCH;
}

// static
void IndexBoundsBuilder::allValuesBounds(const BSONObj& keyPattern,
                                         IndexBounds* bounds,
                                         bool hasNonSimpleCollation) {
    bounds->fields.resize(keyPattern.nFields());

    BSONObjIterator it(keyPattern);
    int field = 0;
    while (it.more()) {
        IndexBoundsBuilder::allValuesForField(it.next(), &bounds->fields[field]);
        ++field;
    }

    alignBounds(bounds, keyPattern, hasNonSimpleCollation);
}

// static
void IndexBoundsBuilder::alignBounds(IndexBounds* bounds,
                                     const BSONObj& kp,
                                     bool hasNonSimpleCollation,
                                     int scanDir) {
    BSONObjIterator it(kp);
    size_t oilIdx = 0;
    while (it.more()) {
        BSONElement elt = it.next();
        // The canonical check as to whether a key pattern element is "ascending" or "descending" is
        // (elt.number() >= 0). This is defined by the Ordering class.
        int direction = (elt.number() >= 0) ? 1 : -1;
        direction *= scanDir;
        if (-1 == direction) {
            bounds->fields[oilIdx].reverse();
        }
        ++oilIdx;
    }

    if constexpr (kDebugBuild) {
        if (!bounds->isValidFor(kp, scanDir)) {
            LOGV2(20933,
                  "Invalid bounds",
                  "bounds"_attr = redact(bounds->toString(hasNonSimpleCollation)),
                  "keyPattern"_attr = redact(kp),
                  "scanDirection"_attr = scanDir);
            MONGO_UNREACHABLE_TASSERT(6349900);
        }
    }
}

void IndexBoundsBuilder::appendTrailingAllValuesInterval(const Interval& interval,
                                                         bool startKeyInclusive,
                                                         bool endKeyInclusive,
                                                         BSONObjBuilder* startBob,
                                                         BSONObjBuilder* endBob) {
    invariant(startBob);
    invariant(endBob);

    // Must be min->max or max->min.
    if (interval.isMinToMax()) {
        // As an example for the logic below, consider the index {a:1, b:1} and a count for
        // {a: {$gt: 2}}.  Our start key isn't inclusive (as it's $gt: 2) and looks like
        // {"":2} so far.  If we move to the key greater than {"":2, "": MaxKey} we will get
        // the first value of 'a' that is greater than 2.
        if (!startKeyInclusive) {
            startBob->appendMaxKey("");
        } else {
            // In this case, consider the index {a:1, b:1} and a count for {a:{$gte: 2}}.
            // We want to look at all values where a is 2, so our start key is {"":2,
            // "":MinKey}.
            startBob->appendMinKey("");
        }

        // Same deal as above.  Consider the index {a:1, b:1} and a count for {a: {$lt: 2}}.
        // Our end key isn't inclusive as ($lt: 2) and looks like {"":2} so far.  We can't
        // look at any values where a is 2 so we have to stop at {"":2, "": MinKey} as
        // that's the smallest key where a is still 2.
        if (!endKeyInclusive) {
            endBob->appendMinKey("");
        } else {
            endBob->appendMaxKey("");
        }
    } else if (interval.isMaxToMin()) {
        // The reasoning here is the same as above but with the directions reversed.
        if (!startKeyInclusive) {
            startBob->appendMinKey("");
        } else {
            startBob->appendMaxKey("");
        }

        if (!endKeyInclusive) {
            endBob->appendMaxKey("");
        } else {
            endBob->appendMinKey("");
        }
    }
}

// static
bool IndexBoundsBuilder::isSingleInterval(const IndexBounds& bounds,
                                          BSONObj* startKey,
                                          bool* startKeyInclusive,
                                          BSONObj* endKey,
                                          bool* endKeyInclusive) {
    // We build our start/end keys as we go.
    BSONObjBuilder startBob;
    BSONObjBuilder endBob;

    // The start and end keys are inclusive unless we have a non-point interval, in which case
    // we take the inclusivity from there.
    *startKeyInclusive = true;
    *endKeyInclusive = true;

    size_t fieldNo = 0;

    // First, we skip over point intervals.
    for (; fieldNo < bounds.fields.size(); ++fieldNo) {
        const OrderedIntervalList& oil = bounds.fields[fieldNo];
        // A point interval requires just one interval...
        if (1 != oil.intervals.size()) {
            break;
        }
        if (!oil.intervals[0].isPoint()) {
            break;
        }
        // Since it's a point, start == end.
        startBob.append(oil.intervals[0].start);
        endBob.append(oil.intervals[0].end);
    }

    if (fieldNo >= bounds.fields.size()) {
        // All our intervals are points.  We count for all values of one field.
        *startKey = startBob.obj();
        *endKey = endBob.obj();
        return true;
    }

    // After point intervals we can have exactly one non-point interval.
    const OrderedIntervalList& nonPoint = bounds.fields[fieldNo];
    if (1 != nonPoint.intervals.size()) {
        return false;
    }

    // Add the non-point interval to our builder and set the inclusivity from it.
    startBob.append(nonPoint.intervals[0].start);
    *startKeyInclusive = nonPoint.intervals[0].startInclusive;
    endBob.append(nonPoint.intervals[0].end);
    *endKeyInclusive = nonPoint.intervals[0].endInclusive;

    ++fieldNo;

    // And after the non-point interval we can have any number of "all values" intervals.
    for (; fieldNo < bounds.fields.size(); ++fieldNo) {
        const OrderedIntervalList& oil = bounds.fields[fieldNo];
        // "All Values" is just one point.
        if (1 != oil.intervals.size()) {
            break;
        }

        if (oil.intervals[0].isMinToMax() || oil.intervals[0].isMaxToMin()) {
            IndexBoundsBuilder::appendTrailingAllValuesInterval(
                oil.intervals[0], *startKeyInclusive, *endKeyInclusive, &startBob, &endBob);
        } else {
            // No dice.
            break;
        }
    }

    if (fieldNo >= bounds.fields.size()) {
        *startKey = startBob.obj();
        *endKey = endBob.obj();
        return true;
    } else {
        return false;
    }
}

}  // namespace mongo
