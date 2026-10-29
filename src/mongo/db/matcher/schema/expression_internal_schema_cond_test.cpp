// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
/**
 * Helper function for parsing and creating MatchExpressions.
 */
std::unique_ptr<InternalSchemaCondMatchExpression> createCondMatchExpression(BSONObj condition,
                                                                             BSONObj thenBranch,
                                                                             BSONObj elseBranch) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto conditionExpr = MatchExpressionParser::parse(condition, expCtx);
    ASSERT_OK(conditionExpr.getStatus());
    auto thenBranchExpr = MatchExpressionParser::parse(thenBranch, expCtx);
    ASSERT_OK(thenBranchExpr.getStatus());
    auto elseBranchExpr = MatchExpressionParser::parse(elseBranch, expCtx);

    std::array<std::unique_ptr<MatchExpression>, 3> expressions = {
        {std::move(conditionExpr.getValue()),
         std::move(thenBranchExpr.getValue()),
         std::move(elseBranchExpr.getValue())}};

    auto cond = std::make_unique<InternalSchemaCondMatchExpression>(std::move(expressions));

    return cond;
}

TEST(InternalSchemaCondMatchExpressionTest, EquivalentReturnsCorrectResults) {
    auto conditionQuery1 = BSON("foo" << 1);
    auto thenQuery1 = BSON("bar" << 2);
    auto elseQuery1 = BSON("qux" << 3);
    auto cond1 = createCondMatchExpression(conditionQuery1, thenQuery1, elseQuery1);

    auto conditionQuery2 = BSON("foo" << 1);
    auto thenQuery2 = BSON("bar" << 2);
    auto elseQuery2 = BSON("qux" << 3);
    auto cond2 = createCondMatchExpression(conditionQuery2, thenQuery2, elseQuery2);

    auto conditionQuery3 = BSON("foo" << 9001);
    auto thenQuery3 = BSON("bar" << 2);
    auto elseQuery3 = BSON("qux" << 3);
    auto cond3 = createCondMatchExpression(conditionQuery3, thenQuery3, elseQuery3);

    ASSERT_TRUE(cond1->equivalent(cond2.get()));
    ASSERT_TRUE(cond2->equivalent(cond1.get()));

    ASSERT_FALSE(cond1->equivalent(cond3.get()));
    ASSERT_FALSE(cond3->equivalent(cond2.get()));
}

TEST(InternalSchemaCondMatchExpressionTest, EquivalentToClone) {
    auto conditionQuery = BSON("likes" << "cats");
    auto thenQuery = BSON("pets" << BSON("$lte" << 1));
    auto elseQuery = BSON("interests" << "dogs");
    auto cond = createCondMatchExpression(conditionQuery, thenQuery, elseQuery);
    auto clone = cond->clone();
    ASSERT_TRUE(cond->equivalent(clone.get()));
}
}  // namespace
}  // namespace mongo
