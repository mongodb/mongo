// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace expression_evaluation_test {

using boost::intrusive_ptr;

namespace {
template <typename ExpressionRegexSubClass>
intrusive_ptr<Expression> generateOptimizedExpression(const BSONObj& input,
                                                      ExpressionContextForTest* expCtx) {

    auto expression =
        ExpressionRegexSubClass::parse(expCtx, input.firstElement(), expCtx->variablesParseState);
    return expression->optimize();
}

void testAllExpressions(const BSONObj& input,
                        bool optimized,
                        const std::vector<Value>& expectedFindAllOutput) {

    auto expCtx = ExpressionContextForTest{};
    {
        // For $regexFindAll.
        auto expression = generateOptimizedExpression<ExpressionRegexFindAll>(input, &expCtx);
        auto regexFindAllExpr = dynamic_cast<ExpressionRegexFindAll*>(expression.get());
        ASSERT_EQ(regexFindAllExpr->hasConstantRegex(), optimized);
        Value output = expression->evaluate({}, &expCtx.variables);
        ASSERT_VALUE_EQ(output, Value(expectedFindAllOutput));
    }
    {
        // For $regexFind.
        auto expression = generateOptimizedExpression<ExpressionRegexFind>(input, &expCtx);
        auto regexFindExpr = dynamic_cast<ExpressionRegexFind*>(expression.get());
        ASSERT_EQ(regexFindExpr->hasConstantRegex(), optimized);
        Value output = expression->evaluate({}, &expCtx.variables);
        ASSERT_VALUE_EQ(output,
                        expectedFindAllOutput.empty() ? Value(BSONNULL) : expectedFindAllOutput[0]);
    }
    {
        // For $regexMatch.
        auto expression = generateOptimizedExpression<ExpressionRegexMatch>(input, &expCtx);
        auto regexMatchExpr = dynamic_cast<ExpressionRegexMatch*>(expression.get());
        ASSERT_EQ(regexMatchExpr->hasConstantRegex(), optimized);
        Value output = expression->evaluate({}, &expCtx.variables);
        ASSERT_VALUE_EQ(output, expectedFindAllOutput.empty() ? Value(false) : Value(true));
    }
}
}  // namespace

TEST(ExpressionRegexTest, BasicTest) {
    testAllExpressions(fromjson("{$regexFindAll : {input: 'asdf', regex: '^as' }}"),
                       true,
                       {Value(fromjson("{match: 'as', idx:0, captures:[]}"))});
}

TEST(ExpressionRegexTest, ExtendedRegexOptions) {
    testAllExpressions(fromjson("{$regexFindAll : {input: 'FirstLine\\nSecondLine', regex: "
                                "'^second' , options: 'mi'}}"),
                       true,
                       {Value(fromjson("{match: 'Second', idx:10, captures:[]}"))});
}

TEST(ExpressionRegexTest, MultipleMatches) {
    testAllExpressions(fromjson("{$regexFindAll : {input: 'a1b2c3', regex: '([a-c][1-3])' }}"),
                       true,
                       {Value(fromjson("{match: 'a1', idx:0, captures:['a1']}")),
                        Value(fromjson("{match: 'b2', idx:2, captures:['b2']}")),
                        Value(fromjson("{match: 'c3', idx:4, captures:['c3']}"))});
}

TEST(ExpressionRegexTest, OptimizPatternWhenInputIsVariable) {
    testAllExpressions(
        fromjson("{$regexFindAll : {input: '$input', regex: '([a-c][1-3])' }}"), true, {});
}

TEST(ExpressionRegexTest, NoOptimizePatternWhenRegexVariable) {
    testAllExpressions(fromjson("{$regexFindAll : {input: 'asdf', regex: '$regex' }}"), false, {});
}

TEST(ExpressionRegexTest, NoOptimizePatternWhenOptionsVariable) {
    testAllExpressions(
        fromjson("{$regexFindAll : {input: 'asdf', regex: '(asdf)', options: '$options' }}"),
        false,
        {Value(fromjson("{match: 'asdf', idx:0, captures:['asdf']}"))});
}

TEST(ExpressionRegexTest, NoMatch) {
    testAllExpressions(fromjson("{$regexFindAll : {input: 'a1b2c3', regex: 'ab' }}"), true, {});
}

TEST(ExpressionRegexTest, FailureCaseBadRegexType) {
    ASSERT_THROWS_CODE(
        testAllExpressions(fromjson("{$regexFindAll : {input: 'FirstLine\\nSecondLine', regex: "
                                    "{invalid : 'regex'} , options: 'mi'}}"),
                           false,
                           {}),
        AssertionException,
        51105);
}

TEST(ExpressionRegexTest, FailureCaseBadRegexPattern) {
    ASSERT_THROWS_CODE(
        testAllExpressions(
            fromjson("{$regexFindAll : {input: 'FirstLine\\nSecondLine', regex: '[0-9'}}"),
            false,
            {}),
        AssertionException,
        51111);
}

TEST(ExpressionRegexTest, InvalidUTF8InInput) {
    std::string inputField = "1234 ";
    // Append an invalid UTF-8 character.
    inputField += '\xe5';
    inputField += "  1234";
    BSONObj input(fromjson("{$regexFindAll: {input: '" + inputField + "', regex: '[0-9]'}}"));

    // Verify that PCRE will error during execution if input is not a valid UTF-8.
    ASSERT_THROWS_CODE(testAllExpressions(input, true, {}), AssertionException, 51156);
}

TEST(ExpressionRegexTest, InvalidUTF8InRegex) {
    std::string regexField = "1234 ";
    // Append an invalid UTF-8 character.
    regexField += '\xe5';
    BSONObj input(fromjson("{$regexFindAll: {input: '123456', regex: '" + regexField + "'}}"));
    // Verify that PCRE will error if REGEX is not a valid UTF-8.
    ASSERT_THROWS_CODE(testAllExpressions(input, false, {}), AssertionException, 51111);
}

}  // namespace expression_evaluation_test
}  // namespace mongo
