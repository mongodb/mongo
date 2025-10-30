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
