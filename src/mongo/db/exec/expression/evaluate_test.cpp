// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;

/* ------------------------ Constant -------------------- */

TEST(ExpressionConstantTest, Create) {
    /** Create an ExpressionConstant from a Value. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value(5));
    ASSERT_BSONOBJ_BINARY_EQ(BSON("" << 5), toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionConstantTest, CreateFromBsonElement) {
    /** Create an ExpressionConstant from a BsonElement. */
    BSONObj spec = BSON("IGNORED_FIELD_NAME" << "foo");
    auto expCtx = ExpressionContextForTest{};
    BSONElement specElement = spec.firstElement();
    VariablesParseState vps = expCtx.variablesParseState;
    intrusive_ptr<Expression> expression = ExpressionConstant::parse(&expCtx, specElement, vps);
    ASSERT_BSONOBJ_BINARY_EQ(BSON("" << "foo"),
                             toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST(ExpressionConstantTest, ConstantOfValueMissingRemovesField) {
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression = ExpressionConstant::create(&expCtx, Value());
    ASSERT_BSONOBJ_BINARY_EQ(
        BSONObj(),
        toBson(expression->evaluate(Document{{"foo", Value("bar"sv)}}, &expCtx.variables, {})));
}

namespace BuiltinRemoveVariable {

TEST(BuiltinRemoveVariableTest, TypeOfRemoveIsMissing) {
    assertExpectedResults("$type", {{{Value("$$REMOVE"sv)}, Value("missing"sv)}});
}

TEST(BuiltinRemoveVariableTest, LiteralEscapesRemoveVar) {
    assertExpectedResults(
        "$literal", {{{Value("$$REMOVE"sv)}, Value(std::vector<Value>{Value("$$REMOVE"sv)})}});
}

}  // namespace BuiltinRemoveVariable

namespace NowAndClusterTime {
TEST(NowAndClusterTime, BasicTest) {
    auto expCtx = ExpressionContextForTest{};

    // $$NOW is the Date type.
    {
        auto expression = ExpressionFieldPath::parse(&expCtx, "$$NOW", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});
        ASSERT_EQ(result.getType(), BSONType::date);
    }
    // $$CLUSTER_TIME is the timestamp type.
    {
        auto expression =
            ExpressionFieldPath::parse(&expCtx, "$$CLUSTER_TIME", expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});
        ASSERT_EQ(result.getType(), BSONType::timestamp);
    }

    // Multiple references to $$NOW must return the same value.
    {
        auto expression = Expression::parseExpression(
            &expCtx, fromjson("{$eq: [\"$$NOW\", \"$$NOW\"]}"), expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});

        ASSERT_VALUE_EQ(result, Value{true});
    }
    // Same is true for the $$CLUSTER_TIME.
    {
        auto expression =
            Expression::parseExpression(&expCtx,
                                        fromjson("{$eq: [\"$$CLUSTER_TIME\", \"$$CLUSTER_TIME\"]}"),
                                        expCtx.variablesParseState);
        Value result = expression->evaluate(Document(), &expCtx.variables, {});

        ASSERT_VALUE_EQ(result, Value{true});
    }
}
}  // namespace NowAndClusterTime
}  // namespace expression_evaluation_test
}  // namespace mongo
