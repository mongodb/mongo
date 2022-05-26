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

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {
namespace {

TEST(Optimizer, ConstEval) {
    // 1 + 2
    auto tree = make<BinaryOp>(Operations::Add, Constant::int64(1), Constant::int64(2));

    // Run the evaluator.
    auto env = VariableEnvironment::build(tree);
    ConstEval evaluator{env};
    evaluator.optimize(tree);

    // The result must be Constant.
    auto result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    // And the value must be 3 (i.e. 1+2).
    ASSERT_EQ(result->getValueInt64(), 3);

    ASSERT_NE(ABT::tagOf<Constant>(), ABT::tagOf<BinaryOp>());
    ASSERT_EQ(tree.tagOf(), ABT::tagOf<Constant>());
}

TEST(Optimizer, ConstEvalCompose) {
    // (1 + 2) + 3
    auto tree =
        make<BinaryOp>(Operations::Add,
                       make<BinaryOp>(Operations::Add, Constant::int64(1), Constant::int64(2)),
                       Constant::int64(3));

    // Run the evaluator.
    auto env = VariableEnvironment::build(tree);
    ConstEval evaluator{env};
    evaluator.optimize(tree);

    // The result must be Constant.
    auto result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    // And the value must be 6 (i.e. 1+2+3).
    ASSERT_EQ(result->getValueInt64(), 6);
}

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

    ABT rootNode =
        make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"P1", "ptest"}},
                       std::move(evalNode));

    auto env = VariableEnvironment::build(rootNode);
    ASSERT(!env.hasFreeVariables());
}

TEST(Optimizer, Tracker2) {
    ABT expr = make<Let>("x",
                         Constant::int64(4),
                         make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("x")));

    auto env = VariableEnvironment::build(expr);
    ConstEval evaluator{env};
    evaluator.optimize(expr);

    // The result must be Constant.
    auto result = expr.cast<Constant>();
    ASSERT(result != nullptr);

    // And the value must be 8 (i.e. x+x = 4+4 = 8).
    ASSERT_EQ(result->getValueInt64(), 8);
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
    auto vars = VariableEnvironment::getVariables(filterNode.cast<FilterNode>()->getFilter());
    ASSERT(vars._variables.size() == 1);
    // Get all definitions from the scan and below (even though there is nothing below the scan).
    auto defs = env.getDefinitions(scanNodeRef);
    // Make sure that variables are defined by the scan (and not by Eval).
    for (const Variable& v : vars._variables) {
        auto it = defs.find(v.name());
        ASSERT(it != defs.end());
        ASSERT(it->second.definedBy == env.getDefinition(v).definedBy);
    }
}

TEST(Optimizer, RefExplain) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    ASSERT_EXPLAIN(
        "Scan [test]\n"
        "  BindBlock:\n"
        "    [ptest]\n"
        "      Source []\n",
        scanNode);

    // Now repeat for the reference type.
    auto ref = scanNode.ref();
    ASSERT_EXPLAIN(
        "Scan [test]\n"
        "  BindBlock:\n"
        "    [ptest]\n"
        "      Source []\n",
        ref);

    ASSERT_EQ(scanNode.tagOf(), ref.tagOf());
}

TEST(Optimizer, CoScan) {
    ABT coScanNode = make<CoScanNode>();
    ABT limitNode =
        make<LimitSkipNode>(properties::LimitSkipRequirement(1, 0), std::move(coScanNode));

    VariableEnvironment venv = VariableEnvironment::build(limitNode);
    ASSERT_TRUE(!venv.hasFreeVariables());

    ASSERT_EXPLAIN(
        "LimitSkip []\n"
        "  limitSkip:\n"
        "    limit: 1\n"
        "    skip: 0\n"
        "  CoScan []\n",
        limitNode);
}

TEST(Optimizer, Basic) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    ASSERT_EXPLAIN(
        "Scan [test]\n"
        "  BindBlock:\n"
        "    [ptest]\n"
        "      Source []\n",
        scanNode);

    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathConstant>(make<UnaryOp>(Operations::Neg, Constant::int64(1))),
                         make<Variable>("ptest")),
        std::move(scanNode));
    ABT evalNode = make<EvaluationNode>(
        "P1",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest")),
        std::move(filterNode));

    ABT rootNode =
        make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"P1", "ptest"}},
                       std::move(evalNode));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    P1\n"
        "    ptest\n"
        "  RefBlock: \n"
        "    Variable [P1]\n"
        "    Variable [ptest]\n"
        "  Evaluation []\n"
        "    BindBlock:\n"
        "      [P1]\n"
        "        EvalPath []\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "          Variable [ptest]\n"
        "    Filter []\n"
        "      EvalFilter []\n"
        "        PathConstant []\n"
        "          UnaryOp [Neg]\n"
        "            Const [1]\n"
        "        Variable [ptest]\n"
        "      Scan [test]\n"
        "        BindBlock:\n"
        "          [ptest]\n"
        "            Source []\n",
        rootNode);


    ABT clonedNode = rootNode;
    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    P1\n"
        "    ptest\n"
        "  RefBlock: \n"
        "    Variable [P1]\n"
        "    Variable [ptest]\n"
        "  Evaluation []\n"
        "    BindBlock:\n"
        "      [P1]\n"
        "        EvalPath []\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "          Variable [ptest]\n"
        "    Filter []\n"
        "      EvalFilter []\n"
        "        PathConstant []\n"
        "          UnaryOp [Neg]\n"
        "            Const [1]\n"
        "        Variable [ptest]\n"
        "      Scan [test]\n"
        "        BindBlock:\n"
        "          [ptest]\n"
        "            Source []\n",
        clonedNode);

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

    ABT rootNode = make<RootNode>(
        properties::ProjectionRequirement{ProjectionNameVector{"p1", "p2", "a1", "a2"}},
        std::move(groupByNode));

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    p1\n"
        "    p2\n"
        "    a1\n"
        "    a2\n"
        "  RefBlock: \n"
        "    Variable [a1]\n"
        "    Variable [a2]\n"
        "    Variable [p1]\n"
        "    Variable [p2]\n"
        "  GroupBy []\n"
        "    groupings: \n"
        "      RefBlock: \n"
        "        Variable [p1]\n"
        "        Variable [p2]\n"
        "    aggregations: \n"
        "      [a1]\n"
        "        Const [10]\n"
        "      [a2]\n"
        "        Const [11]\n"
        "    Evaluation []\n"
        "      BindBlock:\n"
        "        [p3]\n"
        "          Const [3]\n"
        "      Evaluation []\n"
        "        BindBlock:\n"
        "          [p2]\n"
        "            Const [2]\n"
        "        Evaluation []\n"
        "          BindBlock:\n"
        "            [p1]\n"
        "              Const [1]\n"
        "          Scan [test]\n"
        "            BindBlock:\n"
        "              [ptest]\n"
        "                Source []\n",
        rootNode);

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

    ABT rootNode =
        make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"ptest", "B"}},
                       std::move(unionNode));

    {
        auto env = VariableEnvironment::build(rootNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"ptest", "B"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    ptest\n"
        "    B\n"
        "  RefBlock: \n"
        "    Variable [B]\n"
        "    Variable [ptest]\n"
        "  Union []\n"
        "    BindBlock:\n"
        "      [B]\n"
        "        Source []\n"
        "      [ptest]\n"
        "        Source []\n"
        "    Evaluation []\n"
        "      BindBlock:\n"
        "        [B]\n"
        "          Const [3]\n"
        "      Scan [test]\n"
        "        BindBlock:\n"
        "          [ptest]\n"
        "            Source []\n"
        "    Evaluation []\n"
        "      BindBlock:\n"
        "        [B]\n"
        "          Const [4]\n"
        "      Scan [test]\n"
        "        BindBlock:\n"
        "          [ptest]\n"
        "            Source []\n"
        "    Evaluation []\n"
        "      BindBlock:\n"
        "        [B]\n"
        "          Const [5]\n"
        "      Evaluation []\n"
        "        BindBlock:\n"
        "          [ptest]\n"
        "            EvalPath []\n"
        "              PathConstant []\n"
        "                Const [2]\n"
        "              Variable [ptest1]\n"
        "        Scan [test]\n"
        "          BindBlock:\n"
        "            [ptest1]\n"
        "              Source []\n",
        rootNode);
}

TEST(Optimizer, Unwind) {
    ABT scanNode = make<ScanNode>("p1", "test");
    ABT evalNode = make<EvaluationNode>(
        "p2",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("p1")),
        std::move(scanNode));
    ABT unwindNode = make<UnwindNode>("p2", "p2pid", true /*retainNonArrays*/, std::move(evalNode));

    // Make a copy of unwindNode as it will be used later again in the wind test.
    ABT rootNode = make<RootNode>(
        properties::ProjectionRequirement{ProjectionNameVector{"p1", "p2", "p2pid"}}, unwindNode);

    {
        auto env = VariableEnvironment::build(rootNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"p1", "p2", "p2pid"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }

    ASSERT_EXPLAIN(
        "Root []\n"
        "  projections: \n"
        "    p1\n"
        "    p2\n"
        "    p2pid\n"
        "  RefBlock: \n"
        "    Variable [p1]\n"
        "    Variable [p2]\n"
        "    Variable [p2pid]\n"
        "  Unwind [retainNonArrays]\n"
        "    BindBlock:\n"
        "      [p2]\n"
        "        Source []\n"
        "      [p2pid]\n"
        "        Source []\n"
        "    Evaluation []\n"
        "      BindBlock:\n"
        "        [p2]\n"
        "          EvalPath []\n"
        "            PathConstant []\n"
        "              Const [2]\n"
        "            Variable [p1]\n"
        "      Scan [test]\n"
        "        BindBlock:\n"
        "          [p1]\n"
        "            Source []\n",
        rootNode);
}

TEST(Optimizer, Collation) {
    ABT scanNode = make<ScanNode>("a", "test");
    ABT evalNode = make<EvaluationNode>(
        "b",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("a")),
        std::move(scanNode));

    ABT collationNode =
        make<CollationNode>(properties::CollationRequirement(
                                {{"a", CollationOp::Ascending}, {"b", CollationOp::Clustered}}),
                            std::move(evalNode));
    {
        auto env = VariableEnvironment::build(collationNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"a", "b"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }

    ASSERT_EXPLAIN(
        "Collation []\n"
        "  collation: \n"
        "    a: Ascending\n"
        "    b: Clustered\n"
        "  RefBlock: \n"
        "    Variable [a]\n"
        "    Variable [b]\n"
        "  Evaluation []\n"
        "    BindBlock:\n"
        "      [b]\n"
        "        EvalPath []\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "          Variable [a]\n"
        "    Scan [test]\n"
        "      BindBlock:\n"
        "        [a]\n"
        "          Source []\n",
        collationNode);
}

TEST(Optimizer, LimitSkip) {
    ABT scanNode = make<ScanNode>("a", "test");
    ABT evalNode = make<EvaluationNode>(
        "b",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("a")),
        std::move(scanNode));

    ABT limitSkipNode =
        make<LimitSkipNode>(properties::LimitSkipRequirement(10, 20), std::move(evalNode));
    {
        auto env = VariableEnvironment::build(limitSkipNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"a", "b"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }

    ASSERT_EXPLAIN(
        "LimitSkip []\n"
        "  limitSkip:\n"
        "    limit: 10\n"
        "    skip: 20\n"
        "  Evaluation []\n"
        "    BindBlock:\n"
        "      [b]\n"
        "        EvalPath []\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "          Variable [a]\n"
        "    Scan [test]\n"
        "      BindBlock:\n"
        "        [a]\n"
        "          Source []\n",
        limitSkipNode);
}

TEST(Optimizer, Distribution) {
    ABT scanNode = make<ScanNode>("a", "test");
    ABT evalNode = make<EvaluationNode>(
        "b",
        make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("a")),
        std::move(scanNode));

    ABT exchangeNode = make<ExchangeNode>(
        properties::DistributionRequirement({DistributionType::HashPartitioning, {"b"}}),
        std::move(evalNode));

    ASSERT_EXPLAIN(
        "Exchange []\n"
        "  distribution: \n"
        "    type: HashPartitioning\n"
        "      projections: \n"
        "        b\n"
        "  RefBlock: \n"
        "    Variable [b]\n"
        "  Evaluation []\n"
        "    BindBlock:\n"
        "      [b]\n"
        "        EvalPath []\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "          Variable [a]\n"
        "    Scan [test]\n"
        "      BindBlock:\n"
        "        [a]\n"
        "          Source []\n",
        exchangeNode);
}

TEST(Properties, Basic) {
    using namespace properties;

    CollationRequirement collation1(
        {{"p1", CollationOp::Ascending}, {"p2", CollationOp::Descending}});
    CollationRequirement collation2(
        {{"p1", CollationOp::Ascending}, {"p2", CollationOp::Clustered}});
    ASSERT_TRUE(collationsCompatible(collation1.getCollationSpec(), collation2.getCollationSpec()));
    ASSERT_FALSE(
        collationsCompatible(collation2.getCollationSpec(), collation1.getCollationSpec()));

    PhysProps props;
    ASSERT_FALSE(hasProperty<CollationRequirement>(props));
    ASSERT_TRUE(setProperty(props, collation1));
    ASSERT_TRUE(hasProperty<CollationRequirement>(props));
    ASSERT_FALSE(setProperty(props, collation2));
    ASSERT_TRUE(collation1 == getProperty<CollationRequirement>(props));

    LimitSkipRequirement ls(10, 20);
    ASSERT_FALSE(hasProperty<LimitSkipRequirement>(props));
    ASSERT_TRUE(setProperty(props, ls));
    ASSERT_TRUE(hasProperty<LimitSkipRequirement>(props));
    ASSERT_TRUE(ls == getProperty<LimitSkipRequirement>(props));

    LimitSkipRequirement ls1(-1, 10);
    LimitSkipRequirement ls2(5, 0);
    {
        LimitSkipRequirement ls3 = ls2;
        combineLimitSkipProperties(ls3, ls1);
        ASSERT_EQ(5, ls3.getLimit());
        ASSERT_EQ(10, ls3.getSkip());
    }
    {
        LimitSkipRequirement ls3 = ls1;
        combineLimitSkipProperties(ls3, ls2);
        ASSERT_EQ(0, ls3.getLimit());
        ASSERT_EQ(0, ls3.getSkip());
    }
}

}  // namespace
}  // namespace mongo::optimizer
