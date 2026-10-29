// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_find_internal.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <set>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::expression_internal_tests {
constexpr auto kProjectionPostImageVarName =
    projection_executor::ProjectionExecutor::kProjectionPostImageVarName;

auto defineAndSetProjectionPostImageVariable(ExpressionContext* const expCtx, Value postImage) {
    auto& vps = expCtx->variablesParseState;
    auto varId = vps.defineVariable(kProjectionPostImageVarName);
    expCtx->variables.setValue(varId, postImage);
    return varId;
}

class ExpressionInternalFindPositionalTest : public AggregationContextFixture {
protected:
    auto createExpression(BSONObj matchSpec, const std::string& path) {
        auto matchExpr = CopyableMatchExpression{matchSpec,
                                                 getExpCtxRaw(),
                                                 std::make_unique<ExtensionsCallbackNoop>(),
                                                 MatchExpressionParser::kBanAllSpecialFeatures};
        auto expr = make_intrusive<ExpressionInternalFindPositional>(
            getExpCtxRaw(),
            ExpressionFieldPath::parse(getExpCtxRaw(), "$$ROOT", getExpCtx()->variablesParseState),
            ExpressionFieldPath::parse(getExpCtxRaw(),
                                       fmt::format("$${}", kProjectionPostImageVarName),
                                       getExpCtx()->variablesParseState),
            path,
            std::move(matchExpr));
        return expr;
    }
};

class ExpressionInternalFindSliceTest : public AggregationContextFixture {
protected:
    auto createExpression(const std::string& path, boost::optional<int> skip, int limit) {
        auto expr = make_intrusive<ExpressionInternalFindSlice>(
            getExpCtxRaw(),
            ExpressionFieldPath::parse(getExpCtxRaw(),
                                       fmt::format("$${}", kProjectionPostImageVarName),
                                       getExpCtx()->variablesParseState),
            path,
            skip,
            limit);
        return expr;
    }
};

class ExpressionInternalFindElemMatchTest : public AggregationContextFixture {
protected:
    auto createExpression(BSONObj matchSpec, const std::string& path) {
        auto matchExpr = CopyableMatchExpression{matchSpec,
                                                 getExpCtxRaw(),
                                                 std::make_unique<ExtensionsCallbackNoop>(),
                                                 MatchExpressionParser::kBanAllSpecialFeatures};

        return make_intrusive<ExpressionInternalFindElemMatch>(
            getExpCtxRaw(),
            ExpressionFieldPath::parse(getExpCtxRaw(), "$$ROOT", getExpCtx()->variablesParseState),
            path,
            std::move(matchExpr));
    }
};

TEST_F(ExpressionInternalFindPositionalTest, RecordsProjectionDependencies) {
    auto varId = defineAndSetProjectionPostImageVariable(
        getExpCtxRaw(), Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});
    auto expr = createExpression(fromjson("{bar: 1, foo: {$gte: 5}}"), "foo");

    DepsTracker deps;
    expression::addDependencies(expr.get(), &deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT_TRUE(deps.needWholeDocument);

    std::set<Variables::Id> refs;
    expression::addVariableRefs(expr.get(), &refs);
    ASSERT_EQ(refs.size(), 1UL);
    ASSERT_EQ(refs.count(varId), 1UL);
}

TEST_F(ExpressionInternalFindPositionalTest, AddsArrayUndottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtxRaw(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression(fromjson("{bar: 1, foo: {$gte: 5}}"), "foo");

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindPositionalTest,
       AddsOnlyTopLevelFieldOfArrayDottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtxRaw(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression(fromjson("{bar: 1, 'foo.bar': {$gte: 5}}"), "foo.bar");

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindSliceTest, RecordsProjectionDependencies) {
    auto varId = defineAndSetProjectionPostImageVariable(
        getExpCtxRaw(), Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});
    auto expr = createExpression("foo", 1, 2);

    DepsTracker deps;
    expression::addDependencies(expr.get(), &deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT_TRUE(deps.needWholeDocument);

    std::set<Variables::Id> refs;
    expression::addVariableRefs(expr.get(), &refs);
    ASSERT_EQ(refs.size(), 1UL);
    ASSERT_EQ(refs.count(varId), 1UL);
}

TEST_F(ExpressionInternalFindSliceTest, AddsArrayUndottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtxRaw(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression("foo", 1, 2);

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindSliceTest, AddsTopLevelFieldOfArrayDottedPathToComputedPaths) {
    defineAndSetProjectionPostImageVariable(getExpCtxRaw(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression("foo.bar", 1, 2);

    DepsTracker deps;
    auto computedPaths = expr->getComputedPaths({});

    ASSERT_EQ(computedPaths.paths.size(), 1UL);
    ASSERT_EQ(computedPaths.renames.size(), 0UL);
    ASSERT_EQ(computedPaths.paths.count("foo"), 1UL);
}

TEST_F(ExpressionInternalFindElemMatchTest, RecordsProjectionDependencies) {
    auto expr = createExpression(fromjson("{foo: {$elemMatch: {bar: {$gte: 5}}}}"), "foo");

    DepsTracker deps;
    expression::addDependencies(expr.get(), &deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT(deps.needWholeDocument);

    std::set<Variables::Id> refs;
    expression::addVariableRefs(expr.get(), &refs);
    ASSERT_EQ(refs.size(), 0UL);
}
}  // namespace mongo::expression_internal_tests
