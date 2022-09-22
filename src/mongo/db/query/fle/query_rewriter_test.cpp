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
        return elt.Obj().firstElementFieldNameStringData() == "encrypt"_sd;
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
        return nullptr;
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
        return EncryptedBinDataType::kPlaceholder;  // return the 0 type. this isn't used anywhere.
    }
};

void setMockRewriteMaps(fle::MatchTypeToRewriteMap& match,
                        fle::ExpressionToRewriteMap& agg,
                        fle::TagMap& tags,
                        std::set<StringData>& encryptedFields) {
    match[MatchExpression::EQ] = [&](auto* rewriter, auto* expr) {
        return MockPredicateRewriter{rewriter}.rewrite(expr);
    };
}

class MockQueryRewriter : public fle::QueryRewriter {
public:
    MockQueryRewriter(fle::ExpressionToRewriteMap* exprRewrites,
                      fle::MatchTypeToRewriteMap* matchRewrites)
        : fle::QueryRewriter(new ExpressionContextForTest(), *exprRewrites, *matchRewrites) {
        setMockRewriteMaps(*matchRewrites, *exprRewrites, _tags, _encryptedFields);
    }

    BSONObj rewriteMatchExpressionForTest(const BSONObj& obj) {
        auto res = rewriteMatchExpression(obj);
        return res ? res.value() : obj;
    }

private:
    fle::TagMap _tags;
    std::set<StringData> _encryptedFields;
};

class FLEServerRewriteTest : public unittest::Test {
public:
    FLEServerRewriteTest() : _mock(nullptr) {}

    void setUp() override {
        _mock = std::make_unique<MockQueryRewriter>(&_agg, &_match);
    }

    void tearDown() override {}

protected:
    std::unique_ptr<MockQueryRewriter> _mock;
    fle::ExpressionToRewriteMap _agg;
    fle::MatchTypeToRewriteMap _match;
};

#define ASSERT_MATCH_EXPRESSION_REWRITE(input, expected)                 \
    auto actual = _mock->rewriteMatchExpressionForTest(fromjson(input)); \
    ASSERT_BSONOBJ_EQ(actual, fromjson(expected));

#define TEST_FLE_REWRITE_MATCH(name, input, expected)      \
    TEST_F(FLEServerRewriteTest, name##_MatchExpression) { \
        ASSERT_MATCH_EXPRESSION_REWRITE(input, expected);  \
    }

TEST_FLE_REWRITE_MATCH(TopLevel_DottedPath,
                       "{'user.ssn': {$eq: {encrypt: 2}}}",
                       "{'user.ssn': {$gt: 2}}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_BothEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {encrypt: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$gt: 4}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_PartlyEncrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {plain: 4}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$eq: {plain: 4}}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Conjunction_PartlyEncryptedWithUnregisteredOperator,
                       "{$and: [{ssn: {encrypt: 2}}, {age: {$lt: {encrypt: 4}}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {age: {$lt: {encrypt: 4}}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Encrypted_Nested_Unecrypted,
                       "{$and: [{ssn: {encrypt: 2}}, {user: {region: 'US'}}]}",
                       "{$and: [{ssn: {$gt: 2}}, {user: {$eq: {region: 'US'}}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Not,
                       "{ssn: {$not: {$eq: {encrypt: 5}}}}",
                       "{ssn: {$not: {$gt: 5}}}");

TEST_FLE_REWRITE_MATCH(TopLevel_Neq, "{ssn: {$ne: {encrypt: 5}}}", "{ssn: {$not: {$gt: 5}}}}");

TEST_FLE_REWRITE_MATCH(
    NestedConjunction,
    "{$and: [{$and: [{ssn: {encrypt: 2}}, {other: 'field'}]}, {otherSsn: {encrypt: 3}}]}",
    "{$and: [{$and: [{ssn: {$gt: 2}}, {other: {$eq: 'field'}}]}, {otherSsn: {$gt: 3}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Nor,
                       "{$nor: [{ssn: {encrypt: 5}}, {other: {$eq: 'field'}}]}",
                       "{$nor: [{ssn: {$gt: 5}}, {other: {$eq: 'field'}}]}");

TEST_FLE_REWRITE_MATCH(TopLevel_Or,
                       "{$or: [{ssn: {encrypt: 5}}, {other: {$eq: 'field'}}]}",
                       "{$or: [{ssn: {$gt: 5}}, {other: {$eq: 'field'}}]}");
}  // namespace
}  // namespace mongo
