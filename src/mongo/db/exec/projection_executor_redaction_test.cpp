// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <string>


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

TEST(Redaction, ProjectionTest) {
    query_shape::SerializationOptions options =
        query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST;
    auto redactProj = [&](std::string obj) {
        return compileProjection(fromjson(obj))->serializeTransformation(options);
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
        R"({"HASH<_id>":true,"HASH<a>":"?string"})",
        actual);

    actual = redactProj("{a: '$field'}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":"$HASH<field>"})",
        actual);

    // Dotted path
    actual = redactProj("{\"a.b\": \"hi\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":{"HASH<b>":"?string"}})",
        actual);

    // Two fields
    actual = redactProj("{a: \"hi\", b: \"hello\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<a>":"?string","HASH<b>":"?string"})",
        actual);

    // Explicit _id: 0
    actual = redactProj("{b: \"hi\", _id: \"hey\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<b>":"?string","HASH<_id>":"?string"})",
        actual);

    // Two nested fields
    actual = redactProj("{\"b.d\": \"hello\", \"b.c\": \"world\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({"HASH<_id>":true,"HASH<b>":{"HASH<d>":"?string","HASH<c>":"?string"}})",
        actual);

    actual = redactProj("{\"b.d\": \"hello\", a: \"world\", \"b.c\": \"mongodb\"}");
    ASSERT_DOCUMENT_EQ_AUTO(  // NOLINT
        R"({
             "HASH<_id>": true,
             "HASH<b>": {
                 "HASH<d>": "?string",
                 "HASH<c>": "?string"
             },
             "HASH<a>": "?string"
         })",
        actual);
}
}  // namespace
}  // namespace mongo
