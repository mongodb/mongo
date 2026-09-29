// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/matcher_typing.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/tassert_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo::pipeline::type_system {
namespace {

std::unique_ptr<MatchExpression> parseMatchExpr(const std::string& query) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto result = MatchExpressionParser::parse(fromjson(query),
                                               expCtx,
                                               ExtensionsCallbackNoop(),
                                               MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(result.getStatus()) << query;
    return std::move(result.getValue());
}

Type anyObject() {
    return Type(BSONType::object, Extent::kAll);
}

Type objectWithKnownField() {
    return Type::object({{"x", Type(BSONType::numberInt, Extent::kAll)}}, Open::kYes);
}

TEST(MatcherTypingTest, ObjectTypeIsReturnedUnchanged) {
    auto expr = parseMatchExpr("{a: 1}");
    ASSERT_EQ(narrowType(anyObject(), expr.get(), true), anyObject());
    ASSERT_EQ(narrowType(anyObject(), expr.get(), false), anyObject());
}

TEST(MatcherTypingTest, KnownFieldsAreReturnedUnchanged) {
    auto expr = parseMatchExpr("{a: 1}");
    ASSERT_EQ(narrowType(objectWithKnownField(), expr.get(), true), objectWithKnownField());
    ASSERT_EQ(narrowType(objectWithKnownField(), expr.get(), false), objectWithKnownField());
}

TEST(MatcherTypingTest, UnsatisfiableInputIsReturnedUnchanged) {
    auto expr = parseMatchExpr("{a: 1}");
    ASSERT_EQ(narrowType(Type::never(), expr.get(), true), Type::never());
    ASSERT_EQ(narrowType(Type::never(), expr.get(), false), Type::never());
}

TEST(MatcherTypingTest, InputCoveringAnyTypeIsRejected) {
    auto expr = parseMatchExpr("{a: 1}");
    ASSERT_TASSERT_CODE(narrowType(Type::any(), expr.get(), true), 13459201);
}

TEST(MatcherTypingTest, InputCoveringNonObjectTypeIsRejected) {
    auto expr = parseMatchExpr("{a: 1}");
    ASSERT_TASSERT_CODE(narrowType(Type(BSONType::string, Extent::kAll), expr.get(), true),
                        13459201);
    ASSERT_TASSERT_CODE(narrowType(unionType(Type(BSONType::object, Extent::kAll),
                                             Type(BSONType::array, Extent::kAll)),
                                   expr.get(),
                                   true),
                        13459201);
}

}  // namespace
}  // namespace mongo::pipeline::type_system
