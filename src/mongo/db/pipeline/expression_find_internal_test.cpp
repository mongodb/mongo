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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/pipeline/parsed_aggregation_projection.h"
#include "mongo/unittest/unittest.h"

namespace mongo::expression_internal_tests {
constexpr auto kProjectionPostImageVarName =
    parsed_aggregation_projection::ParsedAggregationProjection::kProjectionPostImageVarName;

auto defineAndSetProjectionPostImageVariable(boost::intrusive_ptr<ExpressionContext> expCtx,
                                             Value postImage) {
    auto& vps = expCtx->variablesParseState;
    auto varId = vps.defineVariable(kProjectionPostImageVarName);
    expCtx->variables.setValue(varId, postImage);
    return varId;
}

class ExpressionInternalFindPositionalTest : public AggregationContextFixture {
protected:
    auto createExpression(const MatchExpression* matchExpr, const std::string& path) {
        auto expr = make_intrusive<ExpressionInternalFindPositional>(
            getExpCtx(),
            ExpressionFieldPath::parse(getExpCtx(), "$$ROOT", getExpCtx()->variablesParseState),
            ExpressionFieldPath::parse(
                getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
            path,
            matchExpr);
        return expr;
    }
};

class ExpressionInternalFindSliceTest : public AggregationContextFixture {
protected:
    auto createExpression(const std::string& path, boost::optional<int> skip, int limit) {
        auto expr = make_intrusive<ExpressionInternalFindSlice>(
            getExpCtx(),
            ExpressionFieldPath::parse(
                getExpCtx(), "$$" + kProjectionPostImageVarName, getExpCtx()->variablesParseState),
            path,
            skip,
            limit);
        return expr;
    }
};

class ExpressionInternalFindElemMatchTest : public AggregationContextFixture {
protected:
    auto createExpression(std::unique_ptr<MatchExpression> matchExpr, const std::string& path) {
        return make_intrusive<ExpressionInternalFindElemMatch>(
            getExpCtx(),
            ExpressionFieldPath::parse(getExpCtx(), "$$ROOT", getExpCtx()->variablesParseState),
            path,
            std::move(matchExpr));
    }
};

TEST_F(ExpressionInternalFindPositionalTest, AppliesProjectionToPostImage) {
    defineAndSetProjectionPostImageVariable(getExpCtx(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto matchSpec = fromjson("{bar: 1, foo: {$gte: 5}}");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(matchSpec, getExpCtx()));
    auto expr = createExpression(matchExpr.get(), "foo");

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{bar:1, foo: [6]}")},
        expr->evaluate(Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}, &getExpCtx()->variables)
            .getDocument());
}

TEST_F(ExpressionInternalFindPositionalTest, RecordsProjectionDependencies) {
    auto varId = defineAndSetProjectionPostImageVariable(
        getExpCtx(), Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});
    auto matchSpec = fromjson("{bar: 1, foo: {$gte: 5}}");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(matchSpec, getExpCtx()));
    auto expr = createExpression(matchExpr.get(), "foo");

    DepsTracker deps;
    expr->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 2UL);
    ASSERT_EQ(deps.fields.count("bar"), 1UL);
    ASSERT_EQ(deps.fields.count("foo"), 1UL);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(deps.vars.count(varId), 1UL);
    ASSERT_TRUE(deps.needWholeDocument);
}

TEST_F(ExpressionInternalFindPositionalTest, AddsArrayUndottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtx(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto matchSpec = fromjson("{bar: 1, foo: {$gte: 5}}");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(matchSpec, getExpCtx()));
    auto expr = createExpression(matchExpr.get(), "foo");

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindPositionalTest,
       AddsOnlyTopLevelFieldOfArrayDottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtx(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto matchSpec = fromjson("{bar: 1, 'foo.bar': {$gte: 5}}");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(matchSpec, getExpCtx()));
    auto expr = createExpression(matchExpr.get(), "foo.bar");

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindSliceTest, AppliesProjectionToPostImage) {
    defineAndSetProjectionPostImageVariable(getExpCtx(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression("foo", 1, 2);

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{bar: 1, foo: [2,6]}")},
        expr->evaluate(Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}, &getExpCtx()->variables)
            .getDocument());
}

TEST_F(ExpressionInternalFindSliceTest, RecordsProjectionDependencies) {
    auto varId = defineAndSetProjectionPostImageVariable(
        getExpCtx(), Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});
    auto expr = createExpression("foo", 1, 2);

    DepsTracker deps;
    expr->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT_EQ(deps.vars.size(), 1UL);
    ASSERT_EQ(deps.vars.count(varId), 1UL);
    ASSERT_TRUE(deps.needWholeDocument);
}

TEST_F(ExpressionInternalFindSliceTest, AddsArrayUndottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtx(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression("foo", 1, 2);

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindSliceTest, AddsTopLevelFieldOfArrayDottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtx(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression("foo.bar", 1, 2);

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindElemMatchTest, AppliesProjectionToRootDocument) {
    auto matchSpec = fromjson("{foo: {$elemMatch: {bar: {$gte: 5}}}}");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(matchSpec, getExpCtx()));
    auto expr = createExpression(std::move(matchExpr), "foo");

    ASSERT_VALUE_EQ(Document{fromjson("{foo: [{bar: 6, z: 6}]}")}["foo"],
                    expr->evaluate(Document{fromjson("{foo: [{bar: 1, z: 1}, {bar: 2, z: 2}, "
                                                     "{bar: 6, z: 6}, {bar: 10, z: 10}]}")},
                                   &getExpCtx()->variables));
}

TEST_F(ExpressionInternalFindElemMatchTest, RecordsProjectionDependencies) {
    auto matchSpec = fromjson("{foo: {$elemMatch: {bar: {$gte: 5}}}}");
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(matchSpec, getExpCtx()));
    auto expr = createExpression(std::move(matchExpr), "foo");

    DepsTracker deps;
    expr->addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 1UL);
    ASSERT_EQ(deps.fields.count("foo"), 1UL);
    ASSERT_EQ(deps.vars.size(), 0UL);
    ASSERT(deps.needWholeDocument);
}
}  // namespace mongo::expression_internal_tests
