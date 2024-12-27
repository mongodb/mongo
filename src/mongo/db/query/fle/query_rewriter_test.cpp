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
#include <set>
#include <typeindex>
#include <vector>

#include "query_rewriter.h"
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/fle/encrypted_predicate.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/db/query/fle/server_rewrite_helper.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

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

// Define the mock match and agg rewrite maps to be used by the unit tests.
static const fle::MatchTypeToRewriteMap matchRewriteMap{
    {MatchExpression::EQ,
     {[](auto* rewriter, auto* expr) { return MockPredicateRewriter{rewriter}.rewrite(expr); },
      [](auto* rewriter, auto* expr) {
          return OtherMockPredicateRewriter{rewriter}.rewrite(expr);
      }}}};

static const fle::ExpressionToRewriteMap aggRewriteMap{
    {typeid(ExpressionCompare),
     {[](auto* rewriter, auto* expr) { return MockPredicateRewriter{rewriter}.rewrite(expr); },
      [](auto* rewriter, auto* expr) {
          return OtherMockPredicateRewriter{rewriter}.rewrite(expr);
      }}}};


class MockQueryRewriter : public fle::QueryRewriter {
public:
    MockQueryRewriter(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      const NamespaceString& mockNss,
                      const std::map<NamespaceString, NamespaceString>& escMap)
        : fle::QueryRewriter(expCtx, mockNss, aggRewriteMap, matchRewriteMap, escMap) {}

    BSONObj rewriteMatchExpressionForTest(const BSONObj& obj) {
        auto res = rewriteMatchExpression(obj);
        return res ? res.value() : obj;
    }

    BSONObj rewriteAggExpressionForTest(const BSONObj& obj) {
        tassert(9775503, "Invalid expression context", getExpressionContext());
        auto expr = Expression::parseExpression(
            getExpressionContext(), obj, getExpressionContext()->variablesParseState);
        auto result = rewriteExpression(expr.get());
        return result ? result->serialize().getDocument().toBson()
                      : expr->serialize().getDocument().toBson();
    }

    static fle::QueryRewriter getQueryRewriterWithMockedMaps(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        const std::map<NamespaceString, NamespaceString>& escMap) {
        // Workaround for protected fle::QueryRewriter constructor. Slices the mocked object,
        // leaving us with the copied base class with mocked maps.
        return MockQueryRewriter(expCtx, nss, escMap);
    }

private:
    fle::TagMap _tags;
    std::set<StringData> _encryptedFields;
};

class FLEServerRewriteTest : public unittest::Test {
public:
    FLEServerRewriteTest() : _mock(nullptr) {}

    void setUp() override {
        _mock = std::make_unique<MockQueryRewriter>(_expCtx, _mockNss, _mockEscMap);
    }

    void tearDown() override {}

protected:
    std::unique_ptr<MockQueryRewriter> _mock;
    boost::intrusive_ptr<ExpressionContext> _expCtx{new ExpressionContextForTest()};
    NamespaceString _mockNss = NamespaceString::createNamespaceString_forTest("test.mock"_sd);
    std::map<NamespaceString, NamespaceString> _mockEscMap{{_mockNss, _mockNss}};
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

class MockPipelineRewrite : public fle::PipelineRewrite {
public:
    MockPipelineRewrite(const NamespaceString& nss,
                        const EncryptionInformation& encryptInfo,
                        std::unique_ptr<Pipeline, PipelineDeleter> toRewrite)
        : PipelineRewrite(nss, encryptInfo, std::move(toRewrite)) {}

    ~MockPipelineRewrite() override{};

protected:
    fle::QueryRewriter getQueryRewriterForEsc(FLETagQueryInterface* queryImpl) override {
        return MockQueryRewriter::getQueryRewriterWithMockedMaps(expCtx, nssEsc, _escMap);
    }
};


class FLEServerRewritePipelineTest : public unittest::Test {
public:
    static constexpr auto kSingleEncryptionSchemaEncryptionInfo = R"({
                    "type": 1,
                    "schema":{
                        "test.coll_a": {
                            "escCollection": "enxcol_.coll_a.esc",
                            "ecocCollection": "enxcol_.coll_a.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789012"
                                    },
                                    "path": "ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789013"
                                    },
                                    "path": "age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        }
                    }   
                })";
    static constexpr auto kTwoEncryptionSchemaEncryptionInfo = R"({
                    "type": 1,
                    "schema":{
                        "test.coll_a": {
                            "escCollection": "enxcol_.coll_a.esc",
                            "ecocCollection": "enxcol_.coll_a.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789012"
                                    },
                                    "path": "ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789013"
                                    },
                                    "path": "age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        },
                        "test.coll_b": {
                            "escCollection": "enxcol_.coll_b.esc",
                            "ecocCollection": "enxcol_.coll_b.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789014"
                                    },
                                    "path": "b_ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789015"
                                    },
                                    "path": "b_age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        }
                    }   
                })";
    static constexpr auto kThreeEncryptionSchemaEncryptionInfo = R"({
                    "type": 1,
                    "schema":{
                        "test.coll_a": {
                            "escCollection": "enxcol_.coll_a.esc",
                            "ecocCollection": "enxcol_.coll_a.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789012"
                                    },
                                    "path": "ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789013"
                                    },
                                    "path": "age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        },
                        "test.coll_b": {
                            "escCollection": "enxcol_.coll_b.esc",
                            "ecocCollection": "enxcol_.coll_b.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789014"
                                    },
                                    "path": "b_ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789015"
                                    },
                                    "path": "b_age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        },
                        "test.coll_c": {
                            "escCollection": "enxcol_.coll_c.esc",
                            "ecocCollection": "enxcol_.coll_c.ecoc",
                            "fields": [
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789016"
                                    },
                                    "path": "c_ssn",
                                    "bsonType": "string",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                },
                                {
                                    "keyId": {
                                        "$uuid": "12345678-1234-9876-1234-123456789017"
                                    },
                                    "path": "c_age",
                                    "bsonType": "int",
                                    "queries": {
                                        "queryType": "equality"
                                    }
                                }
                            ]
                        }
                    }   
                })";

    void setResolvedNamespacesForTest(const std::vector<NamespaceString>& additionalNs) {
        StringMap<ResolvedNamespace> resolvedNs;
        resolvedNs.insert_or_assign(_primaryNss.coll().toString(),
                                    {_primaryNss, std::vector<BSONObj>{}});
        for (auto&& ns : additionalNs) {
            resolvedNs.insert_or_assign(ns.coll().toString(), {ns, std::vector<BSONObj>{}});
        }
        _expCtx->setResolvedNamespaces(std::move(resolvedNs));
    }

    auto jsonToPipeline(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const NamespaceString& nss,
                        StringData jsonArray) {
        const auto inputBson = fromjson("{pipeline: " + jsonArray + "}");

        ASSERT_EQUALS(inputBson["pipeline"].type(), BSONType::Array);
        auto rawPipeline = parsePipelineFromBSON(inputBson["pipeline"]);
        expCtx->setNamespaceString(nss);
        return Pipeline::parse(rawPipeline, expCtx);
    }

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx{new ExpressionContextForTest()};
    NamespaceString _primaryNss = NamespaceString::createNamespaceString_forTest("test.coll_a"_sd);
};

static inline void assertPipelinesSame(const std::vector<BSONObj>& expectedPipeline,
                                       const std::vector<BSONObj>& actualPipeline) {
    ASSERT_EQUALS(expectedPipeline.size(), actualPipeline.size());

    auto flags =
        BSONObj::ComparisonRules::kIgnoreFieldOrder | BSONObj::ComparisonRules::kConsiderFieldName;
    for (size_t i = 0; i < actualPipeline.size(); ++i) {
        ASSERT_TRUE(actualPipeline[i].woCompare(expectedPipeline[i], {}, flags) == 0);
    }
}

#define TEST_FLE_REWRITE_PIPELINE(name,                                                        \
                                  input,                                                       \
                                  expected,                                                    \
                                  additionalNamespaces,                                        \
                                  encryptionInformation,                                       \
                                  enableMultiSchemaFeatureFlag)                                \
    TEST_F(FLEServerRewritePipelineTest, name##_PipelineRewrite) {                             \
        RAIIServerParameterControllerForTest _scopedFeature{                                   \
            "featureFlagLookupEncryptionSchemasFLE", enableMultiSchemaFeatureFlag};            \
        setResolvedNamespacesForTest(additionalNamespaces);                                    \
        auto pipeline = jsonToPipeline(_expCtx, _primaryNss, input);                           \
        auto pipelineRewrite =                                                                 \
            MockPipelineRewrite(_primaryNss,                                                   \
                                EncryptionInformation::parse(IDLParserContext("root"),         \
                                                             fromjson(encryptionInformation)), \
                                std::move(pipeline));                                          \
        pipelineRewrite.doRewrite(nullptr);                                                    \
        auto rewrittenPipeline = pipelineRewrite.getPipeline();                                \
        ASSERT(rewrittenPipeline);                                                             \
        SerializationOptions opts{.serializeForFLE2 = true};                                   \
        auto serializedRewrittenPipeline = rewrittenPipeline->serializeToBson(opts);           \
        auto expectedPipeline =                                                                \
            jsonToPipeline(_expCtx, _primaryNss, expected)->serializeToBson(opts);             \
        assertPipelinesSame(expectedPipeline, serializedRewrittenPipeline);                    \
    }

TEST_FLE_REWRITE_PIPELINE(Match,
                          "[{$match: {$and: [{ssn: {encrypt: 2}}, {age: {encrypt: 4}}]}}]",
                          "[{$match: {$and: [{ssn: {$gt: 2}}, {age: {$gt: 4}}]}}]",
                          {},
                          FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
                          true);

TEST_FLE_REWRITE_PIPELINE(
    ProjectWithMatch,
    "[{$project: {foo: '$ssn'}},{$match: {$and: [{foo: {encrypt: 2}}, {age: {encrypt: 4}}]}}]",
    "[{$project: {foo: '$ssn'}},{$match: {$and: [{foo: {$gt: 2}}, {age: {$gt: 4}}]}}]",
    {},
    FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
    true);

TEST_FLE_REWRITE_PIPELINE(GraphLookup,
                          "[{ $graphLookup: {  \
                                from: \"coll_a\",\
                                as: \"selfGraph\",\
                                connectToField: \"name\",\
                                connectFromField: \"reportsTo\", \
                                startWith: \"$reportsTo\", \
                                restrictSearchWithMatch: { \
                                     \"ssn\" : { encrypt : 1234 } }}}]",
                          "[{ $graphLookup: { \
                                from : \"coll_a\", \
                                as : \"selfGraph\",\
                                connectToField : \"name\", \
                                connectFromField : \"reportsTo\", \
                                startWith : \"$reportsTo\", \
                                restrictSearchWithMatch : { \
                                    \"ssn\" : { $gt : 1234 } }}}]",
                          {},
                          FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
                          true);

TEST_FLE_REWRITE_PIPELINE(GeoNear,
                          "[{ $geoNear: { \
                                key: \"location\", \
                                near : {type : {$const : \"Point\"}, \
                                coordinates : [ {$const : -73.99279}, {$const : 40.719296} ]}, \
                                distanceField : \"dist.calculated\", \
                                maxDistance : 10, \
                                minDistance : 2, \
                                query : {ssn : {encrypt : 1234}}, \
                                spherical : true, \
                                includeLocs : \"dist.location\"} \
                                }]",
                          "[{ $geoNear: { \
                                key: \"location\", \
                                near : {type : {$const : \"Point\"}, \
                                coordinates : [ {$const : -73.99279}, {$const : 40.719296} ]}, \
                                distanceField : \"dist.calculated\", \
                                maxDistance : 10, \
                                minDistance : 2, \
                                query : {ssn : {\"$gt\" : 1234}}, \
                                spherical : true, \
                                includeLocs : \"dist.location\"} \
                                }]",
                          {},
                          FLEServerRewritePipelineTest::kSingleEncryptionSchemaEncryptionInfo,
                          true);

// Note, for DocumentSourceLookup testing, we purposely provide an empty "let", because the
// implementation for the rewrites relies on pipeline::serialize() which will add the superfluous
// empty let variables.
TEST_FLE_REWRITE_PIPELINE(LookupSinglyNestedMatch,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]} 
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    {$match:
                                        {$and: [{b_ssn: {$gt: 2}}, {b_age: {$gt: 4}}]} 
                                    }]}}])",
                          {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd)},
                          FLEServerRewritePipelineTest::kTwoEncryptionSchemaEncryptionInfo,
                          true);

TEST_FLE_REWRITE_PIPELINE(LookupDoublyNestedMatch,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo", 
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [ 
                                            {$match:
                                                {$and: [{c_ssn: {encrypt: 2}}, {c_age: {encrypt: 4}}]} 
                                            }
                                            ]}},
                                    {$match: 
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]} 
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo", 
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [ 
                                            {$match:
                                                {$and: [{c_ssn: {$gt: 2}}, {c_age: {$gt: 4}}]} 
                                            }
                                            ]}},
                                    {$match: 
                                        {$and: [{b_ssn: {$gt: 2}}, {b_age: {$gt: 4}}]} 
                                    }]}}])",
                          std::vector<NamespaceString>(
                              {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd),
                               NamespaceString::createNamespaceString_forTest("test.coll_c"_sd)}),
                          FLEServerRewritePipelineTest::kThreeEncryptionSchemaEncryptionInfo,
                          true);

// Test that no rewrites take place when feature flag is disabled.
TEST_FLE_REWRITE_PIPELINE(LookupSinglyNestedMatch_FeatureFlagDisabled,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]} 
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    {$match:
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]} 
                                    }]}}])",
                          {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd)},
                          FLEServerRewritePipelineTest::kTwoEncryptionSchemaEncryptionInfo,
                          false);

TEST_FLE_REWRITE_PIPELINE(LookupDoublyNestedMatch_FeatureFlagDisabled,
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo", 
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [ 
                                            {$match:
                                                {$and: [{c_ssn: {encrypt: 2}}, {c_age: {encrypt: 4}}]} 
                                            }
                                            ]}},
                                    {$match: 
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]} 
                                    }]}}])",
                          R"([{ $lookup: {
                                from: "coll_b",
                                localField: "foo", 
                                foreignField: "b_foo",
                                as: "docs",
                                let: {},
                                pipeline: [ 
                                    { $lookup: {
                                        from: "coll_c",
                                        localField: "b_foo", 
                                        foreignField: "c_foo",
                                        as: "inner_docs",
                                        let: {},
                                        pipeline: [ 
                                            {$match:
                                                {$and: [{c_ssn: {encrypt: 2}}, {c_age: {encrypt: 4}}]} 
                                            }
                                            ]}},
                                    {$match: 
                                        {$and: [{b_ssn: {encrypt: 2}}, {b_age: {encrypt: 4}}]} 
                                    }]}}])",
                          std::vector<NamespaceString>(
                              {NamespaceString::createNamespaceString_forTest("test.coll_b"_sd),
                               NamespaceString::createNamespaceString_forTest("test.coll_c"_sd)}),
                          FLEServerRewritePipelineTest::kThreeEncryptionSchemaEncryptionInfo,
                          false);

}  // namespace
}  // namespace mongo
