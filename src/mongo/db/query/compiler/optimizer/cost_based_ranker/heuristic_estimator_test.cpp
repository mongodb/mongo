// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/cost_based_ranker/heuristic_estimator.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::cost_based_ranker {
namespace {

CardinalityEstimate makeCard(double d) {
    return CardinalityEstimate(CardinalityType(d), EstimationSource::Code);
}

SelectivityEstimate makeSel(double d) {
    return SelectivityEstimate(SelectivityType(d), EstimationSource::Code);
}

std::unique_ptr<MatchExpression> parse(const BSONObj& bson) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto expr = MatchExpressionParser::parse(
        bson, expCtx, ExtensionsCallbackNoop(), MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(expr);
    return std::move(expr.getValue());
}

TEST(HeuristicEstimator, AlwaysFalse) {
    BSONObj query = fromjson("{$alwaysFalse: 1}");
    auto expr = parse(query);
    ASSERT_EQ(heuristicLeafMatchExpressionSel(expr.get(), makeCard(100)), zeroSel);
}

TEST(HeuristicEstimator, AlwaysTrue) {
    BSONObj query = fromjson("{$alwaysTrue: 1}");
    auto expr = parse(query);
    ASSERT_EQ(heuristicLeafMatchExpressionSel(expr.get(), makeCard(100)), oneSel);
}

TEST(HeuristicEstimate, ModExpression) {
    BSONObj query = fromjson("{a: {$mod: [5, 2]}}");
    auto expr = parse(query);
    // Selectivty of mod 5 is 1/5
    ASSERT_EQ(heuristicLeafMatchExpressionSel(expr.get(), makeCard(100)), makeSel(0.2));
}

TEST(HeuristicEstimate, ModNegativeDivisorExpression) {
    BSONObj query = fromjson("{a: {$mod: [-5, 2]}}");
    auto expr = parse(query);
    ASSERT_EQ(heuristicLeafMatchExpressionSel(expr.get(), makeCard(100)), makeSel(0.2));
}

TEST(HeuristicEstimate, ExistsExpression) {
    // Note: {$exists: false} is parsed as {$not: {$exists: true}}
    BSONObj query = fromjson("{a: {$exists: true}}");
    auto expr = parse(query);
    ASSERT_EQ(heuristicLeafMatchExpressionSel(expr.get(), makeCard(100)), kExistsSel);
}

SelectivityEstimate estimateLeafQueryExpr(std::string queryStr, double card) {
    BSONObj query = fromjson(queryStr);
    auto expr = parse(query);
    return heuristicLeafMatchExpressionSel(expr.get(), makeCard(card));
}

TEST(HeuristicEstimate, SelectivityRelationships) {
    double card = 100.0;
    auto eqSel = estimateLeafQueryExpr("{a: 42}", card);
    auto regexSel = estimateLeafQueryExpr("{a: /abc/}", card);
    auto sizeSel = estimateLeafQueryExpr("{a: { $size: 3 }}", card);
    auto bitOpSel = estimateLeafQueryExpr("{a: {$bitsAllClear: [1, 5]}}", card);
    auto exprSel = estimateLeafQueryExpr("{ '$expr' : {  '$ifNull' : [ '$a', null ] } }", card);

    ASSERT_TRUE(approxLt(eqSel, regexSel));
    ASSERT_TRUE(approxLt(regexSel, sizeSel));
    ASSERT_EQ(sizeSel, bitOpSel);
    ASSERT_EQ(sizeSel, exprSel);
}

TEST(HeuristicEstimate, ScaledPredSelStaysInRangeForSubUnitCardinality) {
    // An equality predicate routes through heuristicScaledPredSel(inputCard,
    // kEqualityScalingFactor) with no 'inputCard <= 1' gate. With inputCard = 0.5 the unclamped
    // formula is 1 / pow(0.5, 0.5) = sqrt(2) ~= 1.414, which exceeds 1 and tasserts (9274200) the
    // moment the SelectivityType is constructed. Clamping inputCard to >= 1 makes pow(1, f) == 1,
    // so the selectivity is exactly 1.0; without the clamp this call aborts before returning.
    auto sel = estimateLeafQueryExpr("{a: 42}", 0.5);
    ASSERT_EQ(sel.toDouble(), 1.0);
}

}  // unnamed namespace
}  // namespace mongo::cost_based_ranker
