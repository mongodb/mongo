/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/query/planner_access.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/record_id_bound.h"
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

void testSimplify(auto input,
                  auto expected,
                  Bound expectedMin,
                  Bound expectedMax,
                  const CollatorInterface* collator = nullptr) {
    boost::intrusive_ptr<ExpressionContext> expCtx{new ExpressionContextForTest()};
    BSONObj query = fromjson(input);
    auto expr = Matcher(query, expCtx).getMatchExpression()->clone();

    RecordIdRange recordRange;
    std::set<const MatchExpression*> redundantExprs;

    (void)QueryPlannerAccess::handleRIDRangeScan(
        expr.get(),
        nullptr /* query collator */,
        nullptr /* ccCollator */,
        "_id" /* clustered field name */,
        recordRange,
        [&](const auto& expr) { redundantExprs.insert(expr); });

    auto makeBound = [&](auto value) {
        const BSONObj collated = IndexBoundsBuilder::objFromElement(value.firstElement(), collator);
        return RecordIdBound(record_id_helpers::keyForObj(collated), collated);
    };

    // Assert that the proposed collection scan bounds are as expected.
    if (expectedMin._value) {
        ASSERT_EQ(makeBound(*expectedMin._value), recordRange.getMin());
        ASSERT_EQ(expectedMin._inclusive, recordRange.isMinInclusive());
    } else {
        ASSERT_EQ(boost::none, recordRange.getMin());
    }

    if (expectedMax._value) {
        ASSERT_EQ(makeBound(*expectedMax._value), recordRange.getMax());
        ASSERT_EQ(expectedMax._inclusive, recordRange.isMaxInclusive());
    } else {
        ASSERT_EQ(boost::none, recordRange.getMax());
    }

    // Simplify the filter.
    QueryPlannerAccess::simplifyFilter(expr, redundantExprs);

    // Both bounds can be expressed as min/max record IDs, so the filter should be simplified.
    BSONObj expectedSerialization = fromjson(expected);

    ASSERT_BSONOBJ_EQ(expectedSerialization, expr->serialize());
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

    testSimplify("{_id:{$gt: 2}}", "{}", {2}, {maxNum});
    testSimplify("{_id:{$lt: 4}}", "{}", {minNum}, {4});

    testSimplify("{_id:{$gt: 'x'}}", "{}", {"x"}, {maxStr});
    testSimplify("{_id:{$lt: 'z'}}", "{}", {minStr}, {"z"});

    testSimplify("{$and: [{_id:{$gt: 2}}, {_id:{$lt: 4}}]}", "{}", {2}, {4});
    testSimplify("{$and: [{_id:{$gte: 2}}, {_id:{$lt: 4}}]}", "{}", {2, true}, {4});
    testSimplify("{$and: [{_id:{$gt: 2}}, {_id:{$lte: 4}}]}", "{}", {2}, {4, true});
    testSimplify("{$and: [{_id:{$gte: 2}}, {_id:{$lte: 4}}]}", "{}", {2, true}, {4, true});

    testSimplify("{$and: [{_id:{$gt: 'x'}}, {_id:{$lt: 'z'}}]}", "{}", {"x"}, {"z"});
    testSimplify("{$and: [{_id:{$gte: 'x'}}, {_id:{$lt: 'z'}}]}", "{}", {"x", true}, {"z"});
    testSimplify("{$and: [{_id:{$gt: 'x'}}, {_id:{$lte: 'z'}}]}", "{}", {"x"}, {"z", true});
    testSimplify("{$and: [{_id:{$gte: 'x'}}, {_id:{$lte: 'z'}}]}", "{}", {"x", true}, {"z", true});

    // Fully simplifiable, with already redundant bounds.
    testSimplify("{$and: [{_id:{$gt: 2}}, {_id:{$gte: 2}}]}", "{}", {2}, {maxNum});
    testSimplify("{$and: [{_id:{$gt: 3}}, {_id:{$gte: 2}}]}", "{}", {3}, {maxNum});
    testSimplify("{$and: [{_id:{$lt: 2}}, {_id:{$lte: 2}}]}", "{}", {minNum}, {2});
    testSimplify("{$and: [{_id:{$lt: 1}}, {_id:{$lte: 2}}]}", "{}", {minNum}, {1});

    testSimplify("{$and: [{_id:{$gt: 'x'}}, {_id:{$gte: 'x'}}]}", "{}", {"x"}, {maxStr});
    testSimplify("{$and: [{_id:{$gt: 'y'}}, {_id:{$gte: 'x'}}]}", "{}", {"y"}, {maxStr});
    testSimplify("{$and: [{_id:{$lt: 'x'}}, {_id:{$lte: 'x'}}]}", "{}", {minStr}, {"x"});
    testSimplify("{$and: [{_id:{$lt: 'w'}}, {_id:{$lte: 'x'}}]}", "{}", {minStr}, {"w"});
}

TEST(PlannerAccessTest, SimplifyFilterEqualities) {
    // Test that for clustered collection scans, filters containing equalities
    // which may be completely represented as min/max record ID are simplified
    // when constructing a scan.

    testSimplify("{_id:{$eq: 2}}", "{}", {2, true}, {2, true});

    testSimplify("{_id:{$eq: 'x'}}", "{}", {"x", true}, {"x", true});

    // Equality is effectively (<= && >=), and should interact with other inequalities as such.
    testSimplify("{$and: [{_id:{$eq: 2}}, {_id:{$lt: 4}}]}", "{}", {2, true}, {2, true});

    testSimplify("{$and: [{_id:{$eq: 'x'}}, {_id:{$lt: 'z'}}]}", "{}", {"x", true}, {"x", true});
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
                 {2},
                 {3, true});
    testSimplify(R"({$and: [
                     {$and: [{_id:{$gte: 0}}, {_id:{$lt: 5}}]},
                     {$and: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}
                     ]
                 })",
                 "{}",
                 {2},
                 {3, true});
}

TEST(PlannerAccessTest, SimplifyFilterDisjunctionsNotAffected) {
    // Variant of the above, containing disjunctions.
    // Inequalities in disjunctions cannot be simplified away as min/max record ID
    // (assuming common terms would already have been factored out).

    // Top level disjunction
    // handleRIDRangeScan does not enter disjunctions; no values seen
    // so not even coarse datatype based min/max will be set.
    testSimplify("{$or: [{_id:{$gt: 2}}, {_id:{$lt: 4}}]}",
                 "{$or: [{_id:{$gt: 2}}, {_id:{$lt: 4}}]}",
                 {},
                 {});

    Bound minNum = {
        BSONObjBuilder().appendMinForType("", stdx::to_underlying(BSONType::numberInt)).obj(),
        true};
    // Nested disjunction.
    // The child of the top level conjunction _is_ seen, so a coarse lower bound will be set.
    testSimplify("{$and: [{$or: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}, {_id:{$lt: 4}}]}",
                 "{$and: [{$or: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}]}",
                 {minNum},
                 {4});
    // Disjunction containing conjunction.
    testSimplify("{$or: [{$and: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}, {_id:{$lt: 4}}]}",
                 "{$or: [{$and: [{_id:{$gt: 2}}, {_id:{$lte: 3}}]}, {_id:{$lt: 4}}]}",
                 {},
                 {});
}

}  // namespace
}  // namespace mongo
