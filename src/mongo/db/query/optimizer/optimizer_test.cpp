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

#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/interval_utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/unittest/unittest.h"


namespace mongo::optimizer {
namespace {
using namespace unit_test_abt_literals;

Constant* constEval(ABT& tree) {
    auto env = VariableEnvironment::build(tree);
    ConstEval evaluator{env};
    evaluator.optimize(tree);

    // The result must be Constant.
    Constant* result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    ASSERT_NE(ABT::tagOf<Constant>(), ABT::tagOf<BinaryOp>());
    ASSERT_EQ(tree.tagOf(), ABT::tagOf<Constant>());
    return result;
}

TEST(Optimizer, ConstEval) {
    // 1 + 2
    ABT tree = _binary("Add", "1"_cint64, "2"_cint64)._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueInt64(), 3);
}


TEST(Optimizer, ConstEvalCompose) {
    // (1 + 2) + 3
    ABT tree = _binary("Add", _binary("Add", "1"_cint64, "2"_cint64), "3"_cint64)._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueInt64(), 6);
}


TEST(Optimizer, ConstEvalCompose2) {
    // 3 - (5 - 4)
    auto tree = _binary("Sub", "3"_cint64, _binary("Sub", "5"_cint64, "4"_cint64))._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueInt64(), 2);
}

TEST(Optimizer, ConstEval3) {
    // 1.5 + 0.5
    auto tree = _binary("Add", "1.5"_cdouble, "0.5"_cdouble)._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueDouble(), 2.0);
}

TEST(Optimizer, ConstEval4) {
    // INT32_MAX (as int) + 0 (as double) => INT32_MAX (as double)
    auto tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MAX), Constant::fromDouble(0));
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueDouble(), INT32_MAX);
}

TEST(Optimizer, ConstEval5) {
    // -1 + -2
    ABT tree1 = make<BinaryOp>(Operations::Add, Constant::int32(-1), Constant::int32(-2));
    ASSERT_EQ(constEval(tree1)->getValueInt32(), -3);
    // 1 + -1
    ABT tree2 = make<BinaryOp>(Operations::Add, Constant::int32(1), Constant::int32(-1));
    ASSERT_EQ(constEval(tree2)->getValueInt32(), 0);
    // 1 + INT32_MIN
    ABT tree3 = make<BinaryOp>(Operations::Add, Constant::int32(1), Constant::int32(INT32_MIN));
    ASSERT_EQ(constEval(tree3)->getValueInt32(), -2147483647);
}

TEST(Optimizer, ConstEval6) {
    // -1 * -2
    ABT tree1 = make<BinaryOp>(Operations::Mult, Constant::int32(-1), Constant::int32(-2));
    ASSERT_EQ(constEval(tree1)->getValueInt32(), 2);
    // 1 * -1
    ABT tree2 = make<BinaryOp>(Operations::Mult, Constant::int32(1), Constant::int32(-1));
    ASSERT_EQ(constEval(tree2)->getValueInt32(), -1);
    // 2 * INT32_MAX
    ABT tree3 = make<BinaryOp>(Operations::Mult, Constant::int32(2), Constant::int32(INT32_MAX));
    ASSERT_EQ(constEval(tree3)->getValueInt64(), 4294967294);
}

TEST(Optimizer, ConstEvalNotNegate) {
    // !true = false
    ABT tree1 = make<UnaryOp>(Operations::Not, Constant::boolean(true));
    ASSERT_EQ(constEval(tree1)->getValueBool(), false);
    // !false = true
    ABT tree2 = make<UnaryOp>(Operations::Not, Constant::boolean(false));
    ASSERT_EQ(constEval(tree2)->getValueBool(), true);
}

TEST(Optimizer, IntegerOverflow) {
    auto int32tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MAX), Constant::int32(1));
    ASSERT_EQ(constEval(int32tree)->getValueInt64(), 2147483648);
}

TEST(Optimizer, IntegerUnderflow) {
    auto int32tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MIN), Constant::int32(-1));
    ASSERT_EQ(constEval(int32tree)->getValueInt64(), -2147483649);

    auto tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MAX), Constant::int64(INT64_MIN));
    ASSERT_EQ(constEval(tree)->getValueInt64(), -9223372034707292161);
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

TEST(Optimizer, RefExplain) {
    ABT scanNode = make<ScanNode>("ptest", "test");
    ASSERT_EXPLAIN_AUTO(           // NOLINT (test auto-update)
        "Scan [test, {ptest}]\n",  // NOLINT (test auto-update)
        scanNode);

    // Now repeat for the reference type.
    auto ref = scanNode.ref();
    ASSERT_EXPLAIN_AUTO(           // NOLINT (test auto-update)
        "Scan [test, {ptest}]\n",  // NOLINT (test auto-update)
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
    ASSERT_EXPLAIN_AUTO(           // NOLINT (test auto-update)
        "Scan [test, {ptest}]\n",  // NOLINT (test auto-update)
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

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    P1\n"
        "    ptest\n"
        "  RefBlock: \n"
        "    Variable [P1]\n"
        "    Variable [ptest]\n"
        "  Evaluation [{P1}]\n"
        "    EvalPath []\n"
        "      PathConstant []\n"
        "        Const [2]\n"
        "      Variable [ptest]\n"
        "    Filter []\n"
        "      EvalFilter []\n"
        "        PathConstant []\n"
        "          UnaryOp [Neg]\n"
        "            Const [1]\n"
        "        Variable [ptest]\n"
        "      Scan [test, {ptest}]\n",
        rootNode);


    ABT clonedNode = rootNode;
    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    P1\n"
        "    ptest\n"
        "  RefBlock: \n"
        "    Variable [P1]\n"
        "    Variable [ptest]\n"
        "  Evaluation [{P1}]\n"
        "    EvalPath []\n"
        "      PathConstant []\n"
        "        Const [2]\n"
        "      Variable [ptest]\n"
        "    Filter []\n"
        "      EvalFilter []\n"
        "        PathConstant []\n"
        "          UnaryOp [Neg]\n"
        "            Const [1]\n"
        "        Variable [ptest]\n"
        "      Scan [test, {ptest}]\n",
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

    ASSERT_EXPLAIN_AUTO(
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
        "    Evaluation [{p3}]\n"
        "      Const [3]\n"
        "      Evaluation [{p2}]\n"
        "        Const [2]\n"
        "        Evaluation [{p1}]\n"
        "          Const [1]\n"
        "          Scan [test, {ptest}]\n",
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

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    ptest\n"
        "    B\n"
        "  RefBlock: \n"
        "    Variable [B]\n"
        "    Variable [ptest]\n"
        "  Union [{B, ptest}]\n"
        "    Evaluation [{B}]\n"
        "      Const [3]\n"
        "      Scan [test, {ptest}]\n"
        "    Evaluation [{B}]\n"
        "      Const [4]\n"
        "      Scan [test, {ptest}]\n"
        "    Evaluation [{B}]\n"
        "      Const [5]\n"
        "      Evaluation [{ptest}]\n"
        "        EvalPath []\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "          Variable [ptest1]\n"
        "        Scan [test, {ptest1}]\n",
        rootNode);
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
    ABT rootNode = make<RootNode>(
        properties::ProjectionRequirement{ProjectionNameVector{"p1", "p2", "p2pid"}}, unwindNode);

    {
        auto env = VariableEnvironment::build(rootNode);
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"p1", "p2", "p2pid"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }

    ASSERT_EXPLAIN_AUTO(
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
        "    Evaluation [{p2}]\n"
        "      EvalPath []\n"
        "        PathConstant []\n"
        "          Const [2]\n"
        "        Variable [p1]\n"
        "      Scan [test, {p1}]\n",
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

    ASSERT_EXPLAIN_AUTO(
        "Collation []\n"
        "  collation: \n"
        "    a: Ascending\n"
        "    b: Clustered\n"
        "  RefBlock: \n"
        "    Variable [a]\n"
        "    Variable [b]\n"
        "  Evaluation [{b}]\n"
        "    EvalPath []\n"
        "      PathConstant []\n"
        "        Const [2]\n"
        "      Variable [a]\n"
        "    Scan [test, {a}]\n",
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

    ASSERT_EXPLAIN_AUTO(
        "LimitSkip []\n"
        "  limitSkip:\n"
        "    limit: 10\n"
        "    skip: 20\n"
        "  Evaluation [{b}]\n"
        "    EvalPath []\n"
        "      PathConstant []\n"
        "        Const [2]\n"
        "      Variable [a]\n"
        "    Scan [test, {a}]\n",
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

    ASSERT_EXPLAIN_AUTO(
        "Exchange []\n"
        "  distribution: \n"
        "    type: HashPartitioning\n"
        "      projections: \n"
        "        b\n"
        "  RefBlock: \n"
        "    Variable [b]\n"
        "  Evaluation [{b}]\n"
        "    EvalPath []\n"
        "      PathConstant []\n"
        "        Const [2]\n"
        "      Variable [a]\n"
        "    Scan [test, {a}]\n",
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

TEST(Explain, ExplainV2Compact) {
    ABT pathNode =
        make<PathGet>("a",
                      make<PathTraverse>(
                          PathTraverse::kSingleLevel,
                          make<PathComposeM>(
                              make<PathCompare>(Operations::Gte,
                                                make<UnaryOp>(Operations::Neg, Constant::int64(2))),
                              make<PathCompare>(Operations::Lt, Constant::int64(7)))));
    ABT scanNode = make<ScanNode>("x1", "test");
    ABT evalNode = make<EvaluationNode>(
        "x2", make<EvalPath>(pathNode, make<Variable>("a")), std::move(scanNode));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Evaluation [{x2}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [a]\n"
        "|   PathGet [a] PathTraverse [1] PathComposeM []\n"
        "|   |   PathCompare [Lt] Const [7]\n"
        "|   PathCompare [Gte] UnaryOp [Neg] Const [2]\n"
        "Scan [test, {x1}]\n",
        evalNode);
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

TEST(IntervalNormalize, Basic) {
    auto intervalExpr = _disj(_conj(_interval(_incl("3"_cint64), _incl("4"_cint64)),
                                    _interval(_incl("1"_cint64), _incl("2"_cint64))),
                              _disj(_interval(_incl("3"_cint64), _incl("4"_cint64)),
                                    _interval(_excl("3"_cint64), _incl("4"_cint64)),
                                    _interval(_incl("3"_cint64), _incl("2"_cint64))),
                              _interval(_incl("5"_cint64), _incl("6"_cint64)));

    ASSERT_INTERVAL(
        "{\n"
        "    {\n"
        "        {[Const [3], Const [4]]}\n"
        "     ^ \n"
        "        {[Const [1], Const [2]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Const [3], Const [4]]}\n"
        "     U \n"
        "        {(Const [3], Const [4]]}\n"
        "     U \n"
        "        {[Const [3], Const [2]]}\n"
        "    }\n"
        " U \n"
        "    {[Const [5], Const [6]]}\n"
        "}\n",
        intervalExpr);

    normalizeIntervals(intervalExpr);

    // Demonstrate that Atoms are sorted first, then conjunctions and disjunctions. Atoms are sorted
    // first on the lower then on the upper bounds.
    ASSERT_INTERVAL(
        "{\n"
        "    {[Const [5], Const [6]]}\n"
        " U \n"
        "    {\n"
        "        {[Const [1], Const [2]]}\n"
        "     ^ \n"
        "        {[Const [3], Const [4]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Const [3], Const [2]]}\n"
        "     U \n"
        "        {[Const [3], Const [4]]}\n"
        "     U \n"
        "        {(Const [3], Const [4]]}\n"
        "    }\n"
        "}\n",
        intervalExpr);
}

TEST(IntervalNormalize, IntervalNormalizeConstantsFirst) {
    auto intervalExpr = _disj(_conj(_interval(_incl("var1"_var), _incl("var2"_var))),
                              _conj(_interval(_incl("3"_cint64), _incl("var2"_var))),
                              _conj(_interval(_incl("7"_cint64), _incl("8"_cint64))),
                              _conj(_interval(_incl("var3"_var), _incl("4"_cint64))),
                              _conj(_interval(_incl("1"_cint64), _incl("5"_cint64))));

    ASSERT_INTERVAL(
        "{\n"
        "    {\n"
        "        {[Variable [var1], Variable [var2]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Const [3], Variable [var2]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Const [7], Const [8]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Variable [var3], Const [4]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Const [1], Const [5]]}\n"
        "    }\n"
        "}\n",
        intervalExpr);

    normalizeIntervals(intervalExpr);

    // Demonstrate that constant intervals are sorted first.
    ASSERT_INTERVAL(
        "{\n"
        "    {\n"
        "        {[Const [1], Const [5]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Const [7], Const [8]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Const [3], Variable [var2]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Variable [var1], Variable [var2]]}\n"
        "    }\n"
        " U \n"
        "    {\n"
        "        {[Variable [var3], Const [4]]}\n"
        "    }\n"
        "}\n",
        intervalExpr);
}

TEST(Optimizer, ExplainRIDUnion) {
    ABT filterNode = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(1)))),
                         make<Variable>("root")),
        make<ScanNode>("root", "c1"));

    ABT unionNode = make<RIDUnionNode>("root", filterNode, make<ScanNode>("root", "c1"));

    ABT rootNode = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"root"}},
                                  std::move(unionNode));

    ASSERT_EXPLAIN_V2_AUTO(
        "Root []\n"
        "|   |   projections: \n"
        "|   |       root\n"
        "|   RefBlock: \n"
        "|       Variable [root]\n"
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
