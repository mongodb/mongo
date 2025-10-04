/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_find_internal.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <set>

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
                                       "$$" + kProjectionPostImageVarName,
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
        expr->evaluate(Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}, &getExpCtx()->variables)
            .getDocument());
}

class ExpressionInternalFindSliceTest : public AggregationContextFixture {
protected:
    auto createExpression(const std::string& path, boost::optional<int> skip, int limit) {
        auto expr = make_intrusive<ExpressionInternalFindSlice>(
            getExpCtxRaw(),
            ExpressionFieldPath::parse(getExpCtxRaw(),
                                       "$$" + kProjectionPostImageVarName,
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
        expr->evaluate(Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}, &getExpCtx()->variables)
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
                                   &getExpCtx()->variables));
}


}  // namespace mongo::expression_evaluate_internal_tests
