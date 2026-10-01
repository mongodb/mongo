// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/clustered_collection_scan.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <set>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

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

namespace {

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

    (void)ClusteredCollectionScanPlanner::handleRIDRangeScanLegacy(
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
    ClusteredCollectionScanPlanner::simplifyFilterLegacy(expr, redundantExprs);

    // Both bounds can be expressed as min/max record IDs, so the filter should be simplified.
    BSONObj expectedSerialization = fromjson(expected);

    ASSERT_BSONOBJ_EQ(expectedSerialization, expr->serialize());
}

}  // namespace

TEST(ClusteredCollectionScanTest, SimplifyFilterInequalities) {
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

TEST(ClusteredCollectionScanTest, SimplifyFilterEqualities) {
    // Test that for clustered collection scans, filters containing equalities
    // which may be completely represented as min/max record ID are simplified
    // when constructing a scan.

    testSimplify("{_id:{$eq: 2}}", "{}", {2, true}, {2, true});

    testSimplify("{_id:{$eq: 'x'}}", "{}", {"x", true}, {"x", true});

    // Equality is effectively (<= && >=), and should interact with other inequalities as such.
    testSimplify("{$and: [{_id:{$eq: 2}}, {_id:{$lt: 4}}]}", "{}", {2, true}, {2, true});

    testSimplify("{$and: [{_id:{$eq: 'x'}}, {_id:{$lt: 'z'}}]}", "{}", {"x", true}, {"x", true});
}

TEST(ClusteredCollectionScanTest, SimplifyFilterNestedConjunctions) {
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

TEST(ClusteredCollectionScanTest, SimplifyFilterDisjunctionsNotAffected) {
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
