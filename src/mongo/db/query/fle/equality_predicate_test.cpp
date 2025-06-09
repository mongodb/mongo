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

#include "mongo/db/query/fle/equality_predicate.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::fle {
namespace {
class MockEqualityPredicate : public EqualityPredicate {
public:
    MockEqualityPredicate(const QueryRewriterInterface* rewriter) : EqualityPredicate(rewriter) {}
    MockEqualityPredicate(const QueryRewriterInterface* rewriter,
                          TagMap tags,
                          std::set<StringData> encryptedFields)
        : EqualityPredicate(rewriter), _tags(tags), _encryptedFields(encryptedFields) {}

    void setEncryptedTags(std::pair<StringData, int> fieldvalue, std::vector<PrfBlock> tags) {
        _encryptedFields.insert(fieldvalue.first);
        _tags[fieldvalue] = tags;
        _exprTags[fieldvalue.second] = tags;
    }

    void addEncryptedField(StringData field) {
        _encryptedFields.insert(field);
    }


protected:
    bool isPayload(const BSONElement& elt) const override {
        return _encryptedFields.find(elt.fieldNameStringData()) != _encryptedFields.end();
    }

    bool isPayload(const Value& v) const override {
        // Consider it a payload if either 1) we have configured encrypted fields but no tags set (a
        // shortcut for tests that aren't bothering with tag generation) or 2) we have tags for this
        // value.
        return (!_encryptedFields.empty() && _exprTags.empty()) ||
            (v.numeric() && _exprTags.contains(v.coerceToInt()));
    }

    std::vector<PrfBlock> generateTags(BSONValue payload) const override {
        return visit(OverloadedVisitor{
                         [&](BSONElement p) {
                             ASSERT(p.isNumber());  // Only accept numbers as mock FFPs.
                             ASSERT(_tags.find({p.fieldNameStringData(), p.Int()}) != _tags.end());
                             return _tags.find({p.fieldNameStringData(), p.Int()})->second;
                         },
                         [&](std::reference_wrapper<Value> v) {
                             ASSERT(v.get().numeric());  // Only accept numbers as mock FFPs.
                             ASSERT(_exprTags.find(v.get().coerceToInt()) != _exprTags.end());
                             return _exprTags.find(v.get().coerceToInt())->second;
                         }},
                     payload);
    }

private:
    TagMap _tags;
    // Key the tags for agg expressions based on values only, since we don't have access to the
    // field name.
    std::map<int, std::vector<PrfBlock>> _exprTags;

    std::set<StringData> _encryptedFields;
};

class EqualityPredicateRewriteTest : public EncryptedPredicateRewriteTest {
public:
    EqualityPredicateRewriteTest() : _predicate(&_mock) {}

protected:
    MockEqualityPredicate _predicate;
};

std::unique_ptr<Expression> makeEqAggExpr(
    ExpressionContext* const expCtx,
    StringData path,
    Value value,
    ExpressionCompare::CmpOp comparisonType = ExpressionCompare::EQ) {
    auto valExpr = make_intrusive<ExpressionConstant>(expCtx, value);
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, std::string{path}, expCtx->variablesParseState);
    std::vector<boost::intrusive_ptr<Expression>> children = {std::move(fieldpath),
                                                              std::move(valExpr)};
    return std::make_unique<ExpressionCompare>(expCtx, comparisonType, std::move(children));
}

std::unique_ptr<Expression> makeInAggExpr(ExpressionContext* const expCtx,
                                          StringData path,
                                          std::vector<Value> values) {
    std::vector<boost::intrusive_ptr<Expression>> valExprs;
    for (auto& value : values) {
        auto valExpr = make_intrusive<ExpressionConstant>(expCtx, value);
        valExprs.emplace_back(std::move(valExpr));
    }
    auto valArray = make_intrusive<ExpressionArray>(expCtx, std::move(valExprs));
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, std::string{path}, expCtx->variablesParseState);
    std::vector<boost::intrusive_ptr<Expression>> children{std::move(fieldpath),
                                                           std::move(valArray)};
    return std::make_unique<ExpressionIn>(expCtx, std::move(children));
}

TEST_F(EqualityPredicateRewriteTest, Equality_NoFFP_Match) {
    std::unique_ptr<MatchExpression> input =
        std::make_unique<EqualityMatchExpression>("ssn"_sd, Value(5));
    auto expected = EqualityMatchExpression("ssn"_sd, Value(5));

    auto result = _predicate.rewrite(input.get());
    ASSERT(result == nullptr);
    ASSERT(input->equivalent(&expected));
}

TEST_F(EqualityPredicateRewriteTest, Equality_NoFFP_Expr) {
    std::unique_ptr<Expression> input =
        makeEqAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value(5));
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);

    input = makeEqAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value(5), ExpressionCompare::NE);
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(EqualityPredicateRewriteTest, In_NoFFP_Match) {
    auto input = makeInExpr("name",
                            BSON_ARRAY("harry" << "ron"
                                               << "hermione"));
    auto expected = makeInExpr("name",
                               BSON_ARRAY("harry" << "ron"
                                                  << "hermione"));

    auto result = _predicate.rewrite(input.get());
    ASSERT(result == nullptr);
    ASSERT(input->equivalent(expected.get()));
}

TEST_F(EqualityPredicateRewriteTest, In_NoFFP_Expr) {
    auto input = makeInAggExpr(_mock.getExpressionContext(),
                               "name",
                               {Value("harry"_sd), Value("ron"_sd), Value("hermione"_sd)});
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(EqualityPredicateRewriteTest, Equality_Basic_Match) {
    auto input = EqualityMatchExpression("ssn"_sd, Value(5));
    std::vector<PrfBlock> tags = {{1}, {2}, {3}};

    _predicate.setEncryptedTags({"ssn", 5}, tags);

    assertRewriteToTags(_predicate, &input, toBSONArray(std::move(tags)));
}

TEST_F(EqualityPredicateRewriteTest, Equality_Basic_Expr) {
    auto input = makeEqAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value(5));
    std::vector<PrfBlock> tags = {{1}, {2}};

    _predicate.setEncryptedTags({"ssn", 5}, tags);

    auto actual = _predicate.rewrite(input.get());
    auto actualBson = actual->serialize().getDocument().toBson();

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$or": [
                {
                    "$in": [
                        {
                            "$const": {"$binary":{"base64":"AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","subType":"0"}}
                        },
                        "$__safeContent__"
                    ]
                },
                {
                    "$in": [
                        {
                            "$const": {"$binary":{"base64":"AgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","subType":"0"}}
                        },
                        "$__safeContent__"
                    ]
                }
            ]
        })",
        actualBson);

    // Test the NE operator as well. The rewritten predicate should be the same as the equality
    // predicate, but negated.
    input = makeEqAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value(5), ExpressionCompare::NE);
    auto neActual = _predicate.rewrite(input.get())->serialize().getDocument();
    ASSERT(!neActual.getField("$not").missing() && neActual["$not"].isArray());
    ASSERT_EQ(neActual["$not"].getArrayLength(), 1);
    ASSERT_BSONOBJ_EQ(neActual["$not"][0].getDocument().toBson(), actualBson);
}

TEST_F(EqualityPredicateRewriteTest, In_Basic_Match) {
    auto input = makeInExpr("ssn", BSON_ARRAY(2 << 4 << 6));

    _predicate.setEncryptedTags({"0", 2}, {{1}, {2}});
    _predicate.setEncryptedTags({"1", 4}, {{5}, {3}});
    _predicate.setEncryptedTags({"2", 6}, {{99}, {100}});

    assertRewriteToTags(_predicate, input.get(), toBSONArray({{1}, {2}, {3}, {5}, {99}, {100}}));
}

TEST_F(EqualityPredicateRewriteTest, In_Basic_Expr) {
    auto input = makeInAggExpr(_mock.getExpressionContext(), "ssn", {Value(2), Value(4)});

    _predicate.setEncryptedTags({"0", 2}, {{1}, {2}});
    _predicate.setEncryptedTags({"1", 4}, {{5}, {3}});

    auto actual = _predicate.rewrite(input.get());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$or": [
                {
                    "$in": [
                        {
                            "$const": {"$binary":{"base64":"AQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","subType":"0"}}
                        },
                        "$__safeContent__"
                    ]
                },
                {
                    "$in": [
                        {
                            "$const": {"$binary":{"base64":"AgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","subType":"0"}}
                        },
                        "$__safeContent__"
                    ]
                },
                {
                    "$in": [
                        {
                            "$const": {"$binary":{"base64":"BQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","subType":"0"}}
                        },
                        "$__safeContent__"
                    ]
                },
                {
                    "$in": [
                        {
                            "$const": {"$binary":{"base64":"AwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","subType":"0"}}
                        },
                        "$__safeContent__"
                    ]
                }
            ]
        })",
        actual->serialize().getDocument().toBson());
}

TEST_F(EqualityPredicateRewriteTest, In_NotAllFFPs_Match) {
    auto input = makeInExpr("ssn", BSON_ARRAY(2 << 4 << 6));

    _predicate.setEncryptedTags({"0", 2}, {{1}, {2}});
    _predicate.setEncryptedTags({"1", 4}, {{5}, {3}});

    ASSERT_THROWS_CODE(
        assertRewriteToTags(_predicate, input.get(), toBSONArray({{1}, {2}, {3}, {5}})),
        AssertionException,
        6329400);
}

TEST_F(EqualityPredicateRewriteTest, In_NotAllFFPs_Expr) {
    auto input = makeInAggExpr(_mock.getExpressionContext(), "ssn", {Value(2), Value(4), Value(6)});

    _predicate.setEncryptedTags({"0", 2}, {{1}, {2}});
    _predicate.setEncryptedTags({"1", 4}, {{5}, {3}});

    ASSERT_THROWS_CODE(_predicate.rewrite(input.get()), AssertionException, 6334102);
}

BSONObj generateFFP(StringData path, int value) {
    auto indexKey = getIndexKey();
    FLEIndexKeyAndId indexKeyAndId(indexKey.data, indexKeyId);
    auto userKey = getUserKey();
    FLEUserKeyAndId userKeyAndId(userKey.data, indexKeyId);

    BSONObj doc = BSON("value" << value);
    auto element = doc.firstElement();
    auto fpp = FLEClientCrypto::serializeFindPayloadV2(indexKeyAndId, userKeyAndId, element, 0);

    BSONObjBuilder builder;
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindEqualityPayloadV2, fpp, &builder);
    return builder.obj();
}

std::unique_ptr<MatchExpression> generateEqualityWithFFP(StringData path, BSONObj ffp) {
    return std::make_unique<EqualityMatchExpression>(path, ffp.firstElement());
}

std::unique_ptr<Expression> generateEqualityWithFFP(
    ExpressionContext* const expCtx,
    StringData path,
    int value,
    ExpressionCompare::CmpOp comparisonType = ExpressionCompare::EQ) {
    auto ffp = Value(generateFFP(path, value).firstElement());
    return makeEqAggExpr(expCtx, path, ffp, comparisonType);
}

std::unique_ptr<MatchExpression> generateDisjunctionWithFFP(StringData path,
                                                            std::initializer_list<int> vals) {
    BSONArrayBuilder bab;
    for (auto& value : vals) {
        bab.append(generateFFP(path, value).firstElement());
    }
    auto arr = bab.arr();
    return EncryptedPredicateRewriteTest::makeInExpr(path, arr);
}

std::unique_ptr<Expression> generateDisjunctionWithFFP(ExpressionContext* const expCtx,
                                                       StringData path,
                                                       std::initializer_list<int> values) {
    std::vector<Value> ffps;
    for (auto& value : values) {
        ffps.emplace_back(Value(generateFFP(path, value).firstElement()));
    }
    return makeInAggExpr(expCtx, path, ffps);
}

class EqualityPredicateCollScanRewriteTest : public EncryptedPredicateRewriteTest {
public:
    EqualityPredicateCollScanRewriteTest() : _predicate(&_mock) {
        _mock.setForceEncryptedCollScanForTest();
    }

protected:
    MockEqualityPredicate _predicate;
};

TEST_F(EqualityPredicateCollScanRewriteTest, Eq_Match) {
    auto path = "ssn";
    BSONObj ffp = generateFFP(path, 1);
    auto input = generateEqualityWithFFP(path, ffp);
    _predicate.addEncryptedField(path);

    auto result = _predicate.rewrite(input.get());

    ASSERT(result);
    ASSERT_EQ(result->matchType(), MatchExpression::EXPRESSION);
    auto* expr = static_cast<ExprMatchExpression*>(result.get());
    auto aggExpr = expr->getExpression();

    auto expected = fromjson(R"({
        "$_internalFleEq": {
            "field": "$ssn",
            "server": {
                "$binary": {
                    "base64": "CCL2XXIiI5N4wjgvCxYp6XRXY1OS39OuX6WT6Y60cR9R",
                    "subType": "6"
                }
            }
        }
    })");
    ASSERT_BSONOBJ_EQ(aggExpr->serialize().getDocument().toBson(), expected);
}

TEST_F(EqualityPredicateCollScanRewriteTest, Eq_Expr) {
    auto expCtx = _mock.getExpressionContext();
    auto input = generateEqualityWithFFP(expCtx, "ssn", 1);
    _predicate.addEncryptedField("ssn");

    auto result = _predicate.rewrite(input.get());

    auto expected = fromjson(R"({
        "$_internalFleEq": {
            "field": "$ssn",
            "server": {
                "$binary": {
                    "base64": "CCL2XXIiI5N4wjgvCxYp6XRXY1OS39OuX6WT6Y60cR9R",
                    "subType": "6"
                }
            }
        }
    })");
    ASSERT(result);
    ASSERT_BSONOBJ_EQ(result->serialize().getDocument().toBson(), expected);

    // Test the NE operator as well. The rewritten predicate should be the same as the equality
    // predicate, but negated.
    input = generateEqualityWithFFP(expCtx, "ssn", 1, ExpressionCompare::NE);
    auto neResult = _predicate.rewrite(input.get());
    ASSERT(neResult);

    auto neResultDoc = neResult->serialize().getDocument();
    ASSERT(!neResultDoc.getField("$not").missing() && neResultDoc["$not"].isArray());
    ASSERT_EQ(neResultDoc["$not"].getArrayLength(), 1);
    ASSERT_BSONOBJ_EQ(neResultDoc["$not"][0].getDocument().toBson(), expected);
}

TEST_F(EqualityPredicateCollScanRewriteTest, In_Match) {
    auto input = generateDisjunctionWithFFP("ssn", {1, 2, 3});
    _predicate.addEncryptedField("0");
    _predicate.addEncryptedField("1");
    _predicate.addEncryptedField("2");

    auto result = _predicate.rewrite(input.get());

    ASSERT(result);
    ASSERT_EQ(result->matchType(), MatchExpression::OR);
    auto expected = fromjson(R"({
    "$or": [
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "server": {
                        "$binary": {
                            "base64": "CCL2XXIiI5N4wjgvCxYp6XRXY1OS39OuX6WT6Y60cR9R",
                            "subType": "6"
                        }
                    }
                }
            }
        },
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "server": {
                        "$binary": {
                            "base64": "CB94z16OvvvDIOA1rjeDCWnhyhfjroyqXV8k/3uUkEmN",
                            "subType": "6"
                        }
                    }
                }
            }
        },
        {
            "$expr": {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "server": {
                        "$binary": {
                            "base64": "CIGY0H2DhM4LFcdH2RPZcvYuKE1Mu2d6rfJ4IMlfRd2r",
                            "subType": "6"
                        }
                    }
                }
            }
        }
    ]
})");
    ASSERT_BSONOBJ_EQ(result->serialize(), expected);
}

TEST_F(EqualityPredicateCollScanRewriteTest, In_Expr) {
    auto input = generateDisjunctionWithFFP(_mock.getExpressionContext(), "ssn", {1, 1});
    _predicate.addEncryptedField("0");
    _predicate.addEncryptedField("1");
    _predicate.addEncryptedField("2");

    auto result = _predicate.rewrite(input.get());

    ASSERT(result);
    auto expected = fromjson(R"({ "$or" : [ {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "server": {
                        "$binary": {
                            "base64": "CCL2XXIiI5N4wjgvCxYp6XRXY1OS39OuX6WT6Y60cR9R",
                            "subType": "6"
                        }
                    }
                }},
                {
                "$_internalFleEq": {
                    "field": "$ssn",
                    "server": {
                        "$binary": {
                            "base64": "CCL2XXIiI5N4wjgvCxYp6XRXY1OS39OuX6WT6Y60cR9R",
                            "subType": "6"
                        }
                    }
                }}
            ]})");
    ASSERT_BSONOBJ_EQ(result->serialize().getDocument().toBson(), expected);
}

}  // namespace
}  // namespace mongo::fle
