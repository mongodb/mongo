// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

TEST(ExpressionBsonSize, WorksOnDocumentLargerThan16MB) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$bsonSize" << "$$ROOT");
    auto expression = Expression::parseExpression(&expCtx, obj, vps);

    // Four 9MB strings make a document exceeding the 16MiB BSON size limit.
    constexpr size_t longStringLength = 9 * 1024 * 1024;
    static_assert(4 * longStringLength > BSONObjMaxUserSize);
    MutableDocument md;
    md.addField("a", Value(std::string(longStringLength, 'A')));
    md.addField("b", Value(std::string(longStringLength, 'B')));
    md.addField("c", Value(std::string(longStringLength, 'C')));
    md.addField("d", Value(std::string(longStringLength, 'D')));
    Document largeDoc = md.freeze();

    // Must not throw BSONObjectTooLarge for intermediate documents larger than 16MiB.
    Value result = expression->evaluate(largeDoc, &expCtx.variables);
    ASSERT_EQ(result.getType(), BSONType::numberInt);
    ASSERT_GT(result.getInt(), static_cast<int>(BSONObjMaxUserSize));
}

TEST(ExpressionInternalFindAllValuesAtPath, PreservesSimpleArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a"sv));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    auto result =
        expression->evaluate(Document{{"a", Value({Value(1), Value(2)})}}, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, PreservesSimpleNestedArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"sv));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    auto doc = Document{{"a", Value(Document{{"b", Value({Value(1), Value(2)})}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DescendsThroughSingleArrayAndObject) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"sv));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document doc = Document{
        {"a",
         Value({Document{{"b", Value(1)}}, Document{{"b", Value(2)}}, Document{{"b", Value(3)}}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DescendsThroughMultipleObjectArrayPairs) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"sv));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document doc = Document{{"a",
                             Value({Document{{"b", Value({Value(1), Value(2)})}},
                                    Document{{"b", Value({Value(3), Value(4)})}},
                                    Document{{"b", Value({Value(5), Value(6)})}}})}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(1 << 2 << 3 << 4 << 5 << 6)), result);
}

TEST(ExpressionInternalFindAllValuesAtPath, DoesNotDescendThroughDoubleArray) {
    auto expCtx = ExpressionContextForTest{};
    VariablesParseState vps = expCtx.variablesParseState;
    const BSONObj obj = BSON("$_internalFindAllValuesAtPath" << Value("a.b"sv));
    auto expression = Expression::parseExpression(&expCtx, obj, vps);
    Document seenDoc1 = Document{{"b", Value({Value(5), Value(6)})}};
    Document seenDoc2 = Document{{"b", Value({Value(3), Value(4)})}};
    Document unseenDoc1 = Document{{"b", Value({Value(1), Value(2)})}};
    Document unseenDoc2 = Document{{"b", Value({Value(7), Value(8)})}};

    Document doc = Document{{"a",
                             Value({
                                 Value({unseenDoc1, unseenDoc2}),
                                 Value(seenDoc1),
                                 Value(seenDoc2),
                             })}};
    auto result = expression->evaluate(doc, &expCtx.variables);
    ASSERT_VALUE_EQ(Value(BSON_ARRAY(3 << 4 << 5 << 6)), result);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
