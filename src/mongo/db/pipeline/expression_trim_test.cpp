/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
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
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace ExpressionTests {
using std::string;

namespace Trim {

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
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));

    // Test that it optimizes to a constant if the input also optimizes to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input" << BSON("$concat" << BSON_ARRAY(" " << "abc ")))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));
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
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));

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
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));

    // Test that it optimizes to a constant if both arguments optimize to a constant.
    trim = Expression::parseExpression(
        &expCtx,
        BSON("$trim" << BSON("input" << BSON("$concat" << BSON_ARRAY(" " << "abc ")) << "chars"
                                     << BSON("$substrCP" << BSON_ARRAY("  " << 1 << 1)))),
        expCtx.variablesParseState);
    optimized = trim->optimize();
    constant = dynamic_cast<ExpressionConstant*>(optimized.get());
    ASSERT_TRUE(constant);
    ASSERT_VALUE_EQ(constant->getValue(), Value("abc"_sd));
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
        trim->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}));
    ASSERT_VALUE_EQ(
        trim->serialize(),
        Value(Document{{"$trim", Document{{"input", Document{{"$const", " abc "_sd}}}}}}));

    // Make sure we can re-parse it and evaluate it.
    auto reparsedTrim = Expression::parseExpression(
        &expCtx, trim->serialize().getDocument().toBson(), expCtx.variablesParseState);
    ASSERT_VALUE_EQ(reparsedTrim->evaluate({}, &expCtx.variables), Value("abc"_sd));

    // Use $ltrim, and specify the 'chars' option.
    trim = Expression::parseExpression(&expCtx,
                                       BSON("$ltrim" << BSON("input" << "$inputField"
                                                                     << "chars"
                                                                     << "$$CURRENT.a")),
                                       expCtx.variablesParseState);
    ASSERT_VALUE_EQ(
        trim->serialize(),
        Value(Document{{"$ltrim", Document{{"input", "$inputField"_sd}, {"chars", "$a"_sd}}}}));

    // Make sure we can re-parse it and evaluate it.
    reparsedTrim = Expression::parseExpression(
        &expCtx, trim->serialize().getDocument().toBson(), expCtx.variablesParseState);
    ASSERT_VALUE_EQ(reparsedTrim->evaluate(Document{{"inputField", " , 4"_sd}, {"a", " ,"_sd}},
                                           &expCtx.variables),
                    Value("4"_sd));
}
}  // namespace Trim

}  // namespace ExpressionTests
}  // namespace mongo
