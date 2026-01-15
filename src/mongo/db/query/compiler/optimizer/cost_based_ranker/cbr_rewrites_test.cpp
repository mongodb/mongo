/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_rewrites.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/query/compiler/optimizer/cost_based_ranker/cbr_test_utils.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/unittest/unittest.h"


namespace mongo::cost_based_ranker {
namespace {

const BSONObj constantHolder = BSON_ARRAY(BSONArray() << BSONNULL << MINKEY << MAXKEY);
const BSONElement emptyArrayElem = constantHolder["0"];
const BSONElement nullElem = constantHolder["1"];
const BSONElement minKeyElem = constantHolder["2"];
const BSONElement maxKeyElem = constantHolder["3"];

inline void ASSERT_EXPR(const MatchExpression& expected,
                        const std::unique_ptr<MatchExpression>& actual) {
    ASSERT_TRUE(expected.equivalent(actual.get()))
        << expected.debugString() << " != " << actual->debugString();
}

inline void ASSERT_EXPR(const char* expected, const std::unique_ptr<MatchExpression>& actual) {
    ASSERT_EXPR(*parse(fromjson(expected)), actual);
}

IndexBounds indexBoundsForTypeMatchExpression(const char* path, MatcherTypeSet types) {
    const auto keyPattern = BSON(path << 1);
    const auto index = buildSimpleIndexEntry({path});
    const auto typeExpr = TypeMatchExpression(mongo::StringData(path), types);
    OrderedIntervalList oil;
    IndexBoundsBuilder::translate(&typeExpr, keyPattern[path], index, &oil);
    IndexBounds bounds;
    bounds.fields = {oil};
    return bounds;
}

TEST(CBRRewrites, NullIntervals) {
    const IndexBounds bounds = makeRangeIntervalBounds(
        BSON("" << MINKEY << "" << MINKEY), BoundInclusion::kExcludeBothStartAndEndKeys, "a");
    const auto me = getMatchExpressionFromBounds(bounds, nullptr);
    ASSERT_EXPR(*parse(fromjson("{ $alwaysFalse: 1 }")), me);
}

TEST(CBRRewrites, TypeTrivial) {
    for (const auto type : {
             BSONType::object,
             BSONType::binData,
             BSONType::oid,
             BSONType::boolean,
             BSONType::date,
             BSONType::regEx,
             BSONType::dbRef,
             BSONType::code,
             BSONType::codeWScope,
             BSONType::timestamp,
         }) {
        const auto bounds = indexBoundsForTypeMatchExpression("a", type);
        const auto me = getMatchExpressionFromBounds(bounds, nullptr);
        const auto expected = TypeMatchExpression("a"_sd, MatcherTypeSet(type));
        ASSERT_EXPR(expected, me);
    }
}

TEST(CBRRewrites, TypeMinMaxKey) {
    for (const auto type : {
             BSONType::minKey,
             BSONType::maxKey,
         }) {
        const auto bounds = indexBoundsForTypeMatchExpression("a", type);
        const auto me = getMatchExpressionFromBounds(bounds, nullptr);
        ASSERT_EXPR(
            EqualityMatchExpression("a"_sd, type == BSONType::minKey ? minKeyElem : maxKeyElem),
            me);
    }
}

TEST(CBRRewrites, TypeNumeric) {
    MatcherTypeSet allNumbers;
    allNumbers.allNumbers = true;
    for (const auto type : {BSONType::numberDouble,
                            BSONType::numberInt,
                            BSONType::numberLong,
                            BSONType::numberDecimal}) {
        const auto bounds = indexBoundsForTypeMatchExpression("a", type);
        const auto me = getMatchExpressionFromBounds(bounds, nullptr);
        ASSERT_EXPR("{ a: { $type: [ \"number\" ] } }", me);
    }

    const auto bounds = indexBoundsForTypeMatchExpression("a", allNumbers);
    const auto me = getMatchExpressionFromBounds(bounds, nullptr);
    ASSERT_EXPR("{ a: { $type: [ \"number\" ] } }", me);
}

TEST(CBRRewrites, TypeStringSymbol) {
    for (const auto type : {BSONType::string, BSONType::symbol}) {
        const auto typeExpr = TypeMatchExpression("a"_sd, type);
        const auto bounds = indexBoundsForTypeMatchExpression("a", type);
        const auto me = getMatchExpressionFromBounds(bounds, &typeExpr);
        ASSERT_EXPR(typeExpr, me);
    }
}

TEST(CBRRewrites, UndefinedPointTypeUndefined) {
    const IndexBounds bounds = makePointIntervalBounds(BSON("" << BSONUndefined), "a");
    const BSONObj residualFilterBSON = fromjson("{a: {$type: 'undefined'}}");
    const auto residualFilter = parse(residualFilterBSON);
    const auto me = getMatchExpressionFromBounds(bounds, residualFilter.get());
    ASSERT_EXPR(*residualFilter, me);
}

TEST(CBRRewrites, UndefinedPointEmptyArray) {
    const IndexBounds bounds = makePointIntervalBounds(BSON("" << BSONUndefined), "a");
    const BSONObj residualFilterBSON = fromjson("{a: []}");
    const auto residualFilter = parse(residualFilterBSON);
    const auto me = getMatchExpressionFromBounds(bounds, residualFilter.get());
    ASSERT_EXPR(*residualFilter, me);
}

// TODO(SERVER-105939): Enable this after implementing support for array equalities
// TEST(CBRRewrites, AdditionalFilterArrayEquality) {
//     const auto path = "a"_sd;
//     const auto keyPattern = BSON(path << 1);
//     const auto index = buildSimpleIndexEntry(keyPattern);
//     for (const auto& array : {BSON_ARRAY(0), BSON_ARRAY(0 << 1), BSONArray()}) {
//         const auto tmp = BSON("" << array);
//         const auto arrayEQ = EqualityMatchExpression(path, tmp[""]);
//         OrderedIntervalList oil;
//         IndexBoundsBuilder::translate(&arrayEQ, keyPattern[path], index, &oil);
//         IndexBounds bounds;
//         bounds.fields = {oil};
//         // Array equality predicates get the original predicate itself as the
//         // residual filter.
//         const auto me = getMatchExpressionFromBounds(bounds, &arrayEQ);
//         ASSERT_EXPR(arrayEQ, me);
//     }
// }


TEST(CBRRewrites, AdditionalFilterTypeInteger) {
    const auto query = fromjson("{a: {$lte: 5}}");
    const auto keyPattern = BSON("a" << 1);
    const auto index = buildSimpleIndexEntry({"a"});
    OrderedIntervalList oil;
    IndexBoundsBuilder::translate(parse(query).get(), keyPattern["a"], index, &oil);
    IndexBounds bounds;
    bounds.fields = {oil};
    const auto me =
        getMatchExpressionFromBounds(bounds, parse(fromjson("{a: {$type: 'int'}}")).get());
    ASSERT_EXPR(*parse(fromjson("{ $and: [ { a: { $lte: 5 } }, { a: { $type: [ 16 ] } } ] }")), me);
}

TEST(CBRRewrites, NumberSemiOpenRange) {
    const auto query = fromjson("{a: {$lte: 5}}");
    const auto expected = parse(query);
    const auto keyPattern = BSON("a" << 1);
    const auto index = buildSimpleIndexEntry({"a"});
    OrderedIntervalList oil;
    IndexBoundsBuilder::translate(expected.get(), keyPattern["a"], index, &oil);
    IndexBounds bounds;
    bounds.fields = {oil};
    const auto me = getMatchExpressionFromBounds(bounds, nullptr);
    ASSERT_EXPR(*expected, me);
}

TEST(CBRRewrites, NumberNotNaN) {
    const auto tmp = BSON("" << -std::numeric_limits<double>::infinity());
    const auto query = GTEMatchExpression("a"_sd, tmp[""]);
    const auto keyPattern = BSON("a" << 1);
    const auto index = buildSimpleIndexEntry({"a"});
    OrderedIntervalList oil;
    IndexBoundsBuilder::translate(&query, keyPattern["a"], index, &oil);
    IndexBounds bounds;
    bounds.fields = {oil};
    const auto me = getMatchExpressionFromBounds(bounds, nullptr);
    ASSERT_EXPR(query, me);
}

TEST(CBRRewrites, NullPointExistsFalse) {
    const IndexBounds bounds = makePointIntervalBounds(BSON("" << BSONNULL), "a");
    const BSONObj residualObj = fromjson("{a: {$exists: false}}");
    const auto residual = parse(residualObj);
    const auto me = getMatchExpressionFromBounds(bounds, residual.get());
    ASSERT_EXPR(*residual, me);
}

TEST(CBRRewrites, NullPointEqNull) {
    const IndexBounds bounds = makePointIntervalBounds(BSON("" << BSONNULL), "a");
    const BSONObj residualObj = fromjson("{a: null}");
    const auto residual = parse(residualObj);
    const auto me = getMatchExpressionFromBounds(bounds, residual.get());
    ASSERT_EXPR(*residual, me);
}

TEST(CardinalityEstimator, PointIntervalToEqMatchExpression) {
    const auto pointBounds = makePointIntervalBounds(5.0, "a");
    auto expr = getMatchExpressionFromBounds(pointBounds, nullptr);

    ASSERT_EXPR("{ a: { $eq: 5.0 } }", expr);
}

TEST(CardinalityEstimator, EmptyOILToAlwaysFalseMatchExpression) {
    auto testIndex = buildSimpleIndexEntry({"a"});
    BSONObj query1 = fromjson("{a: 3}");
    auto expr1 = parse(query1);

    BSONElement elt = query1.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr1.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    BSONObj query2 = fromjson("{a: 8}");
    auto expr2 = parse(query2);
    IndexBoundsBuilder::translateAndIntersect(
        expr2.get(), elt, testIndex, &oil, &tightness, &ietBuilder);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);

    IndexBounds bounds;
    bounds.fields.push_back(oil);
    auto expr = getMatchExpressionFromBounds(bounds, nullptr);

    ASSERT_EXPR(AlwaysFalseMatchExpression(), expr);
}

TEST(CardinalityEstimator, FullyOpenIntervalToAlwaysTrueMatchExpression) {
    BSONObj keyPattern = fromjson("{a: 1}");
    OrderedIntervalList oil;
    IndexBoundsBuilder::allValuesForField(keyPattern.firstElement(), &oil);
    ASSERT(oil.isFullyOpen());

    IndexBounds bounds;
    bounds.fields.push_back(oil);
    auto expr = getMatchExpressionFromBounds(bounds, nullptr);

    ASSERT_EXPR(AlwaysTrueMatchExpression(), expr);
}

TEST(CardinalityEstimator, RangeIntervalToAndMatchExpression) {
    const IndexBounds rangeBounds = makeRangeIntervalBounds(
        BSON("" << 5.0 << "" << 10.0), BoundInclusion::kIncludeBothStartAndEndKeys, "a");
    auto expr = getMatchExpressionFromBounds(rangeBounds, nullptr);

    ASSERT_EXPR("{ $and: [ { a: { $lte: 10.0 } }, { a: { $gte: 5.0 } } ] }", expr);
}


TEST(CardinalityEstimator, OpenRangeIntervalToGTEMatchExpression) {
    std::vector<std::string> indexFields = {"a"};
    // Bounds for [3, inf]
    IndexBounds bounds =
        makeRangeIntervalBounds(BSON("" << 3.0 << "" << std::numeric_limits<double>::infinity()),
                                BoundInclusion::kIncludeBothStartAndEndKeys,
                                indexFields[0]);
    auto expr = getMatchExpressionFromBounds(bounds, nullptr);

    ASSERT_EXPR("{ a: { $gte: 3.0 } }", expr);
}

TEST(CardinalityEstimator, OpenRangeIntervalToLTMatchExpression) {
    std::vector<std::string> indexFields = {"a"};
    // Bounds for ["", "sun")
    IndexBounds bounds = makeRangeIntervalBounds(
        BSON("" << "" << "" << "sun"), BoundInclusion::kIncludeStartKeyOnly, indexFields[0]);
    auto expr = getMatchExpressionFromBounds(bounds, nullptr);

    ASSERT_EXPR("{ a: { $lt: \"sun\" } }", expr);
}

TEST(CardinalityEstimator, TwoIntervalsAndExpressionToAndMatchExpression) {
    auto pointOIL = makePointInterval(100, "a");
    OrderedIntervalList rangeOIL("b");
    auto range = BSON("" << 5 << "" << 10);
    rangeOIL.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(range, BoundInclusion::kIncludeBothStartAndEndKeys));
    IndexBounds bounds;
    bounds.fields.push_back(pointOIL);
    bounds.fields.push_back(rangeOIL);

    auto pred = fromjson("{c : 'xyz'}");
    auto parsedPred = parse(pred);

    auto expr = getMatchExpressionFromBounds(bounds, parsedPred.get());

    // Notice the order of the predicates here specifically matches the order of the generated
    // MatchExpressions by getMatchExpressionFromBounds
    ASSERT_EXPR(
        "{ $and: [ { a: { $eq: 100.0 } }, { c: { $eq: \"xyz\" } }, "
        "{ b: { $lte: 10 } }, { b: { $gte: 5 } } ] }",
        expr);
}


}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
