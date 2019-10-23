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

#include <memory>

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace {
using std::string;
using std::unique_ptr;
using std::vector;

using namespace mongo;

using projection_ast::Projection;

/**
 * Helper for creating projections.
 */
projection_ast::Projection createProjection(const BSONObj& query,
                                            const BSONObj& projObj,
                                            ProjectionPolicies policies = {}) {
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    const CollatorInterface* collator = nullptr;
    const boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx.get(), collator));
    StatusWithMatchExpression statusWithMatcher =
        MatchExpressionParser::parse(query, std::move(expCtx));
    ASSERT_OK(statusWithMatcher.getStatus());
    std::unique_ptr<MatchExpression> queryMatchExpr = std::move(statusWithMatcher.getValue());
    projection_ast::Projection res =
        projection_ast::parse(expCtx, projObj, queryMatchExpr.get(), query, policies);

    return res;
}

projection_ast::Projection createProjection(const char* queryStr, const char* projStr) {
    BSONObj query = fromjson(queryStr);
    BSONObj projObj = fromjson(projStr);
    return createProjection(query, projObj);
}

projection_ast::Projection createFindProjection(const char* queryStr, const char* projStr) {
    BSONObj query = fromjson(queryStr);
    BSONObj projObj = fromjson(projStr);
    return createProjection(query, projObj, ProjectionPolicies::findProjectionPolicies());
}

void assertInvalidProjection(const char* queryStr, const char* projStr) {
    BSONObj query = fromjson(queryStr);
    BSONObj projObj = fromjson(projStr);
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    const CollatorInterface* collator = nullptr;
    const boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx.get(), collator));
    StatusWithMatchExpression statusWithMatcher =
        MatchExpressionParser::parse(query, std::move(expCtx));
    ASSERT_OK(statusWithMatcher.getStatus());
    std::unique_ptr<MatchExpression> queryMatchExpr = std::move(statusWithMatcher.getValue());
    ASSERT_THROWS(
        projection_ast::parse(expCtx, projObj, queryMatchExpr.get(), query, ProjectionPolicies{}),
        DBException);
}

TEST(QueryProjectionTest, MakeEmptyProjection) {
    Projection proj(createProjection("{}", "{}"));
    ASSERT_TRUE(proj.requiresDocument());
}

TEST(QueryProjectionTest, MakeEmptyProjectionExcludeId) {
    Projection proj(createProjection("{}", "{_id: 0}"));
    ASSERT_TRUE(proj.requiresDocument());
}

TEST(QueryProjectionTest, MakeSingleFieldInclusion) {
    Projection proj(createProjection("{}", "{a: 1}"));
    ASSERT_FALSE(proj.requiresDocument());
    const auto& fields = proj.getRequiredFields();
    ASSERT_EQUALS(fields.size(), 2U);
    ASSERT_EQUALS(fields[0], "_id");
    ASSERT_EQUALS(fields[1], "a");
}

TEST(QueryProjectionTest, MakeSingleFieldInclusionNoId) {
    Projection proj(createProjection("{}", "{_id: 0, a: 1}"));
    ASSERT_FALSE(proj.requiresDocument());
    const auto& fields = proj.getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "a");
}

TEST(QueryProjectionTest, MakeSingleFieldId) {
    Projection proj(createProjection("{}", "{_id: 1}"));
    ASSERT_FALSE(proj.requiresDocument());
    const auto& fields = proj.getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "_id");
}

TEST(QueryProjectionTest, MakeSingleFieldNoIdBoolean) {
    Projection proj(createProjection("{}", "{_id: 0, a: true}"));
    ASSERT_FALSE(proj.requiresDocument());
    const auto& fields = proj.getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "a");
}

TEST(QueryProjectionTest, MakeSingleFieldFalseIdBoolean) {
    Projection proj(createProjection("{}", "{_id: false, a: 1}"));
    ASSERT_FALSE(proj.requiresDocument());
    const auto& fields = proj.getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "a");
}

//
// Positional operator validation.
//

TEST(QueryProjectionTest, InvalidPositionalOperatorProjections) {
    assertInvalidProjection("{}", "{'a.$': 1}");
    assertInvalidProjection("{a: 1}", "{'b.$': 1}");
    assertInvalidProjection("{a: 1}", "{'a.$': 0}");
    assertInvalidProjection("{a: 1}", "{'a.$.d.$': 1}");
    assertInvalidProjection("{a: 1}", "{'a.$.$': 1}");
    assertInvalidProjection("{a: 1, b: 1, c: 1}", "{'abc.$': 1}");
    assertInvalidProjection("{$or: [{a: 1}, {$or: [{b: 1}, {c: 1}]}]}", "{'d.$': 1}");
    assertInvalidProjection("{a: [1, 2, 3]}", "{'.$': 1}");
}

TEST(QueryProjectionTest, InvalidElemMatchTextProjection) {
    assertInvalidProjection("{}", "{a: {$elemMatch: {$text: {$search: 'str'}}}}");
}

TEST(QueryProjectionTest, InvalidElemMatchWhereProjection) {
    assertInvalidProjection("{}", "{a: {$elemMatch: {$where: 'this.a == this.b'}}}");
}

TEST(QueryProjectionTest, InvalidElemMatchGeoNearProjection) {
    assertInvalidProjection(
        "{}",
        "{a: {$elemMatch: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}}");
}

TEST(QueryProjectionTest, InvalidElemMatchExprProjection) {
    assertInvalidProjection("{}", "{a: {$elemMatch: {$expr: 5}}}");
}

TEST(QueryProjectionTest, ValidPositionalOperatorProjections) {
    createFindProjection("{a: 1}", "{'a.$': 1}");
    createFindProjection("{a: 1}", "{'a.foo.bar.$': 1}");
    createFindProjection("{'a.b.c': 1}", "{'a.b.c.$': 1}");
    createFindProjection("{'a.b.c': 1}", "{'a.e.f.$': 1}");
    createFindProjection("{a: {b: 1}}", "{'a.$': 1}");
    createFindProjection("{a: 1, b: 1}}", "{'a.$': 1}");
    createFindProjection("{a: 1, b: 1}}", "{'b.$': 1}");
    createFindProjection("{$and: [{a: 1}, {b: 1}]}", "{'a.$': 1}");
    createFindProjection("{$and: [{a: 1}, {b: 1}]}", "{'b.$': 1}");
    createFindProjection("{$or: [{a: 1}, {b: 1}]}", "{'a.$': 1}");
    createFindProjection("{$or: [{a: 1}, {b: 1}]}", "{'b.$': 1}");
    createFindProjection("{$and: [{$or: [{a: 1}, {$and: [{b: 1}, {c: 1}]}]}]}", "{'c.d.f.$': 1}");
}

// Some match expressions (eg. $where) do not override MatchExpression::path()
// In this test case, we use an internal match expression implementation ALWAYS_FALSE
// to achieve the same effect.
// Projection parser should handle this the same way as an empty path.
TEST(QueryProjectionTest, InvalidPositionalProjectionDefaultPathMatchExpression) {
    QueryTestServiceContext serviceCtx;
    auto opCtx = serviceCtx.makeOperationContext();
    const boost::intrusive_ptr<ExpressionContext> expCtx(
        new ExpressionContext(opCtx.get(), nullptr));

    unique_ptr<MatchExpression> queryMatchExpr(new AlwaysFalseMatchExpression());
    ASSERT_EQ(nullptr, queryMatchExpr->path().rawData());

    BSONObj projObj = fromjson("{'a.$': 1}");
    ASSERT_THROWS(projection_ast::parse(
                      expCtx, projObj, queryMatchExpr.get(), BSONObj(), ProjectionPolicies{}),
                  DBException);

    // Projecting onto empty field should fail.
    BSONObj emptyFieldProjObj = fromjson("{'.$': 1}");
    ASSERT_THROWS(
        projection_ast::parse(
            expCtx, emptyFieldProjObj, queryMatchExpr.get(), BSONObj(), ProjectionPolicies{}),
        DBException);
}

TEST(QueryProjectionTest, ProjectionDefaults) {
    auto proj = createProjection("{}", "{}");

    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kSortKey]);
    ASSERT_TRUE(proj.requiresDocument());
    ASSERT_FALSE(proj.requiresMatchDetails());
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kTextScore]);
}

TEST(QueryProjectionTest, SortKeyMetaProjectionInExclusionProjection) {
    // A projection with just a $meta projection defaults to an exclusion projection.
    auto proj = createProjection("{}", "{foo: {$meta: 'sortKey'}}");

    ASSERT_TRUE(proj.metadataDeps()[DocumentMetadataFields::kSortKey]);
    ASSERT_FALSE(proj.requiresMatchDetails());
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
    ASSERT_TRUE(proj.requiresDocument());
}

TEST(QueryProjectionTest, SortKeyMetaProjectionInExclusionProjectionWithOtherFields) {
    auto proj = createProjection("{}", "{a: 0, foo: {$meta: 'sortKey'}}");

    ASSERT_TRUE(proj.metadataDeps()[DocumentMetadataFields::kSortKey]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
    ASSERT_TRUE(proj.requiresDocument());
}

TEST(QueryProjectionTest, SortKeyMetaProjectionInInclusionProjection) {
    auto proj = createProjection("{}", "{a: 1, foo: {$meta: 'sortKey'}}");

    ASSERT_TRUE(proj.metadataDeps()[DocumentMetadataFields::kSortKey]);
    ASSERT_FALSE(proj.requiresMatchDetails());
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
    ASSERT_FALSE(proj.requiresDocument());
}

TEST(QueryProjectionTest, SortKeyMetaProjectionDoesNotRequireDocument) {
    auto proj = createProjection("{}", "{a: 1, foo: {$meta: 'sortKey'}, _id: 0}");

    ASSERT_TRUE(proj.metadataDeps()[DocumentMetadataFields::kSortKey]);
    ASSERT_FALSE(proj.requiresDocument());
    ASSERT_FALSE(proj.requiresMatchDetails());
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
}

TEST(QueryProjectionTest, SortKeyMetaAndSlice) {
    auto proj =
        createFindProjection("{}", "{a: 1, foo: {$meta: 'sortKey'}, _id: 0, b: {$slice: 1}}");

    ASSERT_TRUE(proj.metadataDeps()[DocumentMetadataFields::kSortKey]);
    ASSERT_TRUE(proj.requiresDocument());
    ASSERT_FALSE(proj.requiresMatchDetails());
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
}

TEST(QueryProjectionTest, SortKeyMetaAndElemMatch) {
    auto proj = createFindProjection(
        "{}", "{a: 1, foo: {$meta: 'sortKey'}, _id: 0, b: {$elemMatch: {a: 1}}}");

    ASSERT_TRUE(proj.metadataDeps()[DocumentMetadataFields::kSortKey]);
    ASSERT_TRUE(proj.requiresDocument());
    ASSERT_FALSE(proj.requiresMatchDetails());
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearDist]);
    ASSERT_FALSE(proj.metadataDeps()[DocumentMetadataFields::kGeoNearPoint]);
}

//
// Cases for Projection::isFieldRetainedExactly().
//

TEST(QueryProjectionTest, InclusionProjectionPreservesChild) {
    auto proj = createProjection("{}", "{a: 1}");
    ASSERT_TRUE(proj.isFieldRetainedExactly("a.b"));
}

TEST(QueryProjectionTest, InclusionProjectionDoesNotPreserveParent) {
    auto proj = createProjection("{}", "{'a.b': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, InclusionProjectionPreservesField) {
    auto proj = createProjection("{}", "{a: 1}");
    ASSERT_TRUE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, ExclusionProjectionDoesNotPreserveParent) {
    auto proj = createProjection("{}", "{'a.b': 0}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, ExclusionProjectionDoesNotPreserveChild) {
    auto proj = createProjection("{}", "{a: 0}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a.b"));
}

TEST(QueryProjectionTest, ExclusionProjectionDoesNotPreserveField) {
    auto proj = createProjection("{}", "{a: 0}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, InclusionProjectionDoesNotPreserveNonIncludedFields) {
    auto proj = createProjection("{}", "{a: 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("c"));
}

TEST(QueryProjectionTest, ExclusionProjectionPreservesNonExcludedFields) {
    auto proj = createProjection("{}", "{a: 0}");
    ASSERT_TRUE(proj.isFieldRetainedExactly("c"));
}

TEST(QueryProjectionTest, PositionalProjectionDoesNotPreserveField) {
    auto proj = createFindProjection("{a: {$elemMatch: {$eq: 0}}}", "{'a.$': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, PositionalProjectionDoesNotPreserveChild) {
    auto proj = createFindProjection("{a: {$elemMatch: {$eq: 0}}}", "{'a.$': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a.b"));
}

TEST(QueryProjectionTest, PositionalProjectionDoesNotPreserveParent) {
    auto proj = createFindProjection("{'a.b': {$elemMatch: {$eq: 0}}}", "{'a.b.$': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, MetaProjectionDoesNotPreserveField) {
    auto proj = createProjection("{}", "{a: {$meta: 'textScore'}}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, MetaProjectionDoesNotPreserveChild) {
    auto proj = createProjection("{}", "{a: {$meta: 'textScore'}}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a.b"));
}

TEST(QueryProjectionTest, IdExclusionProjectionPreservesOtherFields) {
    auto proj = createProjection("{}", "{_id: 0}");
    ASSERT_TRUE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, IdInclusionProjectionDoesNotPreserveOtherFields) {
    auto proj = createProjection("{}", "{_id: 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, IdSubfieldExclusionProjectionDoesNotPreserveId) {
    auto proj = createProjection("{}", "{'_id.a': 0}");
    ASSERT_TRUE(proj.isFieldRetainedExactly("b"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("_id"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("_id.a"));
}

TEST(QueryProjectionTest, IdSubfieldInclusionProjectionDoesNotPreserveId) {
    auto proj = createProjection("{}", "{'_id.a': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("b"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("_id"));
    ASSERT_TRUE(proj.isFieldRetainedExactly("_id.a"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("_id.b"));
}

TEST(QueryProjectionTest, IdExclusionWithExclusionProjectionDoesNotPreserveId) {
    auto proj = createProjection("{}", "{_id: 0, a: 0}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("_id"));
    ASSERT_TRUE(proj.isFieldRetainedExactly("b"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, IdInclusionWithInclusionProjectionPreservesId) {
    auto proj = createProjection("{}", "{_id: 1, a: 1}");
    ASSERT_TRUE(proj.isFieldRetainedExactly("_id"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("b"));
    ASSERT_TRUE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, IdExclusionWithInclusionProjectionDoesNotPreserveId) {
    auto proj = createProjection("{}", "{_id: 0, a: 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("_id"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("b"));
    ASSERT_TRUE(proj.isFieldRetainedExactly("a"));
}

TEST(QueryProjectionTest, PositionalProjectionDoesNotPreserveFields) {
    auto proj = createFindProjection("{a: 1}", "{'a.$': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("b"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("a.b"));
    ASSERT_TRUE(proj.isFieldRetainedExactly("_id"));
}

TEST(QueryProjectionTest, PositionalProjectionWithIdExclusionDoesNotPreserveFields) {
    auto proj = createFindProjection("{a: 1}", "{_id: 0, 'a.$': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("b"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("a.b"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("_id"));
}

TEST(QueryProjectionTest, PositionalProjectionWithIdInclusionPreservesId) {
    auto proj = createFindProjection("{a: 1}", "{_id: 1, 'a.$': 1}");
    ASSERT_FALSE(proj.isFieldRetainedExactly("b"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("a"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("a.b"));
    ASSERT_TRUE(proj.isFieldRetainedExactly("_id"));
}

TEST(QueryProjectionTest, ProjectionOfFieldSimilarToIdIsNotSpecial) {
    auto proj = createProjection("{}", "{_idimpostor: 0}");
    ASSERT_TRUE(proj.isFieldRetainedExactly("_id"));
    ASSERT_FALSE(proj.isFieldRetainedExactly("_idimpostor"));
}

//
// DBRef projections.
//

TEST(QueryProjectionTest, DBRefProjections) {
    // non-dotted
    createProjection(BSONObj(), BSON("$ref" << 1));
    createProjection(BSONObj(), BSON("$id" << 1));
    createProjection(BSONObj(), BSON("$ref" << 1));
    // dotted before
    createFindProjection("{}", "{'a.$ref': 1}");
    createFindProjection("{}", "{'a.$id': 1}");
    createFindProjection("{}", "{'a.$db': 1}");
    // dotted after
    createProjection("{}", "{'$id.a': 1}");
    // position operator on $id
    // $ref and $db hold the collection and database names respectively,
    // so these fields cannot be arrays.
    createFindProjection("{'a.$id': {$elemMatch: {x: 1}}}", "{'a.$id.$': 1}");
}
}  // namespace
