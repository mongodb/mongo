// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(InternalSchemaEqMatchExpression, EquivalentReturnsCorrectResults) {
    auto query = fromjson(R"(
             {a: {$_internalSchemaEq: {
                 b: {c: 1, d: 1}
             }}})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher eqExpr(query, expCtx);

    query = fromjson(R"(
             {a: {$_internalSchemaEq: {
                 b: {d: 1, c: 1}
             }}})");
    Matcher eqExprEq(query, expCtx);
    ASSERT_TRUE(eqExpr.getMatchExpression()->equivalent(eqExprEq.getMatchExpression()));

    query = fromjson(R"(
             {a: {$_internalSchemaEq: {
                 b: {d: 1}
             }}})");
    Matcher eqExprNotEq(query, expCtx);
    ASSERT_FALSE(eqExpr.getMatchExpression()->equivalent(eqExprNotEq.getMatchExpression()));
}

TEST(InternalSchemaEqMatchExpression, EquivalentToClone) {
    auto query = fromjson("{a: {$_internalSchemaEq: {a:1, b: {c: 1, d: [1]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher rootDocEq(query, expCtx);
    auto clone = rootDocEq.getMatchExpression()->clone();
    ASSERT_TRUE(rootDocEq.getMatchExpression()->equivalent(clone.get()));
}

DEATH_TEST_REGEX(InternalSchemaEqMatchExpressionDeathTest,
                 GetChildFailsLargerThanZero,
                 "Tripwire assertion.*6400213") {
    BSONObj operand = BSON("a" << 5);
    InternalSchemaEqMatchExpression eq("a"sv, operand["a"]);

    ASSERT_EQ(eq.numChildren(), 0);
    ASSERT_THROWS_CODE(eq.getChild(0), AssertionException, 6400213);
}

}  // namespace
}  // namespace mongo
