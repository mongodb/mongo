/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
