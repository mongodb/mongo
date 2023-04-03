/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/range_predicate.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo::fle {
namespace {
class MockRangePredicate : public RangePredicate {
public:
    MockRangePredicate(const QueryRewriterInterface* rewriter) : RangePredicate(rewriter) {}

    MockRangePredicate(const QueryRewriterInterface* rewriter,
                       TagMap tags,
                       std::set<StringData> encryptedFields)
        : RangePredicate(rewriter) {}

    bool payloadValid = true;
    bool isStubPayload = false;

protected:
    bool isPayload(const BSONElement& elt) const override {
        return payloadValid;
    }

    bool isPayload(const Value& v) const override {
        return payloadValid;
    }

    bool isStub(BSONElement elt) const override {
        return isStubPayload;
    }

    bool isStub(Value elt) const override {
        return isStubPayload;
    }

    std::vector<PrfBlock> generateTags(BSONValue payload) const override {
        return stdx::visit(
            OverloadedVisitor{[&](BSONElement p) {
                                  if (p.isABSONObj()) {
                                      std::vector<PrfBlock> allTags;
                                      for (auto& val : p.Array()) {
                                          allTags.push_back(PrfBlock(
                                              {static_cast<unsigned char>(val.safeNumberInt())}));
                                      }
                                      return allTags;
                                  } else {
                                      return std::vector<PrfBlock>{};
                                  }
                              },
                              [&](std::reference_wrapper<Value> v) {
                                  if (v.get().isArray()) {
                                      auto arr = v.get().getArray();
                                      std::vector<PrfBlock> allTags;
                                      for (auto& val : arr) {
                                          allTags.push_back(PrfBlock(
                                              {static_cast<unsigned char>(val.coerceToInt())}));
                                      }
                                      return allTags;
                                  } else {
                                      return std::vector<PrfBlock>{};
                                  }
                              }},
            payload);
    }
};
class RangePredicateRewriteTest : public EncryptedPredicateRewriteTest {
public:
    RangePredicateRewriteTest() : _predicate(&_mock) {}

protected:
    MockRangePredicate _predicate;
};

TEST_F(RangePredicateRewriteTest, MatchRangeRewrite_NoStub) {
    RAIIServerParameterControllerForTest controller("featureFlagFLE2Range", true);

    std::vector<PrfBlock> allTags = {{1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}};

    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto payload = fromjson("{x: [1, 2, 3, 4, 5, 6, 7, 8, 9]}");

    assertRewriteForOp<GTMatchExpression>(_predicate, payload.firstElement(), allTags);
    assertRewriteForOp<GTEMatchExpression>(_predicate, payload.firstElement(), allTags);
    assertRewriteForOp<LTMatchExpression>(_predicate, payload.firstElement(), allTags);
    assertRewriteForOp<LTEMatchExpression>(_predicate, payload.firstElement(), allTags);
}

TEST_F(RangePredicateRewriteTest, MatchRangeRewrite_Stub) {
    RAIIServerParameterControllerForTest controller("featureFlagFLE2Range", true);

    auto expCtx = make_intrusive<ExpressionContextForTest>();

    auto payload = fromjson("{x: [1, 2, 3, 4, 5, 6, 7, 8, 9]}");

#define ASSERT_REWRITE_TO_TRUE(T)                                                             \
    {                                                                                         \
        std::unique_ptr<MatchExpression> inputExpr = std::make_unique<T>("age"_sd, Value(0)); \
        _predicate.isStubPayload = true;                                                      \
        auto rewrite = _predicate.rewrite(inputExpr.get());                                   \
        ASSERT_EQ(rewrite->matchType(), MatchExpression::ALWAYS_TRUE);                        \
    }

    // Rewrites that would normally go to disjunctions.
    {
        ASSERT_REWRITE_TO_TRUE(GTMatchExpression);
        ASSERT_REWRITE_TO_TRUE(GTEMatchExpression);
        ASSERT_REWRITE_TO_TRUE(LTMatchExpression);
        ASSERT_REWRITE_TO_TRUE(LTEMatchExpression);
    }

    // Rewrites that would normally go to $internalFleBetween.
    {
        _mock.setForceEncryptedCollScanForTest();
        ASSERT_REWRITE_TO_TRUE(GTMatchExpression);
        ASSERT_REWRITE_TO_TRUE(GTEMatchExpression);
        ASSERT_REWRITE_TO_TRUE(LTMatchExpression);
        ASSERT_REWRITE_TO_TRUE(LTEMatchExpression);
    }
}

TEST_F(RangePredicateRewriteTest, AggRangeRewrite_Stub) {
    RAIIServerParameterControllerForTest controller("featureFlagFLE2Range", true);

    auto ops = {"$gt", "$lt", "$gte", "$lte"};
    for (auto& op : ops) {
        auto input = fromjson(str::stream() << "{" << op << ": [\"$age\", {$literal: [1, 2, 3]}]}");
        auto inputExpr =
            ExpressionCompare::parseExpression(&_expCtx, input, _expCtx.variablesParseState);

        auto expected = ExpressionConstant::create(&_expCtx, Value(true));

        _predicate.isStubPayload = true;
        auto actual = _predicate.rewrite(inputExpr.get());
        ASSERT(actual);
        ASSERT_BSONOBJ_EQ(actual->serialize(false).getDocument().toBson(),
                          expected->serialize(false).getDocument().toBson());
    }
}

TEST_F(RangePredicateRewriteTest, AggRangeRewrite) {
    auto ops = {"$gt", "$lt", "$gte", "$lte"};
    for (auto& op : ops) {
        auto input = fromjson(str::stream() << "{" << op << ": [\"$age\", {$literal: [1, 2, 3]}]}");
        auto inputExpr =
            ExpressionCompare::parseExpression(&_expCtx, input, _expCtx.variablesParseState);

        auto expected = makeTagDisjunction(&_expCtx, toValues({{1}, {2}, {3}}));

        auto actual = _predicate.rewrite(inputExpr.get());

        ASSERT_BSONOBJ_EQ(actual->serialize(false).getDocument().toBson(),
                          expected->serialize(false).getDocument().toBson());
    }
}

TEST_F(RangePredicateRewriteTest, AggRangeRewriteNoOp) {
    auto ops = {"$gt", "$lt", "$gte", "$lte"};
    for (auto& op : ops) {
        auto input = fromjson(str::stream() << "{" << op << ": [\"$age\", {$literal: [1, 2, 3]}]}");
        auto inputExpr =
            ExpressionCompare::parseExpression(&_expCtx, input, _expCtx.variablesParseState);

        auto expected = inputExpr;

        _predicate.payloadValid = false;
        auto actual = _predicate.rewrite(inputExpr.get());
        ASSERT(actual == nullptr);
    }
}

BSONObj generateFFP(StringData path, int lb, int ub, int min, int max) {
    auto indexKey = getIndexKey();
    FLEIndexKeyAndId indexKeyAndId(indexKey.data, indexKeyId);
    auto userKey = getUserKey();
    FLEUserKeyAndId userKeyAndId(userKey.data, indexKeyId);

    auto edges = minCoverInt32(lb, true, ub, true, min, max, 1);
    FLE2RangeFindSpec spec(0, Fle2RangeOperator::kGt);
    auto ffp =
        FLEClientCrypto::serializeFindRangePayloadV2(indexKeyAndId, userKeyAndId, edges, 0, spec);

    BSONObjBuilder builder;
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindRangePayloadV2, ffp, &builder);
    return builder.obj();
}

template <typename T>
std::unique_ptr<MatchExpression> generateOpWithFFP(StringData path, int lb, int ub) {
    auto ffp = generateFFP(path, lb, ub, 0, 255);
    return std::make_unique<T>(path, ffp.firstElement());
}

std::unique_ptr<Expression> generateBetweenWithFFP(
    ExpressionContext* expCtx, ExpressionCompare::CmpOp op, StringData path, int lb, int ub) {
    auto ffp = Value(generateFFP(path, lb, ub, 0, 255).firstElement());
    auto ffpExpr = make_intrusive<ExpressionConstant>(expCtx, ffp);
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, path.toString(), expCtx->variablesParseState);
    std::vector<boost::intrusive_ptr<Expression>> children = {std::move(fieldpath),
                                                              std::move(ffpExpr)};
    return std::make_unique<ExpressionCompare>(expCtx, op, std::move(children));
}

TEST_F(RangePredicateRewriteTest, CollScanRewriteMatch) {
    _mock.setForceEncryptedCollScanForTest();
    auto expected = fromjson(R"({
        "$_internalFleBetween": {
            "field": "$age",
            "server": [
                {
                    "$binary": {
                        "base64": "CKPIq22z0nimnLE48v2/ZyeljOYmlDVlUYEWiCINEhII",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CGO26k3E2Qb5M5GlI1SoqcxUkXJbsCwHlwPwTbzesr53",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CJ0Xu3p6LfkwHkE1EAR0DLCWEx1SPlyLHL3kkNPlOaxz",
                        "subType": "6"
                    }
                }

            ]
        }
    })");
#define ASSERT_REWRITE_TO_INTERNAL_BETWEEN(T)                                          \
    {                                                                                  \
        auto input = generateOpWithFFP<T>("age", 23, 35);                              \
        auto result = _predicate.rewrite(input.get());                                 \
        ASSERT(result);                                                                \
        ASSERT_EQ(result->matchType(), MatchExpression::EXPRESSION);                   \
        auto* expr = static_cast<ExprMatchExpression*>(result.get());                  \
        auto aggExpr = expr->getExpression();                                          \
        ASSERT_BSONOBJ_EQ(aggExpr->serialize(false).getDocument().toBson(), expected); \
    }
    ASSERT_REWRITE_TO_INTERNAL_BETWEEN(GTMatchExpression);
    ASSERT_REWRITE_TO_INTERNAL_BETWEEN(GTEMatchExpression);
    ASSERT_REWRITE_TO_INTERNAL_BETWEEN(LTMatchExpression);
    ASSERT_REWRITE_TO_INTERNAL_BETWEEN(LTEMatchExpression);
}

TEST_F(RangePredicateRewriteTest, CollScanRewriteAgg) {
    _mock.setForceEncryptedCollScanForTest();
    auto expected = fromjson(R"({
        "$_internalFleBetween": {
            "field": "$age",
            "server": [
                {
                    "$binary": {
                        "base64": "CKPIq22z0nimnLE48v2/ZyeljOYmlDVlUYEWiCINEhII",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CGO26k3E2Qb5M5GlI1SoqcxUkXJbsCwHlwPwTbzesr53",
                        "subType": "6"
                    }
                },
                {
                    "$binary": {
                        "base64": "CJ0Xu3p6LfkwHkE1EAR0DLCWEx1SPlyLHL3kkNPlOaxz",
                        "subType": "6"
                    }
                }

            ]
        }
    })");
    auto ops = {ExpressionCompare::GT,
                ExpressionCompare::GTE,
                ExpressionCompare::LT,
                ExpressionCompare::LTE};
    for (auto& op : ops) {
        auto input = generateBetweenWithFFP(&_expCtx, op, "age", 23, 35);
        auto result = _predicate.rewrite(input.get());
        ASSERT(result);
        ASSERT_BSONOBJ_EQ(result->serialize(false).getDocument().toBson(), expected);
    }
}


TEST_F(RangePredicateRewriteTest, UnsupportedComparisonOps) {
    auto ops = {ExpressionCompare::CMP, ExpressionCompare::EQ, ExpressionCompare::NE};
    for (auto& op : ops) {
        auto input = generateBetweenWithFFP(&_expCtx, op, "age", 23, 35);
        auto result = _predicate.rewrite(input.get());
        ASSERT(result == nullptr);
    }
    _mock.setForceEncryptedCollScanForTest();
    for (auto& op : ops) {
        auto input = generateBetweenWithFFP(&_expCtx, op, "age", 23, 35);
        auto result = _predicate.rewrite(input.get());
        ASSERT(result == nullptr);
    }
}

};  // namespace
}  // namespace mongo::fle
