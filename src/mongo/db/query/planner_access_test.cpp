// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/planner_access.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/record_id_range.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

BSONObj serializeMatcher(Matcher* matcher) {
    return matcher->getMatchExpression()->serialize();
}

TEST(PlannerAccessTest, PrepareForAccessPlanningSortsEqualNodesByTheirChildren) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    Matcher matcher{fromjson("{$or: [{x: 1, b: 1}, {y: 1, a: 1}]}"), expCtx};
    // Before sorting for access planning, the order of the tree should be as specified in the
    // original input BSON.
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{x: {$eq: 1}}, {b: {$eq: 1}}]},"
                               "{$and: [{y: {$eq: 1}}, {a: {$eq: 1}}]}]}"));

    // The two $or nodes in the match expression tree are only differentiated by their children.
    // After sorting in the order expected by the access planner, the $or node with the "a" child
    // should come first.
    prepareForAccessPlanning(matcher.getMatchExpression());
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{a: {$eq: 1}}, {y: {$eq: 1}}]},"
                               "{$and: [{b: {$eq: 1}}, {x: {$eq: 1}}]}]}"));
}

TEST(PlannerAccessTest, PrepareForAccessPlanningSortsByNumberOfChildren) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    Matcher matcher{fromjson("{$or: [{a: 1, c: 1, b: 1}, {b: 1, a: 1}]}"), expCtx};
    // Before sorting for access planning, the order of the tree should be as specified in the
    // original input BSON.
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{a: {$eq: 1}}, {c: {$eq: 1}}, {b: {$eq: 1}}]},"
                               "{$and: [{b: {$eq: 1}}, {a: {$eq: 1}}]}]}"));

    // The two $or nodes in the match expression tree are only differentiated by the number of
    // children they have. Both have {a: {$eq: 1}} and {b: {$eq: 1}} as their first children, but
    // one has an additional child. The node with fewer children should be sorted first.
    prepareForAccessPlanning(matcher.getMatchExpression());
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher),
                      fromjson("{$or: [{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]},"
                               "{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}, {c: {$eq: 1}}]}]}"));
}

TEST(PlannerAccessTest, PrepareForAccessPlanningSortIsStable) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    Matcher matcher{fromjson("{$or: [{a: 2, b: 2}, {a: 1, b: 1}]}"), expCtx};
    BSONObj expectedSerialization = fromjson(
        "{$or: [{$and: [{a: {$eq: 2}}, {b: {$eq: 2}}]},"
        "{$and: [{a: {$eq: 1}}, {b: {$eq: 1}}]}]}");
    // Before sorting for access planning, the order of the tree should be as specified in the
    // original input BSON.
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher), expectedSerialization);

    // Sorting for access planning should not change the order of the match expression tree. The two
    // $or branches are considered equal, since they only differ by their constants.
    prepareForAccessPlanning(matcher.getMatchExpression());
    ASSERT_BSONOBJ_EQ(serializeMatcher(&matcher), expectedSerialization);
}

/**
 * Helper for declaring expected outcomes of simplifying a filter down to
 * {min,max}RecordId.
 */
struct Bound {
    Bound() = default;
    Bound(BSONObj v, bool inclusive = false) : _value(v), _inclusive(inclusive) {}
    template <class Value>
    Bound(Value v, bool inclusive = false) : _value(BSON("" << v)), _inclusive(inclusive) {}
    boost::optional<BSONObj> _value;
    bool _inclusive = false;
};

/**
 * Verifies that for a clustered collection scan on the filter given as json in 'input':
 *  1. handleRIDRangeScan produces exactly the ranges in 'expectedRanges' (a list of
 *     {minBound, maxBound} pairs in sorted order).
 *  2. simplifyFilter reduces the filter to the json given in 'expected'.
 */
void testSimplify(const char* input,
                  const char* expected,
                  std::vector<std::pair<Bound, Bound>> expectedRanges,
                  const CollatorInterface* collator = nullptr) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    BSONObj query = fromjson(input);
    auto expr = Matcher(query, expCtx).getMatchExpression()->clone();

    // First pass: compute the scan range list from the filter.
    RecordIdRangeList rangeList;
    (void)QueryPlannerAccess::handleRIDRangeScan(expr.get(),
                                                 collator /* queryCollator */,
                                                 nullptr /* ccCollator */,
                                                 "_id" /* clustered field name */,
                                                 rangeList);

    ASSERT_EQ(rangeList.getRanges().size(), expectedRanges.size())
        << "Unexpected number of ranges in range list";

    // Type-boundary values (appendMinForType / appendMaxForType) are stored in the range
    // without applying any collation, so makeBound must also skip collation here.
    auto makeBound = [](const BSONObj& value) {
        const BSONObj plain = IndexBoundsBuilder::objFromElement(value.firstElement(), nullptr);
        return RecordIdBound(record_id_helpers::keyForObj(plain), plain);
    };

    const auto& ranges = rangeList.getRanges();
    for (size_t i = 0; i < expectedRanges.size(); ++i) {
        const auto& [expMin, expMax] = expectedRanges[i];
        const auto& r = ranges[i];

        if (expMin._value) {
            ASSERT(r.getMin()) << "Range " << i << ": expected a lower bound but got none";
            ASSERT_EQ(makeBound(*expMin._value), *r.getMin());
            ASSERT_EQ(expMin._inclusive, r.isMinInclusive());
        } else {
            ASSERT_FALSE(r.getMin()) << "Range " << i << ": expected no lower bound";
        }

        if (expMax._value) {
            ASSERT(r.getMax()) << "Range " << i << ": expected an upper bound but got none";
            ASSERT_EQ(makeBound(*expMax._value), *r.getMax());
            ASSERT_EQ(expMax._inclusive, r.isMaxInclusive());
        } else {
            ASSERT_FALSE(r.getMax()) << "Range " << i << ": expected no upper bound";
        }
    }

    // Second pass: simplify the filter given the computed scan range.
    QueryPlannerAccess::simplifyFilter(
        expr, rangeList, collator /* queryCollator */, nullptr /* ccCollator */, "_id");

    ASSERT_BSONOBJ_EQ(fromjson(expected), expr->serialize());
}

TEST(PlannerAccessTest, SimplifyFilterInequalities) {
    // Test that for clustered collection scans, filters containing inequalities
    // which may be completely represented as min/max record ID are simplified
    // when constructing a scan.

    // Where there is no bound provided in the query, handleRIDRangeScan will still set coarse
    // bounds based on the datatype, but the full filter remains present.
    // These are the expected coarse bounds for numeric and string types.
    Bound minNum = {
        BSONObjBuilder().appendMinForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};
    Bound maxNum = {
        BSONObjBuilder().appendMaxForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};
    Bound minStr = {
        BSONObjBuilder().appendMinForType("", stdx::to_underlying(BSONType::string)).obj(), true};
    Bound maxStr = {
        BSONObjBuilder().appendMaxForType("", stdx::to_underlying(BSONType::string)).obj(), true};

    testSimplify("{_id:{$gt: 2}}", "{}", {{{2}, maxNum}});
    testSimplify("{_id:{$lt: 4}}", "{}", {{minNum, {4}}});

    testSimplify("{_id:{$gt: 'x'}}", "{}", {{{"x"}, maxStr}});
    testSimplify("{_id:{$lt: 'z'}}", "{}", {{minStr, {"z"}}});

    testSimplify("{$and: [{_id:{$gt: 2}}, {_id:{$lt: 4}}]}", "{}", {{{2}, {4}}});
    testSimplify("{$and: [{_id:{$gte: 2}}, {_id:{$lt: 4}}]}", "{}", {{{2, true}, {4}}});
    testSimplify("{$and: [{_id:{$gt: 2}}, {_id:{$lte: 4}}]}", "{}", {{{2}, {4, true}}});
    testSimplify("{$and: [{_id:{$gte: 2}}, {_id:{$lte: 4}}]}", "{}", {{{2, true}, {4, true}}});

    testSimplify("{$and: [{_id:{$gt: 'x'}}, {_id:{$lt: 'z'}}]}", "{}", {{{"x"}, {"z"}}});
    testSimplify("{$and: [{_id:{$gte: 'x'}}, {_id:{$lt: 'z'}}]}", "{}", {{{"x", true}, {"z"}}});
    testSimplify("{$and: [{_id:{$gt: 'x'}}, {_id:{$lte: 'z'}}]}", "{}", {{{"x"}, {"z", true}}});
    testSimplify(
        "{$and: [{_id:{$gte: 'x'}}, {_id:{$lte: 'z'}}]}", "{}", {{{"x", true}, {"z", true}}});

    // Fully simplifiable, with already redundant bounds.
    testSimplify("{$and: [{_id:{$gt: 2}}, {_id:{$gte: 2}}]}", "{}", {{{2}, maxNum}});
    testSimplify("{$and: [{_id:{$gt: 3}}, {_id:{$gte: 2}}]}", "{}", {{{3}, maxNum}});
    testSimplify("{$and: [{_id:{$lt: 2}}, {_id:{$lte: 2}}]}", "{}", {{minNum, {2}}});
    testSimplify("{$and: [{_id:{$lt: 1}}, {_id:{$lte: 2}}]}", "{}", {{minNum, {1}}});

    testSimplify("{$and: [{_id:{$gt: 'x'}}, {_id:{$gte: 'x'}}]}", "{}", {{{"x"}, maxStr}});
    testSimplify("{$and: [{_id:{$gt: 'y'}}, {_id:{$gte: 'x'}}]}", "{}", {{{"y"}, maxStr}});
    testSimplify("{$and: [{_id:{$lt: 'x'}}, {_id:{$lte: 'x'}}]}", "{}", {{minStr, {"x"}}});
    testSimplify("{$and: [{_id:{$lt: 'w'}}, {_id:{$lte: 'x'}}]}", "{}", {{minStr, {"w"}}});
}

TEST(PlannerAccessTest, SimplifyFilterEqualities) {
    // Test that for clustered collection scans, filters containing equalities
    // which may be completely represented as min/max record ID are simplified
    // when constructing a scan.

    testSimplify("{_id:{$eq: 2}}", "{}", {{{2, true}, {2, true}}});

    testSimplify("{_id:{$eq: 'x'}}", "{}", {{{"x", true}, {"x", true}}});

    // Equality is effectively (<= && >=), and should interact with other inequalities as such.
    testSimplify("{$and: [{_id:{$eq: 2}}, {_id:{$lt: 4}}]}", "{}", {{{2, true}, {2, true}}});

    testSimplify(
        "{$and: [{_id:{$eq: 'x'}}, {_id:{$lt: 'z'}}]}", "{}", {{{"x", true}, {"x", true}}});
}

TEST(PlannerAccessTest, SimplifyFilterNestedConjunctions) {
    // Variant of the above, containing nested conjunctions.
    // Test that filters are recursively simplified.
    testSimplify(R"({$and: [
                     {$and: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]},
                     {_id:{$lt: 4}}
                     ]
                 })",
                 "{}",
                 {{{2}, {3, true}}});
    testSimplify(R"({$and: [
                     {$and: [{_id:{$gte: 0}}, {_id:{$lt: 5}}]},
                     {$and: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}
                     ]
                 })",
                 "{}",
                 {{{2}, {3, true}}});
}

TEST(PlannerAccessTest, SimplifyFilterDisjunctions) {
    Bound minNum = {
        BSONObjBuilder().appendMinForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};
    Bound maxNum = {
        BSONObjBuilder().appendMaxForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};

    // Top-level disjunction: both branches are simple inequalities on _id.
    // Each branch produces a range; their union covers (minNum, maxNum).
    // Both branches are individually redundant, so the whole $or is simplified away.
    testSimplify("{$or: [{_id:{$gt: 2}}, {_id:{$lt: 4}}]}", "{}", {{minNum, maxNum}});

    // Nested disjunction in a conjunction: the $or becomes redundant (as above),
    // leaving only the {$lt:4} conjunct, which can be expressed as a RecordId bound.
    // Then the whole $and simplifies to {}.
    testSimplify("{$and: [{$or: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}, {_id:{$lt: 4}}]}",
                 "{}",
                 {{minNum, {4}}});

    // Disjunction containing conjunction.
    testSimplify("{$or: [{$and: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}, {_id:{$lt: 4}}]}",
                 "{}",
                 {{minNum, {4}}});
}

// ---------------------------------------------------------------------------
// QueryPlannerAccess::rangeListContainedIn
// ---------------------------------------------------------------------------

// Helpers: build a single-range RecordIdRangeList from integer RecordIds.
RecordIdRangeList makeIntRangeList(int min, bool minIncl, int max, bool maxIncl) {
    RecordIdRange r;
    r.maybeNarrowMin(RecordIdBound(RecordId(min)), minIncl);
    r.maybeNarrowMax(RecordIdBound(RecordId(max)), maxIncl);
    return RecordIdRangeList(r);
}

TEST(PlannerAccessTest, RangeListContainedIn_InnerSubsetOfOuter) {
    auto inner = makeIntRangeList(3, true, 7, true);   // [3, 7]
    auto outer = makeIntRangeList(1, true, 10, true);  // [1, 10]
    ASSERT_TRUE(QueryPlannerAccess::rangeListContainedIn(/* inner */ inner, /* outer */ outer));
    // Asymmetry: the outer range is NOT contained in the (smaller) inner range.
    ASSERT_FALSE(QueryPlannerAccess::rangeListContainedIn(/* inner */ outer, /* outer */ inner));
}

TEST(PlannerAccessTest, RangeListContainedIn_EqualLists) {
    auto inner = makeIntRangeList(3, true, 7, true);
    auto outer = makeIntRangeList(3, true, 7, true);
    // Equal sets are mutually contained (A ⊆ B and B ⊆ A).
    ASSERT_TRUE(QueryPlannerAccess::rangeListContainedIn(/* inner */ inner, /* outer */ outer));
    ASSERT_TRUE(QueryPlannerAccess::rangeListContainedIn(/* inner */ outer, /* outer */ inner));
    // Referential equality also holds.
    ASSERT_TRUE(QueryPlannerAccess::rangeListContainedIn(/* inner */ inner, /* outer */ inner));
}

// ---------------------------------------------------------------------------
// testSimplify: multi-range ($in / disjoint $or) cases
// ---------------------------------------------------------------------------

TEST(PlannerAccessTest, SimplifyFilterInMultiRange) {
    // $in on the cluster key produces one point range per value; the filter
    // is fully expressible as those ranges and is simplified away.
    testSimplify("{_id:{$in: [1, 3, 7]}}",
                 "{}",
                 {{{1, true}, {1, true}}, {{3, true}, {3, true}}, {{7, true}, {7, true}}});
}

TEST(PlannerAccessTest, SimplifyFilterInWithConjunct) {
    // $in intersected with an upper bound: values above the bound are excluded.
    // ( [1,1] ∪ [3,3] ∪ [7,7] ) ∩ (-∞, 5) = [1,1] ∪ [3,3]
    testSimplify("{$and: [{_id:{$in: [1, 3, 7]}}, {_id:{$lt: 5}}]}",
                 "{}",
                 {{{1, true}, {1, true}}, {{3, true}, {3, true}}});
}

TEST(PlannerAccessTest, SimplifyFilterDisjointOrMultiRange) {
    Bound minNum = {
        BSONObjBuilder().appendMinForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};
    Bound maxNum = {
        BSONObjBuilder().appendMaxForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};

    // Non-overlapping branches produce two separate ranges; both are redundant given
    // the scan range list, so the whole $or simplifies away.
    testSimplify("{$or: [{_id:{$lt: 3}}, {_id:{$gt: 7}}]}", "{}", {{minNum, {3}}, {{7}, maxNum}});
}

TEST(PlannerAccessTest, SimplifyFilterCollationMismatch) {
    // With a query collator that does not match the (null) collection collator, string
    // comparisons are collation-incompatible. The string predicate must NOT be simplified
    // away; only a coarse type-range bound is produced for it.
    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);

    Bound maxNum = {
        BSONObjBuilder().appendMaxForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};
    Bound minStr = {
        BSONObjBuilder().appendMinForType("", stdx::to_underlying(BSONType::string)).obj(), true};
    Bound maxStr = {
        BSONObjBuilder().appendMaxForType("", stdx::to_underlying(BSONType::string)).obj(), true};

    // $gt:5 is on a numeric value (unaffected by collation) and would normally be exact,
    // but the scan range also spans the string type, so the numeric branch can't be dropped.
    // {$eq:"hey"} is string-typed with an incompatible collator: it stays in the filter.
    testSimplify("{$or: [{_id:{$gt: 5}}, {_id:{$eq: 'hey'}}]}",
                 "{$or: [{_id:{$gt: 5}}, {_id:{$eq: 'hey'}}]}",
                 {{{5}, maxNum}, {minStr, maxStr}},
                 &mockCollator);
}

TEST(PlannerAccessTest, SimplifyFilterNonClusterKeyField) {
    // A predicate on a field other than the cluster key does not constrain the RecordId
    // scan range (stays unbounded) and must not be removed from the filter.
    testSimplify("{a:{$gt: 5}}", "{a:{$gt: 5}}", {{Bound{}, Bound{}}});
}

}  // namespace
}  // namespace mongo
