/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "document_value/document_value_test_util.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/projection_policies.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/unittest/inline_auto_update.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
std::unique_ptr<projection_executor::ProjectionExecutor> compileProjection(BSONObj proj) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto policies = ProjectionPolicies::findProjectionPolicies();
    auto ast = projection_ast::parseAndAnalyze(expCtx, proj, policies);
    return projection_executor::buildProjectionExecutor(
        expCtx, &ast, policies, projection_executor::kDefaultBuilderParams);
}
std::unique_ptr<projection_executor::ProjectionExecutor> compileProjection(BSONObj proj,
                                                                           BSONObj query) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto match = uassertStatusOK(MatchExpressionParser::parse(query, expCtx));
    auto policies = ProjectionPolicies::findProjectionPolicies();
    auto ast = projection_ast::parseAndAnalyze(expCtx, proj, match.get(), query, policies);
    auto exec = projection_executor::buildProjectionExecutor(
        expCtx, &ast, policies, projection_executor::kDefaultBuilderParams);
    return exec;
}
std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

TEST(Redaction, ProjectionTest) {
    SerializationOptions options;
    options.replacementForLiteralArgs = "?";
    options.redactIdentifiers = true;

    options.identifierRedactionPolicy = redactFieldNameForTest;
    auto redactProj = [&](std::string obj) {
        return compileProjection(fromjson(obj))->serializeTransformation(boost::none, options);
    };

    /// Inclusion projections

    // Simple single inclusion
    auto actual = redactProj("{a: 1}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":true})",
        actual);

    actual = redactProj("{a: true}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":true})",
        actual);

    // Dotted path
    actual = redactProj("{\"a.b\": 1}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":{"HASH<b>":true}})",
        actual);

    // Two fields
    actual = redactProj("{a: 1, b: 1}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":true,"HASH<b>":true})",
        actual);

    // Explicit _id: 1
    actual = redactProj("{b: 1, _id: 1}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<b>":true})",
        actual);

    // Two nested fields
    actual = redactProj("{\"b.d\": 1, \"b.c\": 1}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<b>":{"HASH<d>":true,"HASH<c>":true}})",
        actual);

    actual = redactProj("{\"b.d\": 1, a: 1, \"b.c\": 1}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "HASH<_id>": true,
            "HASH<a>": true,
            "HASH<b>": {
                "HASH<d>": true,
                "HASH<c>": true
            }
        })",
        actual);


    /// Exclusion projections

    // Simple single exclusion
    actual = redactProj("{a: 0}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<a>":false,"HASH<_id>":true})",
        actual);

    // Dotted path
    actual = redactProj("{\"a.b\": 0}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<a>":{"HASH<b>":false},"HASH<_id>":true})",
        actual);

    // Two fields
    actual = redactProj("{a: 0, b: 0}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<a>":false,"HASH<b>":false,"HASH<_id>":true})",
        actual);

    // Explicit _id: 0
    actual = redactProj("{b: 0, _id: 0}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":false,"HASH<b>":false})",
        actual);

    // Two nested fields
    actual = redactProj("{\"b.d\": 0, \"b.c\": 0}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<b>":{"HASH<d>":false,"HASH<c>":false},"HASH<_id>":true})",
        actual);

    actual = redactProj("{\"b.d\": 0, a: 0, \"b.c\": 0}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "HASH<a>": false,
            "HASH<b>": {
                "HASH<d>": false,
                "HASH<c>": false
            },
            "HASH<_id>": true
        })",
        actual);

    /// Add fields projection
    actual = redactProj("{a: \"hi\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":{"$const":"?"}})",
        actual);

    actual = redactProj("{a: '$field'}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":"$HASH<field>"})",
        actual);

    // Dotted path
    actual = redactProj("{\"a.b\": \"hi\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":{"HASH<b>":{"$const":"?"}}})",
        actual);

    // Two fields
    actual = redactProj("{a: \"hi\", b: \"hello\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":{"$const":"?"},"HASH<b>":{"$const":"?"}})",
        actual);

    // Explicit _id: 0
    actual = redactProj("{b: \"hi\", _id: \"hey\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<b>":{"$const":"?"},"HASH<_id>":{"$const":"?"}})",
        actual);

    // Two nested fields
    actual = redactProj("{\"b.d\": \"hello\", \"b.c\": \"world\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "HASH<_id>": true,
            "HASH<b>": {
                "HASH<d>": {
                    "$const": "?"
                },
                "HASH<c>": {
                    "$const": "?"
                }
            }
        })",
        actual);

    actual = redactProj("{\"b.d\": \"hello\", a: \"world\", \"b.c\": \"mongodb\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
            "HASH<_id>": true,
            "HASH<b>": {
                "HASH<d>": {
                    "$const": "?"
                },
                "HASH<c>": {
                    "$const": "?"
                }
            },
            "HASH<a>": {
                "$const": "?"
            }
        })",
        actual);
}

}  // namespace
}  // namespace mongo
