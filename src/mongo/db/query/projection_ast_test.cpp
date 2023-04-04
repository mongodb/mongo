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

#include <map>
#include <string>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/projection_ast.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/db/query/serialization_options.h"
#include "mongo/unittest/inline_auto_update.h"

namespace {

using namespace mongo;

using mongo::projection_ast::Projection;
using mongo::projection_ast::ProjectType;

class ProjectionASTTest : public AggregationContextFixture {
public:
    Projection parseWithDefaultPolicies(const BSONObj& projectionBson,
                                        boost::optional<BSONObj> matchExprBson = boost::none) {
        return parseWithPolicies(projectionBson, matchExprBson, ProjectionPolicies{});
    }

    Projection parseWithFindFeaturesEnabled(const BSONObj& projectionBson,
                                            boost::optional<BSONObj> matchExprBson = boost::none) {
        auto policy = ProjectionPolicies::findProjectionPolicies();
        return parseWithPolicies(projectionBson, matchExprBson, policy);
    }

    Projection parseWithPolicies(const BSONObj& projectionBson,
                                 boost::optional<BSONObj> matchExprBson,
                                 ProjectionPolicies policies) {
        StatusWith<std::unique_ptr<MatchExpression>> swMatchExpression(nullptr);
        if (matchExprBson) {
            swMatchExpression = MatchExpressionParser::parse(*matchExprBson, getExpCtx());
            uassertStatusOK(swMatchExpression.getStatus());
        }

        return projection_ast::parseAndAnalyze(getExpCtx(),
                                               projectionBson,
                                               swMatchExpression.getValue().get(),
                                               matchExprBson.get_value_or(BSONObj()),
                                               policies);
    }
};

void assertCanClone(Projection proj) {
    boost::optional<Projection> optProj(std::move(proj));

    auto clone = optProj->root()->clone();

    auto originalBSON = projection_ast::astToDebugBSON(optProj->root());
    auto cloneBSON = projection_ast::astToDebugBSON(clone.get());
    ASSERT_BSONOBJ_EQ(originalBSON, cloneBSON);

    // Delete the original.
    optProj = boost::none;

    // Make sure we can still serialize the clone.
    auto cloneBSON2 = projection_ast::astToDebugBSON(clone.get());
    ASSERT_BSONOBJ_EQ(cloneBSON, cloneBSON2);
}

TEST_F(ProjectionASTTest, TestParsingTypeEmptyProjectionIsExclusionInFind) {
    Projection proj = parseWithFindFeaturesEnabled(fromjson("{}"));
    ASSERT(proj.type() == ProjectType::kExclusion);
}
TEST_F(ProjectionASTTest, TestParsingTypeEmptyProjectionFailsInAggregation) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{}")), AssertionException, 51272);
}

TEST_F(ProjectionASTTest, TestParsingTypeInclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: 1, b: 1}"));
    ASSERT(proj.type() == ProjectType::kInclusion);

    Projection projWithoutId = parseWithDefaultPolicies(fromjson("{_id: 0, b: 1}"));
    ASSERT(projWithoutId.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeInclusionIdOnly) {
    Projection proj = parseWithDefaultPolicies(fromjson("{_id: 1}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeInclusionWithNesting) {
    Projection proj = parseWithDefaultPolicies(fromjson("{'a.b': 1, 'a.c': 1}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeInclusionWithNestingObjectSyntax) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: {b: 1, c: 1}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeInclusionWithMixedSyntax) {
    {
        Projection proj = parseWithDefaultPolicies(fromjson("{'a.b': 1, a: {c: 1}}"));
        ASSERT(proj.type() == ProjectType::kInclusion);
    }

    {
        Projection proj = parseWithDefaultPolicies(fromjson("{a: {c: 1}, 'a.b': 1}"));
        ASSERT(proj.type() == ProjectType::kInclusion);
    }
}

TEST_F(ProjectionASTTest, TestParsingTypeExclusionWithMixedSyntax) {
    {
        Projection proj = parseWithDefaultPolicies(fromjson("{'a.b': 0, a: {c: 0}}"));
        ASSERT(proj.type() == ProjectType::kExclusion);
    }

    {
        Projection proj = parseWithDefaultPolicies(fromjson("{a: {c: 0}, 'a.b': 0}"));
        ASSERT(proj.type() == ProjectType::kExclusion);
    }
}

TEST_F(ProjectionASTTest, TestParsingTypeExclusion) {
    Projection p = parseWithDefaultPolicies(fromjson("{a: 0, _id: 0}"));
    ASSERT(p.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeExclusionWithNesting) {
    Projection p = parseWithDefaultPolicies(fromjson("{'a.b': 0, _id: 0}"));
    ASSERT(p.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeExclusionIdOnly) {
    Projection proj = parseWithDefaultPolicies(fromjson("{_id: 0}"));
    ASSERT(proj.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeExclusionWithIdIncluded) {
    {
        Projection p = parseWithDefaultPolicies(fromjson("{'a.b': 0, _id: 1}"));
        ASSERT(p.type() == ProjectType::kExclusion);
    }

    {
        Projection p = parseWithDefaultPolicies(fromjson("{_id: 1, 'a.b': 0}"));
        ASSERT(p.type() == ProjectType::kExclusion);
    }
}

TEST_F(ProjectionASTTest, MetaProjectionInAggDefaultsToInclusion) {
    const auto proj = parseWithDefaultPolicies(fromjson("{foo: {$meta: 'sortKey'}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, MetaProjectionInFindDefaultsToExclusion) {
    const auto proj = parseWithFindFeaturesEnabled(fromjson("{foo: {$meta: 'sortKey'}}"));
    ASSERT(proj.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, MetaProjectionWithIdInFindDefaultsToInclusion) {
    const auto proj = parseWithFindFeaturesEnabled(fromjson("{_id: 0, foo: {$meta: 'sortKey'}}"));
    ASSERT(proj.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, MetaProjectionWithIdInAggDefaultsToInclusion) {
    const auto proj = parseWithDefaultPolicies(fromjson("{_id: 0, foo: {$meta: 'sortKey'}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeExclusionWithNestingObjectSyntax) {
    Projection p = parseWithDefaultPolicies(fromjson("{a: {b: 0}, _id: 0}"));
    ASSERT(p.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeOnlyElemMatch) {
    Projection p = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {foo: 3}}}"));
    ASSERT(p.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeOnlySlice) {
    Projection p = parseWithFindFeaturesEnabled(fromjson("{a: {$slice: 1}}"));
    ASSERT(p.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeOnlyIdInclusionAndSlice) {
    Projection p = parseWithFindFeaturesEnabled(fromjson("{_id: 1, a: {$slice: 1}}"));
    ASSERT(p.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeOnlyIdExclusionAndSlice) {
    Projection p = parseWithFindFeaturesEnabled(fromjson("{_id: 0, a: {$slice: 1}}"));
    ASSERT(p.type() == ProjectType::kExclusion);
}

TEST_F(ProjectionASTTest, TestParsingTypeSliceHasHigherPriorityThanElemMatch) {
    {
        Projection p =
            parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {foo: 3}}, b: {$slice: 1}}"));
        ASSERT(p.type() == ProjectType::kExclusion);
    }

    // The order shouldn't matter.
    {
        Projection p =
            parseWithFindFeaturesEnabled(fromjson("{a: {$slice: 1}, b: {$elemMatch: {foo: 3}}}"));
        ASSERT(p.type() == ProjectType::kExclusion);
    }
}

// When only _id and $elemMatch are provided, the default is inclusion, regardless of the value for
// _id.
TEST_F(ProjectionASTTest, TestParsingTypeOnlyElemMatchAndIdExclusion) {
    {
        Projection p = parseWithFindFeaturesEnabled(fromjson("{_id: 0, x: {$elemMatch: {a: 1}}}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }

    {
        Projection p = parseWithFindFeaturesEnabled(fromjson("{x: {$elemMatch: {a: 1}}, _id: 0}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }
}

TEST_F(ProjectionASTTest, TestParsingTypeOnlyElemMatchAndIdInclusion) {
    {
        Projection p = parseWithFindFeaturesEnabled(fromjson("{_id: 1, x: {$elemMatch: {a: 1}}}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }
    {
        Projection p = parseWithFindFeaturesEnabled(fromjson("{x: {$elemMatch: {a: 1}}, _id: 1}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }
}

TEST_F(ProjectionASTTest, TestParsingElemMatchHasHigherPriorityThanMeta) {
    {
        Projection p = parseWithFindFeaturesEnabled(
            fromjson("{_id: 1, x: {$elemMatch: {a: 1}}, z: {$meta: 'recordId'}}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }

    {
        Projection p = parseWithFindFeaturesEnabled(
            fromjson("{_id: 0, x: {$elemMatch: {a: 1}}, z: {$meta: 'recordId'}}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }
}

TEST_F(ProjectionASTTest, TestParsingExclusionWithMeta) {
    {
        Projection p = parseWithDefaultPolicies(fromjson("{a: 0, b: {$meta: 'sortKey'}}"));
        ASSERT(p.type() == ProjectType::kExclusion);
    }

    {
        Projection p = parseWithDefaultPolicies(fromjson("{b: {$meta: 'sortKey'}, c: 0}"));
        ASSERT(p.type() == ProjectType::kExclusion);
    }
}

TEST_F(ProjectionASTTest, TestParsingInclusionWithMeta) {
    {
        Projection p = parseWithDefaultPolicies(fromjson("{a: 1, b: {$meta: 'sortKey'}}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }

    {
        Projection p = parseWithDefaultPolicies(fromjson("{b: {$meta: 'sortKey'}, c: 1}"));
        ASSERT(p.type() == ProjectType::kInclusion);
    }
}

TEST_F(ProjectionASTTest, TestInvalidProjectionWithInclusionsAndExclusions) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: 1, b: 0}")), DBException, 31254);
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: 0, b: 1}")), DBException, 31253);
}

TEST_F(ProjectionASTTest, TestInvalidProjectionWithExpressionInExclusion) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{a: 0, b: {$add: [1, 2]}}")), DBException, 31252);
}

TEST_F(ProjectionASTTest, TestInvalidProjectionWithLiteralInExclusion) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{a: 0, b: 'hallo world'}")), DBException, 31310);
}

TEST_F(ProjectionASTTest, TestInvalidProjectionWithPositionalAndElemMatch) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a.$': 1, b: {$elemMatch: {foo: 'bar'}}}"),
                                     fromjson("{a: 1}")),
        DBException,
        31255);

    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{b: {$elemMatch: {foo: 'bar'}}, 'a.$': 1}"),
                                     fromjson("{a: 1}")),
        DBException,
        31256);
}

TEST_F(ProjectionASTTest, TestCloningWithPositionalAndSlice) {
    Projection proj =
        parseWithFindFeaturesEnabled(fromjson("{'a.b': 1, b: 1, 'c.d.$': 1, f: {$slice: [1, 2]}}"),
                                     fromjson("{'c.d': {$gt: 1}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
    assertCanClone(std::move(proj));
}

TEST_F(ProjectionASTTest, TestCloningWithElemMatch) {
    Projection proj =
        parseWithFindFeaturesEnabled(fromjson("{'a.b': 1, b: 1, f: {$elemMatch: {foo: 'bar'}}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
    assertCanClone(std::move(proj));
}

TEST_F(ProjectionASTTest, TestCloningWithExpression) {
    Projection proj =
        parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, f: {$add: [1, 2, '$a']}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
    assertCanClone(std::move(proj));
}

TEST_F(ProjectionASTTest, TestDebugBSONWithPositionalAndSliceSkipLimit) {
    Projection proj = parseWithFindFeaturesEnabled(
        fromjson("{'a.b': 1, b: 1, 'c.d.$': 1, f: {$slice: [1, 2]}}"), fromjson("{'c.d': 1}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson(
        "{a: {b: true}, b: true, c: {'d.$': {'c.d': {$eq: 1}}}, f: {$slice: [1, 2]}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithNestedDollarPrefixedFields) {
    Projection proj = parseWithDefaultPolicies(fromjson("{c: {$id: 1}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{c: {$id: true}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithNestedPositional) {
    Projection proj =
        parseWithFindFeaturesEnabled(fromjson("{c: {'d.$': 1}}"), fromjson("{'c.d': 1}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{c: {'d.$': {'c.d': {$eq: 1}}}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithSliceLimit) {
    Projection proj = parseWithFindFeaturesEnabled(fromjson("{'a.b': 1, b: 1, f: {$slice: 2}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {b: true}, b: true, f: {$slice: 2}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithElemMatch) {
    Projection proj =
        parseWithFindFeaturesEnabled(fromjson("{'a.b': 1, b: 1, f: {$elemMatch: {foo: 'bar'}}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected =
        fromjson("{a: {b: true}, b: true, f: {$elemMatch: {foo: {$eq: 'bar'}}}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithExpression) {
    Projection proj =
        parseWithDefaultPolicies(fromjson("{'a.b': 1, b: 1, f: {$add: [1, 2, '$a']}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected =
        fromjson("{a: {b: true}, b: true, f: {$add: [{$const: 1}, {$const: 2}, '$a']}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithSimpleInclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: 1, b: 1}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: true, b: true, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithSimpleExclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: 0, b: 0}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: false, b: false}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithObjectSyntaxInclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: {b: {d: 1}, c: 1}, f: 1}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {b: {d: true}, c: true}, f: true, _id : true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithObjectSyntaxExclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: {b: {d: 0}, c: 0}, f: 0}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {b: {d: false}, c: false}, f: false}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithMixedSyntaxInclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: {'b.d': 1, c: 1}, f: 1}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {b: {d: true}, c: true}, f: true, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithMixedSyntaxExclusion) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: {'b.d': 0, c: 0}, f: 0}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {b: {d: false}, c: false}, f: false}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithOnlyElemMatch) {
    Projection proj = parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: {foo: 3}}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {$elemMatch: {foo: {$eq: 3}}}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithOnlySlice) {
    Projection proj = parseWithFindFeaturesEnabled(fromjson("{a: {$slice: 1}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {$slice: 1}}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithLiteralValue) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: 'abc'}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {$const: 'abc'}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, TestDebugBSONWithNestedLiteralValue) {
    Projection proj = parseWithDefaultPolicies(fromjson("{a: {b: 'abc'}}"));

    BSONObj output = projection_ast::astToDebugBSON(proj.root());
    BSONObj expected = fromjson("{a: {b: {$const: 'abc'}}, _id: true}");
    ASSERT_BSONOBJ_EQ(output, expected);
}

TEST_F(ProjectionASTTest, ParserErrorsOnCollisionNestedFieldFirst) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{'a.b': 1, d: 1, a: 1}")), DBException, 31250);
}

TEST_F(ProjectionASTTest, ParserErrorsOnCollisionNestedFieldLast) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{a: 1, d: 1, 'a.b': 1}")), DBException, 31249);
}

TEST_F(ProjectionASTTest, ParserErrorsOnCollisionIdenticalField) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(BSON("a" << 1 << "a" << 1)), DBException, 31250);
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(BSON("a" << BSON("b" << 1) << "a.b" << 1)), DBException, 31250);
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(BSON("a.b" << 1 << "a" << BSON("b" << 1))), DBException, 31250);
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(BSON("a" << 1 << "a" << BSON("b" << 1))), DBException, 31250);
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(BSON("a.b" << 1 << "a" << BSON("b" << BSON("c" << 1)))),
        DBException,
        31250);
}

TEST_F(ProjectionASTTest, ParserErrorsOnSliceWithWrongNumberOfArguments) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a': {$slice: []}}")), DBException, 28667);
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a': {$slice: [1]}}")), DBException, 28667);
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a': {$slice: [1, 2, 3, 4]}}")),
                       DBException,
                       28667);
}

TEST_F(ProjectionASTTest, ParserErrorsOnSliceWithWrongArgumentType) {
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a': {$slice: 'hello world'}}")),
                       DBException,
                       28667);
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a': {$slice: {foo: 1}}}")), DBException, 28667);
}

TEST_F(ProjectionASTTest, ParserErrorsOnFindSliceWithSpecialValuesAsArguments) {
    const StringData fieldName = "a";

    auto checkSliceArguments = [&](const BSONObj& projection,
                                   int expectedLimit,
                                   boost::optional<int> expectedSkip) {
        auto parsedProjection = parseWithFindFeaturesEnabled(projection);
        auto astNode = parsedProjection.root()->getChild(fieldName);
        ASSERT(astNode);
        auto sliceAstNode = dynamic_cast<const projection_ast::ProjectionSliceASTNode*>(astNode);
        ASSERT(sliceAstNode);
        ASSERT_EQ(sliceAstNode->limit(), expectedLimit);
        ASSERT_EQ(sliceAstNode->skip(), expectedSkip);
    };

    const auto positiveClamping =
        BSON_ARRAY((static_cast<long long>(std::numeric_limits<int>::max()) + 1)
                   << std::numeric_limits<long long>::max() << std::numeric_limits<double>::max()
                   << std::numeric_limits<double>::infinity() << Decimal128::kPositiveInfinity
                   << Decimal128::kLargestPositive);
    for (const auto& element : positiveClamping) {
        constexpr auto expectedValue = std::numeric_limits<int>::max();
        checkSliceArguments(
            BSON(fieldName << BSON("$slice" << element)), expectedValue, boost::none);
        checkSliceArguments(
            BSON(fieldName << BSON("$slice" << BSON_ARRAY(1 << element))), expectedValue, 1);
        checkSliceArguments(
            BSON(fieldName << BSON("$slice" << BSON_ARRAY(element << 1))), 1, expectedValue);
        checkSliceArguments(BSON(fieldName << BSON("$slice" << BSON_ARRAY(element << element))),
                            expectedValue,
                            expectedValue);
    }

    const auto negativeClamping =
        BSON_ARRAY((static_cast<long long>(std::numeric_limits<int>::min()) - 1)
                   << std::numeric_limits<long long>::min() << std::numeric_limits<double>::lowest()
                   << -std::numeric_limits<double>::infinity() << Decimal128::kNegativeInfinity
                   << Decimal128::kLargestNegative);
    for (const auto& element : negativeClamping) {
        const auto expectedValue = std::numeric_limits<int>::min();
        checkSliceArguments(
            BSON(fieldName << BSON("$slice" << element)), expectedValue, boost::none);
        checkSliceArguments(
            BSON(fieldName << BSON("$slice" << BSON_ARRAY(element << 1))), 1, expectedValue);
    }

    const auto convertionToZero =
        BSON_ARRAY(0ll << 0.0 << 0.3 << -0.3 << std::numeric_limits<double>::quiet_NaN()
                       << Decimal128::kNegativeNaN << Decimal128::kPositiveNaN
                       << Decimal128::kSmallestPositive << Decimal128::kSmallestNegative);
    for (const auto& element : convertionToZero) {
        constexpr auto expectedValue = 0;
        checkSliceArguments(
            BSON(fieldName << BSON("$slice" << element)), expectedValue, boost::none);
        checkSliceArguments(
            BSON(fieldName << BSON("$slice" << BSON_ARRAY(element << 1))), 1, expectedValue);
    }
}

TEST_F(ProjectionASTTest, ParserErrorsOnInvalidElemMatchArgument) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: []}}")), DBException, 31274);

    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{a: {$elemMatch: 'string'}}")), DBException, 31274);
}

TEST_F(ProjectionASTTest, ParserErrorsOnElemMatchOnDottedField) {
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.b': {$elemMatch: {b: 1}}}")),
                       DBException,
                       31275);

    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{a: {x: {$elemMatch: {b: 1}}}}")),
                       DBException,
                       31275);
}

TEST_F(ProjectionASTTest, ParserErrorsOnMultiplePositionalInOnePath) {
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.$.b.$': 1}"), fromjson("{a: 1}")),
                       DBException,
                       31394);
}

TEST_F(ProjectionASTTest, ParserErrorsOnMultiplePositionalInProjection) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a.$': 1, 'b.$': 1}"), fromjson("{a: 1, b: 1}")),
        DBException,
        31276);

    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.b.$.': 1}"), fromjson("{a: 1}")),
                       DBException,
                       31394);

    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.$.b.$': 1}"), fromjson("{a: 1}")),
                       DBException,
                       31394);

    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.$.$': 1}"), fromjson("{a: 1}")),
                       DBException,
                       31394);
}

TEST_F(ProjectionASTTest, ParserErrorsOnSubfieldPrefixedByDbRefField) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{'a.$idFOOBAR': 1}")), DBException, 16410);
}

TEST_F(ProjectionASTTest, ParserErrorsOnJustPositionalProjection) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'$': 1}"), fromjson("{a: 1}")), DBException, 16410);

    // {$: 1} is an invalid match expression.
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{$: 1}"), fromjson("{$: 1}")),
                       DBException,
                       ErrorCodes::BadValue);

    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'$': 1}"), fromjson("{}")), DBException, 16410);
}

TEST_F(ProjectionASTTest, ParserErrorsOnPositionalAndSlice) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a.$': {$slice: 1}}")), DBException, 31271);
}

TEST_F(ProjectionASTTest, ParserErrorsOnPositionalOnElemMatch) {
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.$': {$elemMatch: {b: 1}}}")),
                       DBException,
                       31271);
}

TEST_F(ProjectionASTTest, ParserErrorsOnPositionalOnLiteral) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a.$': 'literal string'}")), DBException, 31308);
}

TEST_F(ProjectionASTTest, ParserErrorsOnPositionalAndSubObj) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'a': {'b.$': {c: 1}}}")), DBException, 31271);
}

TEST_F(ProjectionASTTest, ParserErrorsOnInvalidPositionalSyntax) {
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'.$': 1}")), DBException, 51050);
}

TEST_F(ProjectionASTTest, ParserDoesNotErrorOnPositionalOfDbRefField) {
    Projection idProj = parseWithFindFeaturesEnabled(fromjson("{'a.$id.b.$': 1, x: 1}"),
                                                     fromjson("{'a.$id.b': 1}"));
    ASSERT(idProj.type() == ProjectType::kInclusion);

    Projection dbProj = parseWithFindFeaturesEnabled(fromjson("{'a.$db.b.$': 1, x: 1}"),
                                                     fromjson("{'a.$db.b': 1}"));
    ASSERT(dbProj.type() == ProjectType::kInclusion);

    Projection refProj = parseWithFindFeaturesEnabled(fromjson("{'a.$ref.b.$': 1, x: 1}"),
                                                      fromjson("{'a.$ref.b': 1}"));
    ASSERT(refProj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, ParserDoesNotErrorOnPositionalOfNonQueryField) {
    {
        Projection proj =
            parseWithFindFeaturesEnabled(fromjson("{'a.$': 1, x: 1}"), fromjson("{'a.b': 1}"));
        ASSERT(proj.type() == ProjectType::kInclusion);
    }

    {
        Projection proj = parseWithFindFeaturesEnabled(fromjson("{'a.b.$': 1, x: 1}"),
                                                       fromjson("{'a.b.$db': 1}"));
        ASSERT(proj.type() == ProjectType::kInclusion);
    }

    {
        Projection proj =
            parseWithFindFeaturesEnabled(fromjson("{'b.$': 1, x: 1}"), fromjson("{'a.b.c': 1}"));
        ASSERT(proj.type() == ProjectType::kInclusion);
    }

    {
        Projection proj =
            parseWithFindFeaturesEnabled(fromjson("{'b.c.$': 1, x: 1}"), fromjson("{'a.b.c': 1}"));
        ASSERT(proj.type() == ProjectType::kInclusion);
    }
}

TEST_F(ProjectionASTTest, ShouldThrowWhenParsingInvalidExpression) {
    ASSERT_THROWS(parseWithDefaultPolicies(BSON("a" << BSON("$gt" << BSON("bad"
                                                                          << "arguments")))),
                  AssertionException);
}

TEST_F(ProjectionASTTest, ShouldThrowWhenParsingUnknownExpression) {
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(BSON("a" << BSON("$fakeExpression" << BSON("bad"
                                                                            << "arguments")))),
        AssertionException,
        31325);
}

TEST_F(ProjectionASTTest, ShouldThrowWhenParsingSliceInvalidWithFindFeaturesOff) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: {$slice: {wrongType: 1}}}")),
                       AssertionException,
                       28667);
}

TEST_F(ProjectionASTTest, ShouldSucceedWhenParsingAggSliceWithFindFeaturesOff) {
    auto proj = parseWithDefaultPolicies(fromjson("{a: {$slice: ['$a', 3]}}"));
    ASSERT(proj.type() == ProjectType::kInclusion);
}

TEST_F(ProjectionASTTest, ShouldThrowWhenParsingElemMatchWithFindFeaturesOff) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{a: {$elemMatch: {foo: 3}}}")),
                       AssertionException,
                       ErrorCodes::InvalidPipelineOperator);
}

TEST_F(ProjectionASTTest, ShouldThrowWhenParsingPositionalWithFindFeaturesOff) {
    ASSERT_THROWS_CODE(parseWithDefaultPolicies(fromjson("{'a.$': 1}"), fromjson("{a: {$gt: 1}}")),
                       AssertionException,
                       31324);
    ASSERT_THROWS_CODE(
        parseWithDefaultPolicies(fromjson("{a: {'b.$': 1}}"), fromjson("{'a.b': {$gt: 1}}")),
        AssertionException,
        31324);
}

TEST_F(ProjectionASTTest, ShouldThrowWithPositionalInTheMiddleOfFieldPaths) {
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.b': 1, b: 1, 'c.d.$.e': 1}"),
                                                    fromjson("{'c.d': 1}")),
                       DBException,
                       31394);
    ASSERT_THROWS_CODE(parseWithFindFeaturesEnabled(fromjson("{'a.b': 1, b: 1, 'c.$id.$.e': 1}"),
                                                    fromjson("{'c.$id': 1}")),
                       DBException,
                       31394);
}

TEST_F(ProjectionASTTest, ShouldThrowWithPositionalOnExclusion) {
    ASSERT_THROWS_CODE(
        parseWithFindFeaturesEnabled(fromjson("{'c.d.$': 0}"), fromjson("{'c.d': 1}")),
        DBException,
        31395);
}
std::string redactFieldNameForTest(StringData s) {
    return str::stream() << "HASH<" << s << ">";
}

TEST_F(ProjectionASTTest, TestASTRedaction) {
    SerializationOptions options;
    options.replacementForLiteralArgs = "?";
    options.redactIdentifiers = true;
    options.identifierRedactionPolicy = redactFieldNameForTest;


    auto proj = fromjson("{'a.b': 1}");
    BSONObj output = projection_ast::serialize(parseWithFindFeaturesEnabled(proj), options);
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"HASH<a>":{"HASH<b>":true},"HASH<_id>":true})",
        output);

    proj = fromjson("{'a.b': 0}");
    output = projection_ast::serialize(parseWithFindFeaturesEnabled(proj), options);
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"HASH<a>":{"HASH<b>":false}})",
        output);

    proj = fromjson("{a: 1, b: 1}");
    output = projection_ast::serialize(parseWithFindFeaturesEnabled(proj), options);
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"HASH<a>":true,"HASH<b>":true,"HASH<_id>":true})",
        output);

    // ElemMatch projection
    proj = fromjson("{f: {$elemMatch: {foo: 'bar'}}}");
    output = projection_ast::serialize(parseWithFindFeaturesEnabled(proj), options);
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"HASH<f>":{"$elemMatch":{"HASH<foo>":{"$eq":"?"}}},"HASH<_id>":true})",
        output);

    // Positional projection
    proj = fromjson("{'x.$': 1}");
    output =
        projection_ast::serialize(parseWithFindFeaturesEnabled(proj, fromjson("{'x.a': 2}")), {});
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"x.$":true,"_id":true})",
        output);

    // Slice (first form)
    proj = fromjson("{a: {$slice: 1}}");
    output = projection_ast::serialize(parseWithFindFeaturesEnabled(proj), options);
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"HASH<a>":{"$slice":"?"}})",
        output);

    // Slice (second form)
    proj = fromjson("{a: {$slice: [1, 3]}}");
    output = projection_ast::serialize(parseWithFindFeaturesEnabled(proj), options);
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"HASH<a>":{"$slice":["?","?"]}})",
        output);

    /// $meta projection
    proj = fromjson("{foo: {$meta: 'indexKey'}}");
    output = projection_ast::serialize(parseWithFindFeaturesEnabled(proj), options);
    ASSERT_BSONOBJ_EQ_AUTO(  //
        R"({"HASH<foo>":{"$meta":"indexKey"}})",
        output);
}
}  // namespace
