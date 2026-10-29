// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::expression_evaluate_internal_tests {

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
                                       "$$" + std::string(kProjectionPostImageVarName),
                                       getExpCtx()->variablesParseState),
            path,
            std::move(matchExpr));
        return expr;
    }
};

TEST_F(ExpressionInternalFindPositionalTest, AppliesProjectionToPostImage) {
    defineAndSetProjectionPostImageVariable(getExpCtxRaw(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression(fromjson("{bar: 1, foo: {$gte: 5}}"), "foo");

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{bar:1, foo: [6]}")},
        expr->evaluate(Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}, &getExpCtx()->variables, {})
            .getDocument());
}

class ExpressionInternalFindSliceTest : public AggregationContextFixture {
protected:
    auto createExpression(const std::string& path, boost::optional<int> skip, int limit) {
        auto expr = make_intrusive<ExpressionInternalFindSlice>(
            getExpCtxRaw(),
            ExpressionFieldPath::parse(getExpCtxRaw(),
                                       "$$" + std::string(kProjectionPostImageVarName),
                                       getExpCtx()->variablesParseState),
            path,
            skip,
            limit);
        return expr;
    }
};

TEST_F(ExpressionInternalFindSliceTest, AppliesProjectionToPostImage) {
    defineAndSetProjectionPostImageVariable(getExpCtxRaw(),
                                            Value{fromjson("{bar: 1, foo: [1,2,6,10]}")});

    auto expr = createExpression("foo", 1, 2);

    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{bar: 1, foo: [2,6]}")},
        expr->evaluate(Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}, &getExpCtx()->variables, {})
            .getDocument());
}

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

TEST_F(ExpressionInternalFindElemMatchTest, AppliesProjectionToRootDocument) {
    auto expr = createExpression(fromjson("{foo: {$elemMatch: {bar: {$gte: 5}}}}"), "foo");

    ASSERT_VALUE_EQ(Document{fromjson("{foo: [{bar: 6, z: 6}]}")}["foo"],
                    expr->evaluate(Document{fromjson("{foo: [{bar: 1, z: 1}, {bar: 2, z: 2}, "
                                                     "{bar: 6, z: 6}, {bar: 10, z: 10}]}")},
                                   &getExpCtx()->variables,
                                   {}));
}


}  // namespace mongo::expression_evaluate_internal_tests
