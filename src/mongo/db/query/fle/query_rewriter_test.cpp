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


#include <memory>

#include "query_rewriter.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/overloaded_visitor.h"


namespace mongo {
namespace {

/*
 *  The server rewrite itself is only responsible for traversing agg and MatchExpressions and
 *  executing whatever rewrites are registered. For unit testing, we will only verify that this
 *  traversal and rewrite is happening properly using a mock predicate rewriter that rewrites any
 *  equality with an object with the key `encrypt` to a $gt operator. Unit tests for the actual
 *  rewrites while mocking out tag generation are located in the test file for each encrypted
 *  predicate type. Full end-to-end testing happens in jstests. This organization ensures that we
 *  don't write redundant tests that each index type is properly rewritten under different
 *  circumstances, when the same exact code is called for each index type.
 */

class MockPredicateRewriter : public fle::EncryptedPredicate {
public:
    MockPredicateRewriter(const fle::QueryRewriterInterface* rewriter)
        : EncryptedPredicate(rewriter) {}

protected:
    bool isPayload(const BSONElement& elt) const override {
        if (!elt.isABSONObj()) {
            return false;
        }
        return elt.Obj().hasField("encrypt"_sd);
    }
    bool isPayload(const Value& v) const override {
        if (!v.isObject()) {
            return false;
        }
        return !v.getDocument().getField("encrypt").missing();
    }

    std::vector<PrfBlock> generateTags(fle::BSONValue payload) const override {
        return {};
    };

    // Encrypted values will be rewritten from $eq to $gt. This is an arbitrary decision just to
    // make sure that the rewrite works properly.
    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override {
        invariant(expr->matchType() == MatchExpression::EQ);
        auto eqMatch = static_cast<EqualityMatchExpression*>(expr);
        if (!isPayload(eqMatch->getData())) {
            return nullptr;
        }
        return std::make_unique<GTMatchExpression>(eqMatch->path(),
                                                   eqMatch->getData().Obj().firstElement());
    };

    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override {
        auto eqMatch = dynamic_cast<ExpressionCompare*>(expr);
        invariant(eqMatch);
        // Only operate over equality comparisons.
        if (eqMatch->getOp() != ExpressionCompare::EQ) {
            return nullptr;
        }
        auto payload = dynamic_cast<ExpressionConstant*>(eqMatch->getOperandList()[1].get());
        // If the comparison doesn't hold a constant, then don't rewrite.
        if (!payload) {
            return nullptr;
        }

        // If the constant is not considered a payload, then don't rewrite.
        if (!isPayload(payload->getValue())) {
            return nullptr;
        }
        auto cmp = std::make_unique<ExpressionCompare>(eqMatch->getExpressionContext(),
                                                       ExpressionCompare::GT);
        cmp->addOperand(eqMatch->getOperandList()[0]);
        cmp->addOperand(
            ExpressionConstant::create(eqMatch->getExpressionContext(),
                                       payload->getValue().getDocument().getField("encrypt")));
        return cmp;
    }

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override {
        return nullptr;
    }

    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override {
        return nullptr;
    }

private:
    // This method is not used in mock implementations of the EncryptedPredicate since isPayload(),
    // which normally calls encryptedBinDataType(), is overridden to look for plain objects rather
    // than BinData. Since this method is pure virtual on the superclass and needs to be
    // implemented, it is set to kPlaceholder (0).
    EncryptedBinDataType encryptedBinDataType() const override {
        return EncryptedBinDataType::kPlaceholder;
    }
};

// A second mock rewrite which replaces documents with the key "foo" into $lt operations. We need
// two different rewrites that are registered on the same operator to verify that all rewrites are
// iterated through.
class OtherMockPredicateRewriter : public fle::EncryptedPredicate {
public:
    OtherMockPredicateRewriter(const fle::QueryRewriterInterface* rewriter)
        : EncryptedPredicate(rewriter) {}

protected:
    bool isPayload(const BSONElement& elt) const override {
        if (!elt.isABSONObj()) {
            return false;
        }
        return elt.Obj().hasField("foo"_sd);
    }
    bool isPayload(const Value& v) const override {
        if (!v.isObject()) {
            return false;
        }
        return !v.getDocument().getField("foo").missing();
    }

    std::vector<PrfBlock> generateTags(fle::BSONValue payload) const override {
        return {};
    };

    // Encrypted values will be rewritten from $eq to $lt. This is an arbitrary decision just to
    // make sure that the rewrite works properly.
    std::unique_ptr<MatchExpression> rewriteToTagDisjunction(MatchExpression* expr) const override {
        invariant(expr->matchType() == MatchExpression::EQ);
        auto eqMatch = static_cast<EqualityMatchExpression*>(expr);
        if (!isPayload(eqMatch->getData())) {
            return nullptr;
        }
        return std::make_unique<LTMatchExpression>(eqMatch->path(),
                                                   eqMatch->getData().Obj().firstElement());
    };

    std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const override {
        auto eqMatch = dynamic_cast<ExpressionCompare*>(expr);
        invariant(eqMatch);
        if (eqMatch->getOp() != ExpressionCompare::EQ) {
            return nullptr;
        }
        auto payload = dynamic_cast<ExpressionConstant*>(eqMatch->getOperandList()[1].get());
        if (!payload) {
            return nullptr;
        }

        if (!isPayload(payload->getValue())) {
            return nullptr;
        }
        auto cmp = std::make_unique<ExpressionCompare>(eqMatch->getExpressionContext(),
                                                       ExpressionCompare::LT);
        cmp->addOperand(eqMatch->getOperandList()[0]);
        cmp->addOperand(ExpressionConstant::create(
            eqMatch->getExpressionContext(), payload->getValue().getDocument().getField("foo")));
        return cmp;
    }

    std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const override {
        return nullptr;
    }

    std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const override {
        return nullptr;
    }

private:
    EncryptedBinDataType encryptedBinDataType() const override {
        return EncryptedBinDataType::kPlaceholder;
    }
};

void setMockRewriteMaps(fle::MatchTypeToRewriteMap& match,
                        fle::ExpressionToRewriteMap& agg,
                        fle::TagMap& tags,
                        std::set<StringData>& encryptedFields) {
    match[MatchExpression::EQ] = {
        [&](auto* rewriter, auto* expr) { return MockPredicateRewriter{rewriter}.rewrite(expr); },
        [&](auto* rewriter, auto* expr) {
            return OtherMockPredicateRewriter{rewriter}.rewrite(expr);
        },
    };
    agg[typeid(ExpressionCompare)] = {
        [&](auto* rewriter, auto* expr) { return MockPredicateRewriter{rewriter}.rewrite(expr); },
        [&](auto* rewriter, auto* expr) {
            return OtherMockPredicateRewriter{rewriter}.rewrite(expr);
        },
    };
}

class MockQueryRewriter : public fle::QueryRewriter {
public:
    MockQueryRewriter(fle::ExpressionToRewriteMap* exprRewrites,
                      fle::MatchTypeToRewriteMap* matchRewrites,
                      const NamespaceString& mockNss)
        : fle::QueryRewriter(
              new ExpressionContextForTest(), mockNss, *exprRewrites, *matchRewrites) {
        setMockRewriteMaps(*matchRewrites, *exprRewrites, _tags, _encryptedFields);
    }

    BSONObj rewriteMatchExpressionForTest(const BSONObj& obj) {
        auto res = rewriteMatchExpression(obj);
        return res ? res.value() : obj;
    }

    BSONObj rewriteAggExpressionForTest(const BSONObj& obj) {
        auto expr = Expression::parseExpression(&_expCtx, obj, _expCtx.variablesParseState);
        auto result = rewriteExpression(expr.get());
        return result ? result->serialize(false).getDocument().toBson()
                      : expr->serialize(false).getDocument().toBson();
    }

private:
    fle::TagMap _tags;
    std::set<StringData> _encryptedFields;
    ExpressionContextForTest _expCtx;
};

class FLEServerRewriteTest : public unittest::Test {
public:
    FLEServerRewriteTest() : _mock(nullptr) {}

    void setUp() override {
        _mock = std::make_unique<MockQueryRewriter>(&_agg, &_match, _mockNss);
    }

    void tearDown() override {}

protected:
    std::unique_ptr<MockQueryRewriter> _mock;
    fle::ExpressionToRewriteMap _agg;
    fle::MatchTypeToRewriteMap _match;
    NamespaceString _mockNss{"mock"_sd};
};

#define ASSERT_MATCH_EXPRESSION_REWRITE(input, expected)                 \
    auto actual = _mock->rewriteMatchExpressionForTest(fromjson(input)); \
    ASSERT_BSONOBJ_EQ(actual, fromjson(expected));

#define TEST_FLE_REWRITE_MATCH(name, input, expected)      \
    TEST_F(FLEServerRewriteTest, name##_MatchExpression) { \
        ASSERT_MATCH_EXPRESSION_REWRITE(input, expected);  \
    }

#define ASSERT_AGG_EXPRESSION_REWRITE(input, expected)                 \
    auto actual = _mock->rewriteAggExpressionForTest(fromjson(input)); \
    ASSERT_BSONOBJ_EQ(actual, fromjson(expected));

#define TEST_FLE_REWRITE_AGG(name, input, expected)      \
    TEST_F(FLEServerRewriteTest, name##_AggExpression) { \
        ASSERT_AGG_EXPRESSION_REWRITE(input, expected);  \
    }

TEST_FLE_REWRITE_MATCH(TopLevel_DottedPath,
                       "{'user.ssn': {$eq: {encrypt: 2}}}",
                       "{'user.ssn': {$gt: 2}}");

TEST_FLE_REWRITE_AGG(TopLevel_DottedPath,
                     "{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}",
                     "{$gt: ['$user.ssn', {$const: 2}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_BothEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {encrypt: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$gt: 4}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Conjunction_BothEncrypted,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$eq: ['$age', {$const: {encrypt: "
    "4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$gt: ['$age', {$const: 4}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_PartlyEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {plain: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$eq: {plain: 4}}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Conjunction_PartlyEncrypted,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$eq: ['$age', {$const: {plain: 4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$eq: ['$age', {$const: {plain: 4}}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_PartlyEncryptedWithUnregisteredOperator,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {$lt: {encrypt: 4}}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$lt: {encrypt: 4}}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Conjunction_PartlyEncryptedWithUnregisteredOperator,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$lt: ['$age', {$const: {encrypt: "
    "4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$lt: ['$age', {$const: {encrypt: "
    "4}}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Encrypted_Nested_Unecrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {user: {region: 'US'}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {user: {$eq: {region: 'US'}}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Not,
                       "{ssn: {$not: {$eq: {encrypt: 5}}}}",
                       "{ssn: {$not: {$gt: 5}}}");

TEST_FLE_REWRITE_AGG(TopLevel_Not,
                     "{$not: [{$eq: ['$ssn', {$const: {encrypt: 2}}]}]}",
                     "{$not: [{$gt: ['$ssn', {$const: 2}]}]}")

TEST_FLE_REWRITE_MATCH(TopLevel_Neq, "{ssn: {$ne: {encrypt: 5}}}", "{ssn: {$not: {$gt: 5}}}}");

TEST_FLE_REWRITE_MATCH(
    NestedConjunction,
    "{$and: [{$and: [{ssn: {encrypt: 2}}, {other: 'field'}]}, {otherSsn: {encrypt: 3}}]}",
    "{$and: [{$and: [{ssn: {$gt: 2}}, {other: {$eq: 'field'}}]}, {otherSsn: {$gt: 3}}]}");

TEST_FLE_REWRITE_AGG(NestedConjunction,
                     "{$and: [{$and: [{$eq: ['$ssn', {$const: {encrypt: 2}}]},{$eq: ['$other', "
                     "'field']}]},{$eq: ['$age',{$const: {encrypt: 4}}]}]}",
                     "{$and: [{$and: [{$gt: ['$ssn', {$const: 2}]},{$eq: ['$other', "
                     "{$const: 'field'}]}]},{$gt: ['$age',{$const: 4}]}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Nor,
                       "{$nor: [{ssn: {encrypt: 5}}, {other: {$eq: 'field'}}]}",
                       "{$nor: [{ssn: {$gt: 5}}, {other: {$eq: 'field'}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Or,
                       "{$or: [{ssn: {encrypt: 5}}, {other: {$eq: 'field'}}]}",
                       "{$or: [{ssn: {$gt: 5}}, {other: {$eq: 'field'}}]}");

TEST_FLE_REWRITE_AGG(
    TopLevel_Or,
    "{$or: [{$eq: ['$ssn', {$const: {encrypt: 2}}]}, {$eq: ['$ssn', {$const: {encrypt: 4}}]}]}",
    "{$or: [{$gt: ['$ssn', {$const: 2}]}, {$gt: ['$ssn', {$const: 4}]}]}")


// Test that the rewriter will work from any rewrite registered to an expression. The test rewriter
// has two rewrites registered on $eq.

TEST_FLE_REWRITE_MATCH(OtherRewrite_Basic, "{'ssn': {$eq: {foo: 2}}}", "{'ssn': {$lt: 2}}");

TEST_FLE_REWRITE_AGG(OtherRewrite_Basic,
                     "{$eq: ['$user.ssn', {$const: {foo: 2}}]}",
                     "{$lt: ['$user.ssn', {$const: 2}]}");

TEST_FLE_REWRITE_MATCH(OtherRewrite_Conjunction_BothEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {foo: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$lt: 4}}]}");

TEST_FLE_REWRITE_AGG(
    OtherRewrite_Conjunction_BothEncrypted,
    "{$and: [{$eq: ['$user.ssn', {$const: {encrypt: 2}}]}, {$eq: ['$age', {$const: {foo: "
    "4}}]}]}",
    "{$and: [{$gt: ['$user.ssn', {$const: 2}]}, {$lt: ['$age', {$const: 4}]}]}");
}  // namespace
}  // namespace mongo
