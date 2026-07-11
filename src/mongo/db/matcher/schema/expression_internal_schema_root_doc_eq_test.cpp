// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

TEST(InternalSchemaRootDocEqMatchExpression, EquivalentReturnsCorrectResults) {
    auto query = fromjson(R"(
             {$_internalSchemaRootDocEq: {
                 b: 1, c: 1
             }})");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher rootDocEq(std::move(query), expCtx);

    query = fromjson(R"(
             {$_internalSchemaRootDocEq: {
                 c: 1, b: 1
             }})");
    Matcher exprEq(std::move(query), expCtx);
    ASSERT_TRUE(rootDocEq.getMatchExpression()->equivalent(exprEq.getMatchExpression()));

    query = fromjson(R"(
             {$_internalSchemaRootDocEq: {
                 c: 1
             }})");
    Matcher exprNotEq(std::move(query), expCtx);
    ASSERT_FALSE(rootDocEq.getMatchExpression()->equivalent(exprNotEq.getMatchExpression()));
}

TEST(InternalSchemaRootDocEqMatchExpression, EquivalentToClone) {
    auto query = fromjson("{$_internalSchemaRootDocEq: {a:1, b: {c: 1, d: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    Matcher rootDocEq(std::move(query), expCtx);

    auto clone = rootDocEq.getMatchExpression()->clone();
    ASSERT_TRUE(rootDocEq.getMatchExpression()->equivalent(clone.get()));
}

DEATH_TEST_REGEX(InternalSchemaRootDocEqMatchExpressionDeathTest,
                 GetChildFailsIndexLargerThanZero,
                 "Tripwire assertion.*6400218") {
    InternalSchemaRootDocEqMatchExpression rootDocEq(BSON("a" << 1 << "b" << BSON("c" << 1)));

    ASSERT_EQ(rootDocEq.numChildren(), 0);
    ASSERT_THROWS_CODE(rootDocEq.getChild(0), AssertionException, 6400218);
}

}  // namespace
}  // namespace mongo
