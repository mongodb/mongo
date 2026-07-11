// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace ExpressionTests {
using std::string;

namespace Trim {
using namespace std::literals::string_view_literals;

TEST(ExpressionTrimParsingTest, ThrowsIfSpecIsNotAnObject) {
    auto expCtx = ExpressionContextForTest{};

    ASSERT_THROWS(
        Expression::parseExpression(&expCtx, BSON("$trim" << 1), expCtx.variablesParseState),
        AssertionException);
    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$trim" << BSON_ARRAY(1 << 2)), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$ltrim" << BSONNULL), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$rtrim" << "string"), expCtx.variablesParseState),
                  AssertionException);
}

TEST(ExpressionTrimParsingTest, ThrowsIfSpecDoesNotSpecifyInput) {
    auto expCtx = ExpressionContextForTest{};

    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$trim" << BSONObj()), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(&expCtx,
                                              BSON("$ltrim" << BSON("chars" << "xyz")),
                                              expCtx.variablesParseState),
                  AssertionException);
}

TEST(ExpressionTrimParsingTest, ThrowsIfSpecContainsUnrecognizedField) {
    auto expCtx = ExpressionContextForTest{};

    ASSERT_THROWS(Expression::parseExpression(
                      &expCtx, BSON("$trim" << BSON("other" << 1)), expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(&expCtx,
                                              BSON("$ltrim" << BSON("chars" << "xyz"
                                                                            << "other" << 1)),
                                              expCtx.variablesParseState),
                  AssertionException);
    ASSERT_THROWS(Expression::parseExpression(&expCtx,
                                              BSON("$rtrim" << BSON("input" << "$x"
                                                                            << "chars"
                                                                            << "xyz"
                                                                            << "other" << 1)),
                                              expCtx.variablesParseState),
                  AssertionException);
}

TEST(ExpressionTrimTest, DoesOptimizeToConstantWithNoChars) {
    auto expCtx = ExpressionContextForTest{};
    auto trim = Expression::parseExpression(
        &expCtx, BSON("$trim" << BSON("input" << " abc ")), expCtx.variablesParseState);
    auto optimized = trim->optimize();
    auto constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"sv));

    // Test that it optimizes to a constant if the input also optimizes to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input" << BSON("$concat" << BSON_ARRAY(" " << "abc ")))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"sv));
}

TEST(ExpressionTrimTest, DoesOptimizeToConstantWithCustomChars) {
    auto expCtx = ExpressionContextForTest{};
    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input" << " abc "
                                                                         << "chars"
                                                                         << " ")),
                                            expCtx.variablesParseState);
    auto optimized = trim->optimize();
    auto constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"sv));

    // Test that it optimizes to a constant if the chars argument optimizes to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input" << "  abc "
                                     << "chars"
                                     << BSON("$substrCP" << BSON_ARRAY("  " << 1 << 1)))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"sv));

    // Test that it optimizes to a constant if both arguments optimize to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input" << BSON("$concat" << BSON_ARRAY(" " << "abc ")) << "chars"
                                     << BSON("$substrCP" << BSON_ARRAY("  " << 1 << 1)))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"sv));
}

TEST(ExpressionTrimTest, DoesNotOptimizeToConstantWithFieldPaths) {
    auto expCtx = ExpressionContextForTest{};

    // 'input' is field path.
    auto trim = Expression::parseExpression(
        &expCtx, BSON("$trim" << BSON("input" << "$inputField")), expCtx.variablesParseState);
    auto optimized = trim->optimize();
    auto constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_FALSE(constant);

    // 'chars' is field path.
    trim = Expression::parseExpression(&expCtx,
                                       BSON("$trim" << BSON("input" << " abc "
                                                                    << "chars"
                                                                    << "$secondInput")),
                                       expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_FALSE(constant);

    // Both are field paths.
    trim = Expression::parseExpression(&expCtx,
                                       BSON("$trim" << BSON("input" << "$inputField"
                                                                    << "chars"
                                                                    << "$secondInput")),
                                       expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_FALSE(constant);
}

TEST(ExpressionTrimTest, DoesAddInputDependencies) {
    auto expCtx = ExpressionContextForTest{};

    auto trim = Expression::parseExpression(
        &expCtx, BSON("$trim" << BSON("input" << "$inputField")), expCtx.variablesParseState);
    DepsTracker deps;
    expression::addDependencies(trim.get(), &deps);
    ASSERT_EQ(deps.fields.count("inputField"), 1u);
    ASSERT_EQ(deps.fields.size(), 1u);
}

TEST(ExpressionTrimTest, DoesAddCharsDependencies) {
    auto expCtx = ExpressionContextForTest{};

    auto trim = Expression::parseExpression(&expCtx,
                                            BSON("$trim" << BSON("input" << "$inputField"
                                                                         << "chars"
                                                                         << "$$CURRENT.a")),
                                            expCtx.variablesParseState);
    DepsTracker deps;
    expression::addDependencies(trim.get(), &deps);
    ASSERT_EQ(deps.fields.count("inputField"), 1u);
    ASSERT_EQ(deps.fields.count("a"), 1u);
    ASSERT_EQ(deps.fields.size(), 2u);
}

TEST(ExpressionTrimTest, DoesSerializeCorrectly) {
    auto expCtx = ExpressionContextForTest{};

    auto trim = Expression::parseExpression(
        &expCtx, BSON("$trim" << BSON("input" << " abc ")), expCtx.variablesParseState);
    ASSERT_VALUE_EQ(
        trim->serialize(),
        trim->serialize(query_shape::SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
    ASSERT_VALUE_EQ(
        trim->serialize(),
        Value(Document{{"$trim", Document{{"input", Document{{"$const", " abc "sv}}}}}}));

    // Make sure we can re-parse it and evaluate it.
    auto reparsedTrim = Expression::parseExpression(
        &expCtx, trim->serialize().getDocument().toBson(), expCtx.variablesParseState);
    ASSERT_VALUE_EQ(reparsedTrim->evaluate({}, &expCtx.variables), Value("abc"sv));

    // Use $ltrim, and specify the 'chars' option.
    trim = Expression::parseExpression(&expCtx,
                                       BSON("$ltrim" << BSON("input" << "$inputField"
                                                                     << "chars"
                                                                     << "$$CURRENT.a")),
                                       expCtx.variablesParseState);
    ASSERT_VALUE_EQ(
        trim->serialize(),
        Value(Document{{"$ltrim", Document{{"input", "$inputField"sv}, {"chars", "$a"sv}}}}));

    // Make sure we can re-parse it and evaluate it.
    reparsedTrim = Expression::parseExpression(
        &expCtx, trim->serialize().getDocument().toBson(), expCtx.variablesParseState);
    ASSERT_VALUE_EQ(reparsedTrim->evaluate(Document{{"inputField", " , 4"sv}, {"a", " ,"sv}},
                                           &expCtx.variables),
                    Value("4"sv));
}
}  // namespace Trim

}  // namespace ExpressionTests
}  // namespace mongo
