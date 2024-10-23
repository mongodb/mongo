/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

#include "mongo/base/string_data.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"


namespace mongo::optimizer {
namespace {
using namespace unit_test_abt_literals;


TEST(Optimizer, Tracker1) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathConstant>(make<UnaryOp>(Operations::Neg, Constant::int64(1))),
                         make<Variable>("ptest")),
        std::move(scanNode));
    ABT evalNode = make<EvaluationNode>(
        "P1",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest")),
        std::move(filterNode));

    ABT rootNode = make<RootNode>(ProjectionNameVector{"P1", "ptest"}, std::move(evalNode));

    auto env = VariableEnvironment::build(rootNode);
    ASSERT(!env.hasFreeVariables());
}

TEST(Optimizer, Tracker3) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    ABT filterNode = make<FilterNode>(make<Variable>("free"), std::move(scanNode));
    ABT evalNode1 = make<EvaluationNode>("free", Constant::int64(5), std::move(filterNode));

    auto env = VariableEnvironment::build(evalNode1);
    // "free" must still be a free variable.
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("free"), 1);

    // Projecting "unrelated" must not resolve "free".
    ABT evalNode2 = make<EvaluationNode>("unrelated", Constant::int64(5), std::move(evalNode1));

    env.rebuild(evalNode2);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("free"), 1);

    // Another expression referencing "free" will resolve. But the original "free" reference is
    // unaffected (i.e. it is still a free variable).
    ABT filterNode2 = make<FilterNode>(make<Variable>("free"), std::move(evalNode2));

    env.rebuild(filterNode2);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("free"), 1);
}

TEST(Optimizer, Tracker4) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    auto scanNodeRef = scanNode.ref();
    ABT evalNode = make<EvaluationNode>("unrelated", Constant::int64(5), std::move(scanNode));
    ABT filterNode = make<FilterNode>(make<Variable>("ptest"), std::move(evalNode));

    auto env = VariableEnvironment::build(filterNode);
    ASSERT(!env.hasFreeVariables());

    // Get all variables from the expression
    std::vector<std::reference_wrapper<const Variable>> vars;
    VariableEnvironment::walkVariables(filterNode.cast<FilterNode>()->getFilter(),
                                       [&](const Variable& var) { vars.push_back(var); });
    ASSERT(vars.size() == 1);
    // Get all definitions from the scan and below (even though there is nothing below the scan).
    auto defs = env.getDefinitions(scanNodeRef);
    // Make sure that variables are defined by the scan (and not by Eval).
    for (const Variable& v : vars) {
        auto it = defs.find(v.name());
        ASSERT(it != defs.end());
        ASSERT(it->second.definedBy == env.getDefinition(v).definedBy);
    }
}

TEST(Optimizer, Ref) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    auto ref = scanNode.ref();
    ASSERT_EQ(scanNode.tagOf(), ref.tagOf());
}

TEST(Optimizer, CoScan) {
    ABT coScanNode = make<CoScanNode>();
    ABT limitNode = make<LimitSkipNode>(1, 0, std::move(coScanNode));

    VariableEnvironment venv = VariableEnvironment::build(limitNode);
    ASSERT_TRUE(!venv.hasFreeVariables());
}

TEST(Optimizer, Basic) {
    ABT scanNode = make<ScanNode>("ptest", "test");

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathConstant>(make<UnaryOp>(Operations::Neg, Constant::int64(1))),
                         make<Variable>("ptest")),
        std::move(scanNode));
    ABT evalNode = make<EvaluationNode>(
        "P1",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest")),
        std::move(filterNode));

    ABT rootNode = make<RootNode>(ProjectionNameVector{"P1", "ptest"}, std::move(evalNode));

    auto env = VariableEnvironment::build(rootNode);
    ProjectionNameSet set = env.topLevelProjections();
    ProjectionNameSet expSet = {"P1", "ptest"};
    ASSERT(expSet == set);
    ASSERT(!env.hasFreeVariables());
}

TEST(Optimizer, GroupBy) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    ABT evalNode1 = make<EvaluationNode>("p1", Constant::int64(1), std::move(scanNode));
    ABT evalNode2 = make<EvaluationNode>("p2", Constant::int64(2), std::move(evalNode1));
    ABT evalNode3 = make<EvaluationNode>("p3", Constant::int64(3), std::move(evalNode2));

    {
        auto env = VariableEnvironment::build(evalNode3);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"p1", "p2", "p3", "ptest"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }

    ABT agg1 = Constant::int64(10);
    ABT agg2 = Constant::int64(11);
    ABT groupByNode = make<GroupByNode>(ProjectionNameVector{"p1", "p2"},
                                        ProjectionNameVector{"a1", "a2"},
                                        makeSeq(std::move(agg1), std::move(agg2)),
                                        std::move(evalNode3));

    ABT rootNode =
        make<RootNode>(ProjectionNameVector{"p1", "p2", "a1", "a2"}, std::move(groupByNode));

    {
        auto env = VariableEnvironment::build(rootNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"p1", "p2", "a1", "a2"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }
}

TEST(Optimizer, Union) {
    ABT scanNode1 = make<ScanNode>("ptest", "test");
    ABT projNode1 = make<EvaluationNode>("B", Constant::int64(3), std::move(scanNode1));
    ABT scanNode2 = make<ScanNode>("ptest", "test");
    ABT projNode2 = make<EvaluationNode>("B", Constant::int64(4), std::move(scanNode2));
    ABT scanNode3 = make<ScanNode>("ptest1", "test");
    ABT evalNode = make<EvaluationNode>(
        "ptest",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest1")),
        std::move(scanNode3));
    ABT projNode3 = make<EvaluationNode>("B", Constant::int64(5), std::move(evalNode));


    ABT unionNode = make<UnionNode>(ProjectionNameVector{"ptest", "B"},
                                    makeSeq(projNode1, projNode2, projNode3));

    ABT rootNode = make<RootNode>(ProjectionNameVector{"ptest", "B"}, std::move(unionNode));

    {
        auto env = VariableEnvironment::build(rootNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"ptest", "B"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }
}

TEST(Optimizer, UnionReferences) {
    ABT scanNode1 = make<ScanNode>("ptest1", "test1");
    ABT projNode1 = make<EvaluationNode>("A", Constant::int64(3), std::move(scanNode1));
    ABT scanNode2 = make<ScanNode>("ptest2", "test2");
    ABT projNode2 = make<EvaluationNode>("B", Constant::int64(4), std::move(scanNode2));

    ABT unionNode =
        make<UnionNode>(ProjectionNameVector{"ptest3", "C"}, makeSeq(projNode1, projNode2));
    ABTVector unionNodeReferences =
        unionNode.cast<UnionNode>()->get<1>().cast<References>()->nodes();
    ABTVector expectedUnionNodeReferences = {make<Variable>("ptest3"),
                                             make<Variable>("C"),
                                             make<Variable>("ptest3"),
                                             make<Variable>("C")};
    ASSERT(unionNodeReferences == expectedUnionNodeReferences);
}

TEST(Optimizer, Unwind) {
    ABT scanNode = make<ScanNode>("p1", "test");
    ABT evalNode = make<EvaluationNode>(
        "p2",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("p1")),
        std::move(scanNode));
    ABT unwindNode = make<UnwindNode>("p2", "p2pid", true /*retainNonArrays*/, std::move(evalNode));

    // Make a copy of unwindNode as it will be used later again in the wind test.
    ABT rootNode = make<RootNode>(ProjectionNameVector{"p1", "p2", "p2pid"}, unwindNode);

    {
        auto env = VariableEnvironment::build(rootNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"p1", "p2", "p2pid"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }
}

TEST(Optimizer, Collation) {
    ABT scanNode = make<ScanNode>("a", "test");
    ABT evalNode = make<EvaluationNode>(
        "b",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("a")),
        std::move(scanNode));

    ABT collationNode = make<CollationNode>(
        ProjectionCollationSpec({{"a", CollationOp::Ascending}, {"b", CollationOp::Clustered}}),
        std::move(evalNode));
    {
        auto env = VariableEnvironment::build(collationNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"a", "b"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }
}

TEST(Optimizer, LimitSkip) {
    ABT scanNode = make<ScanNode>("a", "test");
    ABT evalNode = make<EvaluationNode>(
        "b",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("a")),
        std::move(scanNode));

    ABT limitSkipNode = make<LimitSkipNode>(10, 20, std::move(evalNode));
    {
        auto env = VariableEnvironment::build(limitSkipNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"a", "b"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }
}

TEST(Properties, Basic) {
    ProjectionCollationSpec collation1(
        {{"p1", CollationOp::Ascending}, {"p2", CollationOp::Descending}});
    ProjectionCollationSpec collation2(
        {{"p1", CollationOp::Ascending}, {"p2", CollationOp::Clustered}});
    ASSERT_TRUE(collationsCompatible(collation1, collation2));
    ASSERT_FALSE(collationsCompatible(collation2, collation1));
}

TEST(Explain, ExplainBsonForConstant) {
    ABT cNode = Constant::int64(3);

    ASSERT_EXPLAIN_BSON(
        "{\n"
        "    nodeType: \"Const\", \n"
        "    tag: \"NumberInt64\", \n"
        "    value: 3\n"
        "}\n",
        cNode);
}

TEST(Optimizer, ExplainRIDUnion) {
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(1)))),
                         make<Variable>("root")),
        make<ScanNode>("root", "c1"));

    ABT unionNode = make<RIDUnionNode>(
        "root", ProjectionNameVector{"root"}, filterNode, make<ScanNode>("root", "c1"));

    ABT rootNode = make<RootNode>(ProjectionNameVector{"root"}, std::move(unionNode));

    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "RIDUnion [root]\n"
        "|   Scan [c1, {root}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a]\n"
        "|   PathTraverse [1]\n"
        "|   PathCompare [Eq]\n"
        "|   Const [1]\n"
        "Scan [c1, {root}]\n",
        rootNode);
}

}  // namespace
}  // namespace mongo::optimizer
