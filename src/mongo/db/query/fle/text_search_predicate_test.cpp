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

#include <functional>
#include <initializer_list>
#include <map>
#include <set>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/db/query/fle/text_search_predicate.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::fle {
namespace {
class MockTextSearchPredicate : public TextSearchPredicate {
public:
    MockTextSearchPredicate(const QueryRewriterInterface* rewriter)
        : TextSearchPredicate(rewriter) {}
    MockTextSearchPredicate(const QueryRewriterInterface* rewriter,
                            StrTagMap tags,
                            std::set<StringData> encryptedFields)
        : TextSearchPredicate(rewriter), _tags(tags), _encryptedFields(encryptedFields) {}

    void setEncryptedTags(std::pair<StringData, StringData> fieldvalue,
                          std::vector<PrfBlock> tags) {
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
            (_exprTags.contains(v.coerceToString()));
    }

    std::vector<PrfBlock> generateTags(BSONValue payload) const override {
        return visit(OverloadedVisitor{[&](BSONElement p) {
                                           // We will never generateTags for a MatchExpression for
                                           // any encrypted text search.
                                           MONGO_UNREACHABLE_TASSERT(10112604);
                                           return std::vector<PrfBlock>();
                                       },
                                       [&](std::reference_wrapper<Value> v) {
                                           ASSERT(_exprTags.find(v.get().coerceToString()) !=
                                                  _exprTags.end());
                                           return _exprTags.find(v.get().coerceToString())->second;
                                       }},
                     payload);
    }

private:
    StrTagMap _tags;
    // Key the tags for agg expressions based on values only, since we don't have access to the
    // field name.
    std::map<StringData, std::vector<PrfBlock>> _exprTags;

    std::set<StringData> _encryptedFields;
};

class TextSearchPredicateRewriteTest : public EncryptedPredicateRewriteTest {
public:
    TextSearchPredicateRewriteTest() : _predicate(&_mock) {}

protected:
    MockTextSearchPredicate _predicate;
};

std::unique_ptr<Expression> makeEncStrStartsWithAggExpr(ExpressionContext* const expCtx,
                                                        StringData path,
                                                        Value value) {
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, path.toString(), expCtx->variablesParseState);
    auto textExpr = make_intrusive<ExpressionConstant>(expCtx, value);
    return std::make_unique<ExpressionEncStrStartsWith>(
        expCtx, std::move(fieldpath), std::move(textExpr));
}

std::unique_ptr<Expression> makeEncStrEndsWithAggExpr(ExpressionContext* const expCtx,
                                                      StringData path,
                                                      Value value) {
    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, path.toString(), expCtx->variablesParseState);
    auto textExpr = make_intrusive<ExpressionConstant>(expCtx, value);
    return std::make_unique<ExpressionEncStrEndsWith>(
        expCtx, std::move(fieldpath), std::move(textExpr));
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Starts_With_NoFFP_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrStartsWithAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value("5"_sd));
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Starts_With_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrStartsWithAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));
    std::vector<PrfBlock> tags = {{1}, {2}};

    _predicate.setEncryptedTags({"ssn", "hello"}, tags);

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
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_Ends_With_NoFFP_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrEndsWithAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value("5"_sd));
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_Ends_With_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrEndsWithAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));
    std::vector<PrfBlock> tags = {{1}, {2}};

    _predicate.setEncryptedTags({"ssn", "hello"}, tags);

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
}

class TextSearchPredicateCollScanRewriteTest : public EncryptedPredicateRewriteTest {
public:
    TextSearchPredicateCollScanRewriteTest() : _predicate(&_mock) {
        _mock.setForceEncryptedCollScanForTest();
    }

protected:
    MockTextSearchPredicate _predicate;
};

TEST_F(TextSearchPredicateCollScanRewriteTest, Enc_Str_Starts_With_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrStartsWithAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));

    // Serialize the expression before any rewrites occur to validate later.
    auto expressionPreRewrite = input->serialize().getDocument().toBson();

    std::vector<PrfBlock> tags = {{1}, {2}};
    _predicate.setEncryptedTags({"ssn", "hello"}, tags);

    auto result = _predicate.rewrite(input.get());

    // Since we don't rewrite the expression when we force a collection scan, the result will be
    // null.
    ASSERT(!result);

    // Make sure the original expression hasn't changed.
    ASSERT_BSONOBJ_EQ(input->serialize().getDocument().toBson(), expressionPreRewrite);
}

TEST_F(TextSearchPredicateCollScanRewriteTest, Enc_Str_Ends_With_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrEndsWithAggExpr(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));

    // Serialize the expression before any rewrites occur to validate later.
    auto expressionPreRewrite = input->serialize().getDocument().toBson();

    std::vector<PrfBlock> tags = {{1}, {2}};
    _predicate.setEncryptedTags({"ssn", "hello"}, tags);

    auto result = _predicate.rewrite(input.get());

    // Since we don't rewrite the expression when we force a collection scan, the result will be
    // null.
    ASSERT(!result);

    // Make sure the original expression hasn't changed.
    ASSERT_BSONOBJ_EQ(input->serialize().getDocument().toBson(), expressionPreRewrite);
}


}  // namespace
}  // namespace mongo::fle
