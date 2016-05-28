/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/query/parsed_projection.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/unittest/unittest.h"
#include <memory>

namespace {

using std::unique_ptr;
using std::string;
using std::vector;

using namespace mongo;

//
// creation function
//

unique_ptr<ParsedProjection> createParsedProjection(const BSONObj& query, const BSONObj& projObj) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression statusWithMatcher =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT(statusWithMatcher.isOK());
    std::unique_ptr<MatchExpression> queryMatchExpr = std::move(statusWithMatcher.getValue());
    ParsedProjection* out = NULL;
    Status status = ParsedProjection::make(
        projObj, queryMatchExpr.get(), &out, ExtensionsCallbackDisallowExtensions());
    if (!status.isOK()) {
        FAIL(mongoutils::str::stream() << "failed to parse projection " << projObj << " (query: "
                                       << query
                                       << "): "
                                       << status.toString());
    }
    ASSERT(out);
    return unique_ptr<ParsedProjection>(out);
}

unique_ptr<ParsedProjection> createParsedProjection(const char* queryStr, const char* projStr) {
    BSONObj query = fromjson(queryStr);
    BSONObj projObj = fromjson(projStr);
    return createParsedProjection(query, projObj);
}

//
// Failure to create a parsed projection is expected
//

void assertInvalidProjection(const char* queryStr, const char* projStr) {
    BSONObj query = fromjson(queryStr);
    BSONObj projObj = fromjson(projStr);
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression statusWithMatcher =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT(statusWithMatcher.isOK());
    std::unique_ptr<MatchExpression> queryMatchExpr = std::move(statusWithMatcher.getValue());
    ParsedProjection* out = NULL;
    Status status = ParsedProjection::make(
        projObj, queryMatchExpr.get(), &out, ExtensionsCallbackDisallowExtensions());
    std::unique_ptr<ParsedProjection> destroy(out);
    ASSERT(!status.isOK());
}

// canonical_query.cpp will invoke ParsedProjection::make only when
// the projection spec is non-empty. This test case is included for
// completeness and do not reflect actual usage.
TEST(ParsedProjectionTest, MakeId) {
    unique_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{}"));
    ASSERT(parsedProj->requiresDocument());
}

TEST(ParsedProjectionTest, MakeEmpty) {
    unique_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: 0}"));
    ASSERT(parsedProj->requiresDocument());
}

TEST(ParsedProjectionTest, MakeSingleField) {
    unique_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{a: 1}"));
    ASSERT(!parsedProj->requiresDocument());
    const vector<StringData>& fields = parsedProj->getRequiredFields();
    ASSERT_EQUALS(fields.size(), 2U);
    ASSERT_EQUALS(fields[0], "_id");
    ASSERT_EQUALS(fields[1], "a");
}

TEST(ParsedProjectionTest, MakeSingleFieldCovered) {
    unique_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: 0, a: 1}"));
    ASSERT(!parsedProj->requiresDocument());
    const vector<StringData>& fields = parsedProj->getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "a");
}

TEST(ParsedProjectionTest, MakeSingleFieldIDCovered) {
    unique_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: 1}"));
    ASSERT(!parsedProj->requiresDocument());
    const vector<StringData>& fields = parsedProj->getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "_id");
}

// boolean support is undocumented
TEST(ParsedProjectionTest, MakeSingleFieldCoveredBoolean) {
    unique_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: 0, a: true}"));
    ASSERT(!parsedProj->requiresDocument());
    const vector<StringData>& fields = parsedProj->getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "a");
}

// boolean support is undocumented
TEST(ParsedProjectionTest, MakeSingleFieldCoveredIdBoolean) {
    unique_ptr<ParsedProjection> parsedProj(createParsedProjection("{}", "{_id: false, a: 1}"));
    ASSERT(!parsedProj->requiresDocument());
    const vector<StringData>& fields = parsedProj->getRequiredFields();
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_EQUALS(fields[0], "a");
}

//
// Positional operator validation
//

TEST(ParsedProjectionTest, InvalidPositionalOperatorProjections) {
    assertInvalidProjection("{}", "{'a.$': 1}");
    assertInvalidProjection("{a: 1}", "{'b.$': 1}");
    assertInvalidProjection("{a: 1}", "{'a.$': 0}");
    assertInvalidProjection("{a: 1}", "{'a.$.d.$': 1}");
    assertInvalidProjection("{a: 1}", "{'a.$.$': 1}");
    assertInvalidProjection("{a: 1}", "{'a.$.$': 1}");
    assertInvalidProjection("{a: 1, b: 1, c: 1}", "{'abc.$': 1}");
    assertInvalidProjection("{$or: [{a: 1}, {$or: [{b: 1}, {c: 1}]}]}", "{'d.$': 1}");
    assertInvalidProjection("{a: [1, 2, 3]}", "{'.$': 1}");
}

TEST(ParsedProjectionTest, ValidPositionalOperatorProjections) {
    createParsedProjection("{a: 1}", "{'a.$': 1}");
    createParsedProjection("{a: 1}", "{'a.foo.bar.$': 1}");
    createParsedProjection("{a: 1}", "{'a.foo.bar.$.x.y': 1}");
    createParsedProjection("{'a.b.c': 1}", "{'a.b.c.$': 1}");
    createParsedProjection("{'a.b.c': 1}", "{'a.e.f.$': 1}");
    createParsedProjection("{a: {b: 1}}", "{'a.$': 1}");
    createParsedProjection("{a: 1, b: 1}}", "{'a.$': 1}");
    createParsedProjection("{a: 1, b: 1}}", "{'b.$': 1}");
    createParsedProjection("{$and: [{a: 1}, {b: 1}]}", "{'a.$': 1}");
    createParsedProjection("{$and: [{a: 1}, {b: 1}]}", "{'b.$': 1}");
    createParsedProjection("{$or: [{a: 1}, {b: 1}]}", "{'a.$': 1}");
    createParsedProjection("{$or: [{a: 1}, {b: 1}]}", "{'b.$': 1}");
    createParsedProjection("{$and: [{$or: [{a: 1}, {$and: [{b: 1}, {c: 1}]}]}]}", "{'c.d.f.$': 1}");
    // Fields with empty name can be projected using the positional $ operator.
    createParsedProjection("{'': [1, 2, 3]}", "{'.$': 1}");
}

// Some match expressions (eg. $where) do not override MatchExpression::path()
// In this test case, we use an internal match expression implementation ALWAYS_FALSE
// to achieve the same effect.
// Projection parser should handle this the same way as an empty path.
TEST(ParsedProjectionTest, InvalidPositionalProjectionDefaultPathMatchExpression) {
    unique_ptr<MatchExpression> queryMatchExpr(new FalseMatchExpression(""));
    ASSERT(NULL == queryMatchExpr->path().rawData());

    ParsedProjection* out = NULL;
    BSONObj projObj = fromjson("{'a.$': 1}");
    Status status = ParsedProjection::make(
        projObj, queryMatchExpr.get(), &out, ExtensionsCallbackDisallowExtensions());
    ASSERT(!status.isOK());
    std::unique_ptr<ParsedProjection> destroy(out);

    // Projecting onto empty field should fail.
    BSONObj emptyFieldProjObj = fromjson("{'.$': 1}");
    status = ParsedProjection::make(
        emptyFieldProjObj, queryMatchExpr.get(), &out, ExtensionsCallbackDisallowExtensions());
    ASSERT(!status.isOK());
}

TEST(ParsedProjectionTest, ParsedProjectionDefaults) {
    auto parsedProjection = createParsedProjection("{}", "{}");

    ASSERT_FALSE(parsedProjection->wantSortKey());
    ASSERT_TRUE(parsedProjection->requiresDocument());
    ASSERT_FALSE(parsedProjection->requiresMatchDetails());
    ASSERT_FALSE(parsedProjection->wantGeoNearDistance());
    ASSERT_FALSE(parsedProjection->wantGeoNearPoint());
    ASSERT_FALSE(parsedProjection->wantIndexKey());
}

TEST(ParsedProjectionTest, SortKeyMetaProjection) {
    auto parsedProjection = createParsedProjection("{}", "{foo: {$meta: 'sortKey'}}");

    ASSERT_EQ(parsedProjection->getProjObj(), fromjson("{foo: {$meta: 'sortKey'}}"));
    ASSERT_TRUE(parsedProjection->wantSortKey());
    ASSERT_TRUE(parsedProjection->requiresDocument());

    ASSERT_FALSE(parsedProjection->requiresMatchDetails());
    ASSERT_FALSE(parsedProjection->wantGeoNearDistance());
    ASSERT_FALSE(parsedProjection->wantGeoNearPoint());
    ASSERT_FALSE(parsedProjection->wantIndexKey());
}

TEST(ParsedProjectionTest, SortKeyMetaProjectionCovered) {
    auto parsedProjection = createParsedProjection("{}", "{a: 1, foo: {$meta: 'sortKey'}, _id: 0}");

    ASSERT_EQ(parsedProjection->getProjObj(), fromjson("{a: 1, foo: {$meta: 'sortKey'}, _id: 0}"));
    ASSERT_TRUE(parsedProjection->wantSortKey());

    ASSERT_FALSE(parsedProjection->requiresDocument());
    ASSERT_FALSE(parsedProjection->requiresMatchDetails());
    ASSERT_FALSE(parsedProjection->wantGeoNearDistance());
    ASSERT_FALSE(parsedProjection->wantGeoNearPoint());
    ASSERT_FALSE(parsedProjection->wantIndexKey());
}

TEST(ParsedProjectionTest, SortKeyMetaAndSlice) {
    auto parsedProjection =
        createParsedProjection("{}", "{a: 1, foo: {$meta: 'sortKey'}, _id: 0, b: {$slice: 1}}");

    ASSERT_EQ(parsedProjection->getProjObj(),
              fromjson("{a: 1, foo: {$meta: 'sortKey'}, _id: 0, b: {$slice: 1}}"));
    ASSERT_TRUE(parsedProjection->wantSortKey());
    ASSERT_TRUE(parsedProjection->requiresDocument());

    ASSERT_FALSE(parsedProjection->requiresMatchDetails());
    ASSERT_FALSE(parsedProjection->wantGeoNearDistance());
    ASSERT_FALSE(parsedProjection->wantGeoNearPoint());
    ASSERT_FALSE(parsedProjection->wantIndexKey());
}

TEST(ParsedProjectionTest, SortKeyMetaAndElemMatch) {
    auto parsedProjection = createParsedProjection(
        "{}", "{a: 1, foo: {$meta: 'sortKey'}, _id: 0, b: {$elemMatch: {a: 1}}}");

    ASSERT_EQ(parsedProjection->getProjObj(),
              fromjson("{a: 1, foo: {$meta: 'sortKey'}, _id: 0, b: {$elemMatch: {a: 1}}}"));
    ASSERT_TRUE(parsedProjection->wantSortKey());
    ASSERT_TRUE(parsedProjection->requiresDocument());

    ASSERT_FALSE(parsedProjection->requiresMatchDetails());
    ASSERT_FALSE(parsedProjection->wantGeoNearDistance());
    ASSERT_FALSE(parsedProjection->wantGeoNearPoint());
    ASSERT_FALSE(parsedProjection->wantIndexKey());
}

TEST(ParsedProjectionTest, SortKeyMetaAndExclusion) {
    auto parsedProjection = createParsedProjection("{}", "{a: 0, foo: {$meta: 'sortKey'}, _id: 0}");

    ASSERT_EQ(parsedProjection->getProjObj(), fromjson("{a: 0, foo: {$meta: 'sortKey'}, _id: 0}"));
    ASSERT_TRUE(parsedProjection->wantSortKey());
    ASSERT_TRUE(parsedProjection->requiresDocument());

    ASSERT_FALSE(parsedProjection->requiresMatchDetails());
    ASSERT_FALSE(parsedProjection->wantGeoNearDistance());
    ASSERT_FALSE(parsedProjection->wantGeoNearPoint());
    ASSERT_FALSE(parsedProjection->wantIndexKey());
}

//
// Cases for ParsedProjection::isFieldRetainedExactly().
//

TEST(ParsedProjectionTest, InclusionProjectionPreservesChild) {
    auto parsedProjection = createParsedProjection("{}", "{a: 1}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a.b"));
}

TEST(ParsedProjectionTest, InclusionProjectionDoesNotPreserveParent) {
    auto parsedProjection = createParsedProjection("{}", "{'a.b': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, InclusionProjectionPreservesField) {
    auto parsedProjection = createParsedProjection("{}", "{a: 1}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, InclusionProjectionOrderingDeterminesPreservation) {
    auto parsedProjection = createParsedProjection("{}", "{a: 1, 'a.b': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a.b"));

    parsedProjection = createParsedProjection("{}", "{'a.b': 1, a: 1}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a.b"));
}

TEST(ParsedProjectionTest, ExclusionProjectionDoesNotPreserveParent) {
    auto parsedProjection = createParsedProjection("{}", "{'a.b': 0}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, ExclusionProjectionDoesNotPreserveChild) {
    auto parsedProjection = createParsedProjection("{}", "{a: 0}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a.b"));
}

TEST(ParsedProjectionTest, ExclusionProjectionDoesNotPreserveField) {
    auto parsedProjection = createParsedProjection("{}", "{a: 0}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, InclusionProjectionDoesNotPreserveNonIncludedFields) {
    auto parsedProjection = createParsedProjection("{}", "{a: 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("c"));
}

TEST(ParsedProjectionTest, ExclusionProjectionPreservesNonExcludedFields) {
    auto parsedProjection = createParsedProjection("{}", "{a: 0}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("c"));
}

TEST(ParsedProjectionTest, PositionalProjectionDoesNotPreserveField) {
    auto parsedProjection = createParsedProjection("{a: {$elemMatch: {$eq: 0}}}", "{'a.$': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, PositionalProjectionDoesNotPreserveChild) {
    auto parsedProjection = createParsedProjection("{a: {$elemMatch: {$eq: 0}}}", "{'a.$': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a.b"));
}

TEST(ParsedProjectionTest, PositionalProjectionDoesNotPreserveParent) {
    auto parsedProjection =
        createParsedProjection("{'a.b': {$elemMatch: {$eq: 0}}}", "{'a.b.$': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, MetaProjectionDoesNotPreserveField) {
    auto parsedProjection = createParsedProjection("{}", "{a: {$meta: 'textScore'}}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, MetaProjectionDoesNotPreserveChild) {
    auto parsedProjection = createParsedProjection("{}", "{a: {$meta: 'textScore'}}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a.b"));
}

TEST(ParsedProjectionTest, IdExclusionProjectionPreservesOtherFields) {
    auto parsedProjection = createParsedProjection("{}", "{_id: 0}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, IdInclusionProjectionDoesNotPreserveOtherFields) {
    auto parsedProjection = createParsedProjection("{}", "{_id: 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, IdSubfieldExclusionProjectionPreservesId) {
    auto parsedProjection = createParsedProjection("{}", "{'_id.a': 0}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id.a"));
}

TEST(ParsedProjectionTest, IdSubfieldInclusionProjectionPreservesId) {
    auto parsedProjection = createParsedProjection("{}", "{'_id.a': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id.a"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id.b"));
}

TEST(ParsedProjectionTest, IdExclusionWithExclusionProjectionDoesNotPreserveId) {
    auto parsedProjection = createParsedProjection("{}", "{_id: 0, a: 0}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("_id"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, IdInclusionWithInclusionProjectionPreservesId) {
    auto parsedProjection = createParsedProjection("{}", "{_id: 1, a: 1}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, IdExclusionWithInclusionProjectionDoesNotPreserveId) {
    auto parsedProjection = createParsedProjection("{}", "{_id: 0, a: 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("_id"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("a"));
}

TEST(ParsedProjectionTest, PositionalProjectionDoesNotPreserveFields) {
    auto parsedProjection = createParsedProjection("{a: 1}", "{'a.$': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a.b"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id"));
}

TEST(ParsedProjectionTest, PositionalProjectionWithIdExclusionDoesNotPreserveFields) {
    auto parsedProjection = createParsedProjection("{a: 1}", "{_id: 0, 'a.$': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a.b"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("_id"));
}

TEST(ParsedProjectionTest, PositionalProjectionWithIdInclusionPreservesId) {
    auto parsedProjection = createParsedProjection("{a: 1}", "{_id: 1, 'a.$': 1}");
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("b"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("a.b"));
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id"));
}

TEST(ParsedProjectionTest, ProjectionOfFieldSimilarToIdIsNotSpecial) {
    auto parsedProjection = createParsedProjection("{}", "{_idimpostor: 0}");
    ASSERT_TRUE(parsedProjection->isFieldRetainedExactly("_id"));
    ASSERT_FALSE(parsedProjection->isFieldRetainedExactly("_idimpostor"));
}

//
// DBRef projections
//

TEST(ParsedProjectionTest, DBRefProjections) {
    // non-dotted
    createParsedProjection(BSONObj(), BSON("$ref" << 1));
    createParsedProjection(BSONObj(), BSON("$id" << 1));
    createParsedProjection(BSONObj(), BSON("$ref" << 1));
    // dotted before
    createParsedProjection("{}", "{'a.$ref': 1}");
    createParsedProjection("{}", "{'a.$id': 1}");
    createParsedProjection("{}", "{'a.$db': 1}");
    // dotted after
    createParsedProjection("{}", "{'$id.a': 1}");
    // position operator on $id
    // $ref and $db hold the collection and database names respectively,
    // so these fields cannot be arrays.
    createParsedProjection("{'a.$id': {$elemMatch: {x: 1}}}", "{'a.$id.$': 1}");
}
}  // unnamed namespace
