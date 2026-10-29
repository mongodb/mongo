// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

TEST(ExpressionToHashedIndexKeyTest, StringInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << "hashThisStringLiteral"sv);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-5776344739422278694));
}

TEST(ExpressionToHashedIndexKeyTest, IntInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << 123);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-6548868637522515075));
}

TEST(ExpressionToHashedIndexKeyTest, TimestampInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << Timestamp(0, 0));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-7867208682377458672));
}

TEST(ExpressionToHashedIndexKeyTest, ObjectIdInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << OID("47cc67093475061e3d95369d"));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(1576265281381834298));
}

TEST(ExpressionToHashedIndexKeyTest, DateInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << Date_t());
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(-1178696894582842035));
}

TEST(ExpressionToHashedIndexKeyTest, MissingInputValueSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << "$missingField");
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2338878944348059895));
}

TEST(ExpressionToHashedIndexKeyTest, NullInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSONNULL);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2338878944348059895));
}

TEST(ExpressionToHashedIndexKeyTest, ExpressionInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSON("$pow" << BSON_ARRAY(2 << 4)));
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(2598032665634823220));
}

TEST(ExpressionToHashedIndexKeyTest, UndefinedInputSucceeds) {
    auto expCtx = ExpressionContextForTest{};
    const BSONObj obj = BSON("$toHashedIndexKey" << BSONUndefined);
    auto expression = Expression::parseExpression(&expCtx, obj, expCtx.variablesParseState);
    Value result = expression->evaluate({}, &expCtx.variables);
    ASSERT_VALUE_EQ(result, Value::createIntOrLong(40158834000849533LL));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
