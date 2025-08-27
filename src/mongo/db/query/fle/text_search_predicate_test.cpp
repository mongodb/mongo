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

#include "mongo/db/query/fle/text_search_predicate.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_tokens.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/fle/encrypted_predicate_test_fixtures.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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
class MockTextSearchPredicate : public TextSearchPredicate {
public:
    MockTextSearchPredicate(const QueryRewriterInterface* rewriter)
        : TextSearchPredicate(rewriter) {}
    MockTextSearchPredicate(const QueryRewriterInterface* rewriter,
                            StrTagMap tags,
                            std::set<StringData> encryptedFields)
        : TextSearchPredicate(rewriter) {}

    void setEncryptedTags(StringData payload, std::vector<PrfBlock> tags) {
        _exprTags[payload] = tags;
    }


protected:
    bool isPayload(const BSONElement& elt) const override {
        // We should never use this function since we don't support match expressions.
        MONGO_UNREACHABLE_TASSERT(10172500);
        return false;
    }

    bool isPayload(const Value& v) const override {
        // For text search, we must always have tags set.
        return _exprTags.contains(v.coerceToString());
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
    // Key the tags for agg expressions based on values only, since we don't have access to the
    // field name.
    std::map<StringData, std::vector<PrfBlock>> _exprTags;
};

class TextSearchPredicateRewriteTest : public EncryptedPredicateRewriteTest {
public:
    TextSearchPredicateRewriteTest() : _predicate(&_mock) {}

protected:
    MockTextSearchPredicate _predicate;
};

class MockTextSearchPredicateWithFFP : public TextSearchPredicate {
public:
    MockTextSearchPredicateWithFFP(const QueryRewriterInterface* rewriter)
        : TextSearchPredicate(rewriter) {}
    MockTextSearchPredicateWithFFP(const QueryRewriterInterface* rewriter,
                                   StrTagMap tags,
                                   std::set<StringData> encryptedFields)
        : TextSearchPredicate(rewriter) {}

    void setEncryptedTags(Value payload, std::vector<PrfBlock> tags) {
        _exprTags.emplace(payload, tags);
    }


protected:
    std::vector<PrfBlock> generateTags(BSONValue payload) const override {
        return visit(OverloadedVisitor{[&](BSONElement p) {
                                           // We will never generateTags for a MatchExpression for
                                           // any encrypted text search.
                                           MONGO_UNREACHABLE_TASSERT(10172501);
                                           return std::vector<PrfBlock>();
                                       },
                                       [&](std::reference_wrapper<Value> v) {
                                           ASSERT(_exprTags.find(v) != _exprTags.end());
                                           return _exprTags.find(v)->second;
                                       }},
                     payload);
    }

private:
    struct KeyHash {
        std::size_t operator()(const Value& k) const {
            return std::hash<int>()(k.getBinData().length);
        }
    };

    struct KeyEqual {
        bool operator()(const Value& lhs, const Value& rhs) const {
            auto lhsBinData = lhs.getBinData();
            auto rhsBinData = rhs.getBinData();
            if (lhsBinData.type != rhsBinData.type) {
                return false;
            }
            if (lhsBinData.length != rhsBinData.length) {
                return false;
            }
            auto lhsData = (const uint8_t*)lhsBinData.data;
            auto rhsData = (const uint8_t*)rhsBinData.data;
            for (int i = 0; i < lhsBinData.length; ++i) {
                if (lhsData[i] != rhsData[i]) {
                    return false;
                }
            }
            return true;
        }
    };
    // Key the tags for agg expressions based on values only, since we don't have access to the
    // field name. The Value in the map must contain BSONBinData type used for payloads.
    stdx::unordered_map<Value, std::vector<PrfBlock>, KeyHash, KeyEqual> _exprTags;
};

class TextSearchPredicateRewriteTestWithFFP : public EncryptedPredicateRewriteTest {
public:
    TextSearchPredicateRewriteTestWithFFP() : _predicate(&_mock) {}

protected:
    MockTextSearchPredicateWithFFP _predicate;
};

// This duplicates text search contexts in "query_analysis.h" located in the enterprise module.
enum class EncryptionPlaceholderContext {
    kTextPrefixComparison,
    kTextSuffixComparison,
    kTextSubstringComparison,
    kTextNormalizedComparison,
};

BSONObj generateFFP(StringData path, Value value, EncryptionPlaceholderContext context) {
    auto indexKey = getIndexKey();
    FLEIndexKeyAndId indexKeyAndId(indexKey.data, indexKeyId);
    auto userKey = getUserKey();
    FLEUserKeyAndId userKeyAndId(userKey.data, indexKeyId);

    BSONObj doc = BSON("value" << value);
    auto element = doc.firstElement();
    auto cdrValue = ConstDataRange(element.value(), element.value() + element.valuesize());

    // Tokens are made according to fle_tokens.h.
    auto collectionToken = CollectionsLevel1Token::deriveFrom(indexKey.data);
    auto edcToken = EDCToken::deriveFrom(collectionToken);
    auto escToken = ESCToken::deriveFrom(collectionToken);
    auto serverToken = ServerTokenDerivationLevel1Token::deriveFrom(indexKey.data);

    FLE2FindTextPayload payload;
    // Set some default values here.
    payload.setCaseFold(false);
    payload.setDiacriticFold(false);
    payload.setMaxCounter(22);
    payload.setTokenSets(mongo::TextSearchFindTokenSets{});

    // Create tokens specific to each type, also from fle_tokens.h.
    switch (context) {
        case EncryptionPlaceholderContext::kTextPrefixComparison: {
            auto edcTextPrefixToken = EDCTextPrefixToken::deriveFrom(edcToken);
            auto edcTextPrefixDerivedFromDataToken =
                EDCTextPrefixDerivedFromDataToken::deriveFrom(edcTextPrefixToken, cdrValue);

            auto escTextPrefixToken = ESCTextPrefixToken::deriveFrom(escToken);
            auto escTextPrefixDerivedFromDataToken =
                ESCTextPrefixDerivedFromDataToken::deriveFrom(escTextPrefixToken, cdrValue);

            auto serverTextPrefixToken = ServerTextPrefixToken::deriveFrom(serverToken);
            auto serverTextPrefixDerivedFromDataToken =
                ServerTextPrefixDerivedFromDataToken::deriveFrom(serverTextPrefixToken, cdrValue);

            payload.getTokenSets().setPrefixTokens(
                TextPrefixFindTokenSet{edcTextPrefixDerivedFromDataToken,
                                       escTextPrefixDerivedFromDataToken,
                                       serverTextPrefixDerivedFromDataToken});
        } break;
        case EncryptionPlaceholderContext::kTextSuffixComparison: {
            auto edcTextSuffixToken = EDCTextSuffixToken::deriveFrom(edcToken);
            auto edcTextSuffixDerivedFromDataToken =
                EDCTextSuffixDerivedFromDataToken::deriveFrom(edcTextSuffixToken, cdrValue);

            auto escTextSuffixToken = ESCTextSuffixToken::deriveFrom(escToken);
            auto escTextSuffixDerivedFromDataToken =
                ESCTextSuffixDerivedFromDataToken::deriveFrom(escTextSuffixToken, cdrValue);

            auto serverTextSuffixToken = ServerTextSuffixToken::deriveFrom(serverToken);
            auto serverTextSuffixDerivedFromDataToken =
                ServerTextSuffixDerivedFromDataToken::deriveFrom(serverTextSuffixToken, cdrValue);

            payload.getTokenSets().setSuffixTokens(
                TextSuffixFindTokenSet{edcTextSuffixDerivedFromDataToken,
                                       escTextSuffixDerivedFromDataToken,
                                       serverTextSuffixDerivedFromDataToken});
        } break;
        case EncryptionPlaceholderContext::kTextSubstringComparison: {
            auto edcTextSubstringToken = EDCTextSubstringToken::deriveFrom(edcToken);
            auto edcTextSubstringDerivedFromDataToken =
                EDCTextSubstringDerivedFromDataToken::deriveFrom(edcTextSubstringToken, cdrValue);

            auto escTextSubstringToken = ESCTextSubstringToken::deriveFrom(escToken);
            auto escTextSubstringDerivedFromDataToken =
                ESCTextSubstringDerivedFromDataToken::deriveFrom(escTextSubstringToken, cdrValue);

            auto serverTextSubstringToken = ServerTextSubstringToken::deriveFrom(serverToken);
            auto serverTextSubstringDerivedFromDataToken =
                ServerTextSubstringDerivedFromDataToken::deriveFrom(serverTextSubstringToken,
                                                                    cdrValue);

            payload.getTokenSets().setSubstringTokens(
                TextSubstringFindTokenSet{edcTextSubstringDerivedFromDataToken,
                                          escTextSubstringDerivedFromDataToken,
                                          serverTextSubstringDerivedFromDataToken});
        } break;
        case EncryptionPlaceholderContext::kTextNormalizedComparison: {
            auto edcTextExactToken = EDCTextExactToken::deriveFrom(edcToken);
            auto edcTextExactDerivedFromDataToken =
                EDCTextExactDerivedFromDataToken::deriveFrom(edcTextExactToken, cdrValue);

            auto escTextExactToken = ESCTextExactToken::deriveFrom(escToken);
            auto escTextExactDerivedFromDataToken =
                ESCTextExactDerivedFromDataToken::deriveFrom(escTextExactToken, cdrValue);

            auto serverTextExactToken = ServerTextExactToken::deriveFrom(serverToken);
            auto serverTextExactDerivedFromDataToken =
                ServerTextExactDerivedFromDataToken::deriveFrom(serverTextExactToken, cdrValue);

            payload.getTokenSets().setExactTokens(
                TextExactFindTokenSet{edcTextExactDerivedFromDataToken,
                                      escTextExactDerivedFromDataToken,
                                      serverTextExactDerivedFromDataToken});
        } break;
        default:
            MONGO_UNREACHABLE_TASSERT(10256200);
    }

    BSONObjBuilder builder;
    FLE2FindTextPayload::parse(payload.toBSON(), IDLParserContext{"FLE2FindTextPayload"});
    toEncryptedBinData(path, EncryptedBinDataType::kFLE2FindTextPayload, payload, &builder);
    return builder.obj();
}


template <typename T>
std::unique_ptr<Expression> makeEncStrStartsWith(ExpressionContext* const expCtx,
                                                 StringData path,
                                                 T value) {
    // A BSONObj corresponds to using an actual FindTextPayload, while a Value is used otherwise.
    static_assert(std::is_same_v<T, BSONObj> || std::is_same_v<T, Value>);

    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, std::string{path}, expCtx->variablesParseState);

    if constexpr (std::is_same_v<T, BSONObj>) {
        return std::make_unique<ExpressionEncStrStartsWith>(
            expCtx,
            std::move(fieldpath),
            ExpressionConstant::parse(expCtx, value.firstElement(), expCtx->variablesParseState));
    } else {
        return std::make_unique<ExpressionEncStrStartsWith>(
            expCtx, std::move(fieldpath), make_intrusive<ExpressionConstant>(expCtx, value));
    }
}

template <typename T>
std::unique_ptr<Expression> makeEncStrEndsWith(ExpressionContext* const expCtx,
                                               StringData path,
                                               T value) {
    // A BSONObj corresponds to using an actual FindTextPayload, while a Value is used otherwise.
    static_assert(std::is_same_v<T, BSONObj> || std::is_same_v<T, Value>);

    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, std::string{path}, expCtx->variablesParseState);

    if constexpr (std::is_same_v<T, BSONObj>) {
        return std::make_unique<ExpressionEncStrEndsWith>(
            expCtx,
            std::move(fieldpath),
            ExpressionConstant::parse(expCtx, value.firstElement(), expCtx->variablesParseState));
    } else {
        return std::make_unique<ExpressionEncStrEndsWith>(
            expCtx, std::move(fieldpath), make_intrusive<ExpressionConstant>(expCtx, value));
    }
}

template <typename T>
std::unique_ptr<Expression> makeEncStrContains(ExpressionContext* const expCtx,
                                               StringData path,
                                               T value) {
    // A BSONObj corresponds to using an actual FindTextPayload, while a Value is used otherwise.
    static_assert(std::is_same_v<T, BSONObj> || std::is_same_v<T, Value>);

    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, std::string{path}, expCtx->variablesParseState);

    if constexpr (std::is_same_v<T, BSONObj>) {
        return std::make_unique<ExpressionEncStrContains>(
            expCtx,
            std::move(fieldpath),
            ExpressionConstant::parse(expCtx, value.firstElement(), expCtx->variablesParseState));
    } else {
        return std::make_unique<ExpressionEncStrContains>(
            expCtx, std::move(fieldpath), make_intrusive<ExpressionConstant>(expCtx, value));
    }
}

template <typename T>
std::unique_ptr<Expression> makeEncStrNormalizedEq(ExpressionContext* const expCtx,
                                                   StringData path,
                                                   T value) {
    // A BSONObj corresponds to using an actual FindTextPayload, while a Value is used otherwise.
    static_assert(std::is_same_v<T, BSONObj> || std::is_same_v<T, Value>);

    auto fieldpath = ExpressionFieldPath::createPathFromString(
        expCtx, std::string{path}, expCtx->variablesParseState);

    if constexpr (std::is_same_v<T, BSONObj>) {
        return std::make_unique<ExpressionEncStrNormalizedEq>(
            expCtx,
            std::move(fieldpath),
            ExpressionConstant::parse(expCtx, value.firstElement(), expCtx->variablesParseState));
    } else {
        return std::make_unique<ExpressionEncStrNormalizedEq>(
            expCtx, std::move(fieldpath), make_intrusive<ExpressionConstant>(expCtx, value));
    }
}

template <typename T>
void runEncStrStartsWithTest(T& predicate,
                             ExpressionContext* const expCtx,
                             StringData fieldName,
                             StringData valueSD,
                             std::vector<PrfBlock> tags,
                             BSONObj expectedResult) {
    static_assert(std::is_same_v<T, MockTextSearchPredicate> ||
                  std::is_same_v<T, MockTextSearchPredicateWithFFP>);

    std::unique_ptr<Expression> input = nullptr;
    Value value = Value(valueSD);

    if constexpr (std::is_same_v<T, MockTextSearchPredicateWithFFP>) {
        BSONObj ffp =
            generateFFP(fieldName, value, EncryptionPlaceholderContext::kTextPrefixComparison);
        predicate.setEncryptedTags(Value(ffp.firstElement()), tags);
        input = makeEncStrStartsWith(expCtx, fieldName, ffp);
    } else {
        predicate.setEncryptedTags(valueSD, std::move(tags));
        input = makeEncStrStartsWith(expCtx, fieldName, value);
    }

    auto actual = predicate.rewrite(input.get());
    auto actualBson = actual->serialize().getDocument().toBson();
    ASSERT_BSONOBJ_EQ(expectedResult, actualBson);
}

template <typename T>
void runEncStrEndsWithTest(T& predicate,
                           ExpressionContext* const expCtx,
                           StringData fieldName,
                           StringData valueSD,
                           std::vector<PrfBlock> tags,
                           BSONObj expectedResult) {
    static_assert(std::is_same_v<T, MockTextSearchPredicate> ||
                  std::is_same_v<T, MockTextSearchPredicateWithFFP>);

    std::unique_ptr<Expression> input = nullptr;
    Value value = Value(valueSD);

    if constexpr (std::is_same_v<T, MockTextSearchPredicateWithFFP>) {
        BSONObj ffp =
            generateFFP(fieldName, value, EncryptionPlaceholderContext::kTextSuffixComparison);
        predicate.setEncryptedTags(Value(ffp.firstElement()), tags);
        input = makeEncStrEndsWith(expCtx, fieldName, ffp);
    } else {
        predicate.setEncryptedTags(valueSD, std::move(tags));
        input = makeEncStrEndsWith(expCtx, fieldName, value);
    }

    auto actual = predicate.rewrite(input.get());
    auto actualBson = actual->serialize().getDocument().toBson();
    ASSERT_BSONOBJ_EQ(expectedResult, actualBson);
}

template <typename T>
void runEncStrContainsTest(T& predicate,
                           ExpressionContext* const expCtx,
                           StringData valueSD,
                           StringData fieldName,
                           std::vector<PrfBlock> tags,
                           BSONObj expectedResult) {
    static_assert(std::is_same_v<T, MockTextSearchPredicate> ||
                  std::is_same_v<T, MockTextSearchPredicateWithFFP>);

    std::unique_ptr<Expression> input = nullptr;
    Value value = Value(valueSD);

    if constexpr (std::is_same_v<T, MockTextSearchPredicateWithFFP>) {
        BSONObj ffp =
            generateFFP(fieldName, value, EncryptionPlaceholderContext::kTextSubstringComparison);
        predicate.setEncryptedTags(Value(ffp.firstElement()), tags);
        input = makeEncStrContains(expCtx, fieldName, ffp);
    } else {
        predicate.setEncryptedTags(valueSD, std::move(tags));
        input = makeEncStrContains(expCtx, fieldName, value);
    }

    auto actual = predicate.rewrite(input.get());
    auto actualBson = actual->serialize().getDocument().toBson();
    ASSERT_BSONOBJ_EQ(expectedResult, actualBson);
}

template <typename T>
void runEncStrNormalizedEqTest(T& predicate,
                               ExpressionContext* const expCtx,
                               StringData valueSD,
                               StringData fieldName,
                               std::vector<PrfBlock> tags,
                               BSONObj expectedResult) {
    static_assert(std::is_same_v<T, MockTextSearchPredicate> ||
                  std::is_same_v<T, MockTextSearchPredicateWithFFP>);

    std::unique_ptr<Expression> input = nullptr;
    Value value = Value(valueSD);

    if constexpr (std::is_same_v<T, MockTextSearchPredicateWithFFP>) {
        BSONObj ffp =
            generateFFP(fieldName, value, EncryptionPlaceholderContext::kTextNormalizedComparison);
        predicate.setEncryptedTags(Value(ffp.firstElement()), tags);
        input = makeEncStrNormalizedEq(expCtx, fieldName, ffp);
    } else {
        predicate.setEncryptedTags(valueSD, std::move(tags));
        input = makeEncStrNormalizedEq(expCtx, fieldName, value);
    }

    auto actual = predicate.rewrite(input.get());
    auto actualBson = actual->serialize().getDocument().toBson();
    ASSERT_BSONOBJ_EQ(expectedResult, actualBson);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Starts_With_NoFFP_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrStartsWith(_mock.getExpressionContext(), "ssn"_sd, Value("5"_sd));
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Starts_With_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrStartsWithTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
}

// Testing the same scenario again with an actual payload.
TEST_F(TextSearchPredicateRewriteTestWithFFP, Enc_Starts_With_FFP_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrStartsWithTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_Ends_With_NoFFP_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrEndsWith(_mock.getExpressionContext(), "ssn"_sd, Value("5"_sd));
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_Ends_With_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrEndsWithTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
}

// Tests same as above with an actual FindTextPayload.
TEST_F(TextSearchPredicateRewriteTestWithFFP, Enc_Str_Ends_With_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrEndsWithTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_Contains_NoFFP_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrContains(_mock.getExpressionContext(), "ssn"_sd, Value("5"_sd));
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_Contains_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrContainsTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
}

// Tests same as above with an actual FindTextPayload.
TEST_F(TextSearchPredicateRewriteTestWithFFP, Enc_Str_Contains_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrContainsTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_NormalizedEq_NoFFP_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrNormalizedEq(_mock.getExpressionContext(), "ssn"_sd, Value("5"_sd));
    ASSERT_EQ(_predicate.rewrite(input.get()), nullptr);
}

TEST_F(TextSearchPredicateRewriteTest, Enc_Str_NormalizedEq_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrNormalizedEqTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
}

// Tests same as above with an actual FindTextPayload.
TEST_F(TextSearchPredicateRewriteTestWithFFP, Enc_Str_NormalizedEq_Expr) {
    auto expectedResult = fromjson(R"({
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
})");
    runEncStrNormalizedEqTest(
        _predicate, _mock.getExpressionContext(), "ssn"_sd, "hello"_sd, {{1}, {2}}, expectedResult);
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
        makeEncStrStartsWith(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));

    // Serialize the expression before any rewrites occur to validate later.
    auto expressionPreRewrite = input->serialize().getDocument().toBson();

    std::vector<PrfBlock> tags = {{1}, {2}};
    _predicate.setEncryptedTags("hello", tags);

    auto result = _predicate.rewrite(input.get());

    // Since we don't rewrite the expression when we force a collection scan, the result will be
    // null.
    ASSERT(!result);

    // Make sure the original expression hasn't changed.
    ASSERT_BSONOBJ_EQ(input->serialize().getDocument().toBson(), expressionPreRewrite);
}

TEST_F(TextSearchPredicateCollScanRewriteTest, Enc_Str_Ends_With_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrEndsWith(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));

    // Serialize the expression before any rewrites occur to validate later.
    auto expressionPreRewrite = input->serialize().getDocument().toBson();

    std::vector<PrfBlock> tags = {{1}, {2}};
    _predicate.setEncryptedTags("hello", tags);

    auto result = _predicate.rewrite(input.get());

    // Since we don't rewrite the expression when we force a collection scan, the result will be
    // null.
    ASSERT(!result);

    // Make sure the original expression hasn't changed.
    ASSERT_BSONOBJ_EQ(input->serialize().getDocument().toBson(), expressionPreRewrite);
}

TEST_F(TextSearchPredicateCollScanRewriteTest, Enc_Str_Contains_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrContains(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));

    // Serialize the expression before any rewrites occur to validate later.
    auto expressionPreRewrite = input->serialize().getDocument().toBson();

    std::vector<PrfBlock> tags = {{1}, {2}};
    _predicate.setEncryptedTags("hello", tags);

    auto result = _predicate.rewrite(input.get());

    // Since we don't rewrite the expression when we force a collection scan, the result will be
    // null.
    ASSERT(!result);

    // Make sure the original expression hasn't changed.
    ASSERT_BSONOBJ_EQ(input->serialize().getDocument().toBson(), expressionPreRewrite);
}

TEST_F(TextSearchPredicateCollScanRewriteTest, Enc_Str_NormalizedEq_Expr) {
    std::unique_ptr<Expression> input =
        makeEncStrNormalizedEq(_mock.getExpressionContext(), "ssn"_sd, Value("hello"_sd));

    // Serialize the expression before any rewrites occur to validate later.
    auto expressionPreRewrite = input->serialize().getDocument().toBson();

    std::vector<PrfBlock> tags = {{1}, {2}};
    _predicate.setEncryptedTags("hello", tags);

    auto result = _predicate.rewrite(input.get());

    // Since we don't rewrite the expression when we force a collection scan, the result will be
    // null.
    ASSERT(!result);

    // Make sure the original expression hasn't changed.
    ASSERT_BSONOBJ_EQ(input->serialize().getDocument().toBson(), expressionPreRewrite);
}

}  // namespace
}  // namespace mongo::fle
