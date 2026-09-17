// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
auto parseMatchExpr(const std::string& query) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BSONObj queryBson = fromjson(query);
    auto result = MatchExpressionParser::parse(queryBson, expCtx);
    ASSERT_TRUE(result.isOK());
    return std::move(result.getValue());
}
}  // namespace

TEST(ElemMatchDependenciesTest, Simple) {
    DepsTracker deps;
    auto me = parseMatchExpr("{xyz: {$elemMatch: {$eq: 42}}}");
    dependency_analysis::addDependencies(me.get(), &deps);
    ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
}

TEST(ElemMatchDependenciesTest, SimpleNested) {
    DepsTracker deps;
    auto me = parseMatchExpr("{xyz: {$elemMatch: {$elemMatch: {$eq: 42}}}}");
    dependency_analysis::addDependencies(me.get(), &deps);
    ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
}

TEST(ElemMatchDependenciesTest, SimpleNestedObjElemMatch) {
    {
        DepsTracker deps;
        auto me = parseMatchExpr("{xyz: {$elemMatch: {$elemMatch: {a: 42}}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
    }
    {
        DepsTracker deps;
        auto me = parseMatchExpr("{xyz: {$elemMatch: {b: {$elemMatch: {a: 42}}}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
    }
}

TEST(ElemMatchDependenciesTest, SimpleNestedInternalSchemaObjMatchElemMatch) {
    DepsTracker deps;
    auto me = parseMatchExpr("{xyz: {$_internalSchemaObjectMatch: {a: {$elemMatch: {c: 42}}}}}");
    dependency_analysis::addDependencies(me.get(), &deps);
    ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
}

TEST(ElemMatchDependenciesTest, MixedNestedInternalSchemaObjMatchElemMatch) {
    DepsTracker deps;
    auto me = parseMatchExpr(
        "{xyz: {$_internalSchemaObjectMatch: {a: {$elemMatch: {c: 42}}, b: {$eq: 3}}}}");
    dependency_analysis::addDependencies(me.get(), &deps);
    ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
}

TEST(ElemMatchDependenciesTest, TwoPredicatesUnderElemMatch) {
    {
        // Ensure we correctly avoid adding "" to the dependencies.
        DepsTracker deps;
        auto me = parseMatchExpr("{abc: 1, xyz: {$elemMatch: {$elemMatch: {$eq: 42}, $eq: 42}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz", "abc"}));
    }
    {
        // Test the reverse order.
        DepsTracker deps;
        auto me =
            parseMatchExpr("{xyz: {$elemMatch: {$eq: 42, $elemMatch: {$eq: 42}}}, abc: {a: 1}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz", "abc"}));
    }
}

TEST(ElemMatchDependenciesTest, TwoPredicatesUnderElemMatchDeepOddNesting) {
    {
        // Ensure we correctly avoid adding "" to the dependencies.
        DepsTracker deps;
        auto me = parseMatchExpr(
            "{xyz: {$elemMatch: {$elemMatch: {$elemMatch: {$eq: 42}}, $eq: [[42]]}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
    }
    {
        // Test the reverse order.
        DepsTracker deps;
        auto me = parseMatchExpr(
            "{xyz: {$elemMatch: {$eq: [[42]], $elemMatch: {$elemMatch: {$eq: 42}}}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
    }
    {
        // Test bushier tree.
        DepsTracker deps;
        auto me = parseMatchExpr(
            "{xyz: {$elemMatch: {$eq: [[42]], $elemMatch: {$elemMatch: {$eq: 42}, $eq: [42]}}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
    }
}

TEST(ElemMatchDependenciesTest, TwoPredicatesUnderElemMatchDeepEvenNesting) {
    {
        // Ensure we correctly avoid adding "" to the dependencies.
        DepsTracker deps;
        auto me = parseMatchExpr(
            "{xyz: {$elemMatch: {$elemMatch: {$elemMatch: {$elemMatch: {$eq: 42}}}, $eq: [42]}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
    }
    {
        // Test the reverse order.
        DepsTracker deps;
        auto me = parseMatchExpr(
            "{xyz: {$elemMatch: {$eq: 42, $elemMatch: {$elemMatch: {$elemMatch: {$eq: 42}}}}}}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz"}));
    }
    {
        // Test bushier tree.
        DepsTracker deps;
        auto me = parseMatchExpr(
            "{xyz: {$elemMatch: {$elemMatch: {$elemMatch: {$elemMatch: {$eq: 42}}}, $eq: [42]}}, "
            "abc: 123}");
        dependency_analysis::addDependencies(me.get(), &deps);
        ASSERT_EQ(deps.fields, OrderedPathSet({"xyz", "abc"}));
    }
}

}  // namespace mongo
