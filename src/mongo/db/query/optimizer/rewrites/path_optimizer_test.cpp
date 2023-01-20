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
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer {
namespace {
using namespace unit_test_abt_literals;

TEST(Path, Const) {
    auto tree = make<EvalPath>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest"));
    auto env = VariableEnvironment::build(tree);

    auto fusor = PathFusion(env);
    fusor.optimize(tree);

    // The result must be Constant.
    auto result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    // And the value must be 2
    ASSERT_EQ(result->getValueInt64(), 2);
}

TEST(Path, ConstFilter) {
    auto tree = make<EvalFilter>(make<PathConstant>(Constant::int64(2)), make<Variable>("ptest"));
    auto env = VariableEnvironment::build(tree);

    auto fusor = PathFusion(env);
    fusor.optimize(tree);

    // The result must be Constant.
    auto result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    // And the value must be 2
    ASSERT_EQ(result->getValueInt64(), 2);
}

TEST(Path, GetConst) {
    // Get "any" Const 2
    auto tree = make<EvalPath>(make<PathGet>("any", make<PathConstant>(Constant::int64(2))),
                               make<Variable>("ptest"));
    auto env = VariableEnvironment::build(tree);

    auto fusor = PathFusion(env);
    fusor.optimize(tree);

    // The result must be Constant.
    auto result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    // And the value must be 2
    ASSERT_EQ(result->getValueInt64(), 2);
}

TEST(Path, Fuse1) {
    // Field "a" Const 2
    auto field = make<EvalPath>(make<PathField>("a", make<PathConstant>(Constant::int64(2))),
                                make<Variable>("root"));

    // Get "a" Id
    auto get = make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("x"));

    // let x = (Field "a" Const 2 | root)
    //     in  (Get "a" Id | x)
    auto tree = make<Let>("x", std::move(field), std::move(get));
    auto env = VariableEnvironment::build(tree);

    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (PathFusion{env}.optimize(tree)) {
            changed = true;
        }

        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }

    } while (changed);

    // The result must be Constant.
    auto result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    // And the value must be 2
    ASSERT_EQ(result->getValueInt64(), 2);
}

TEST(Path, Fuse2) {
    auto scanNode = make<ScanNode>("root", "test");

    // Field "a" Const 2
    auto field = make<EvalPath>(make<PathField>("a", make<PathConstant>(Constant::int64(2))),
                                make<Variable>("root"));

    auto project1 = make<EvaluationNode>("x", std::move(field), std::move(scanNode));

    // Get "a" Id
    auto get = make<EvalPath>(make<PathGet>("a", make<PathIdentity>()), make<Variable>("x"));
    auto project2 = make<EvaluationNode>("y", std::move(get), std::move(project1));

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"y"}},
                               std::move(project2));

    auto env = VariableEnvironment::build(tree);
    {
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"x", "y", "root"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }

    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (PathFusion{env}.optimize(tree)) {
            changed = true;
        }

        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }

    } while (changed);

    // After rewrites for x projection disappear from the tree.
    {
        ProjectionNameSet projSet = env.topLevelProjections();
        ProjectionNameSet expSet = {"root", "y"};
        ASSERT(expSet == projSet);
        ASSERT(!env.hasFreeVariables());
    }
}

TEST(Path, Fuse3) {
    auto scanNode = make<ScanNode>("root", "test");

    auto project0 = make<EvaluationNode>(
        "z",
        make<EvalPath>(make<PathGet>("z", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    // Field "a" Const Var "z"
    auto field = make<EvalPath>(make<PathField>("a", make<PathConstant>(make<Variable>("z"))),
                                make<Variable>("root"));
    auto project1 = make<EvaluationNode>("x", std::move(field), std::move(project0));

    // Get "a" Traverse Const 2
    auto get =
        make<EvalPath>(make<PathGet>("a",
                                     make<PathTraverse>(PathTraverse::kUnlimited,
                                                        make<PathConstant>(Constant::int64(2)))),
                       make<Variable>("x"));
    auto project2 = make<EvaluationNode>("y", std::move(get), std::move(project1));

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"y"}},
                               std::move(project2));
    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    y\n"
        "  RefBlock: \n"
        "    Variable [y]\n"
        "  Evaluation [{y}]\n"
        "    EvalPath []\n"
        "      PathGet [a]\n"
        "        PathTraverse [inf]\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "      Variable [x]\n"
        "    Evaluation [{x}]\n"
        "      EvalPath []\n"
        "        PathField [a]\n"
        "          PathConstant []\n"
        "            Variable [z]\n"
        "        Variable [root]\n"
        "      Evaluation [{z}]\n"
        "        EvalPath []\n"
        "          PathGet [z]\n"
        "            PathIdentity []\n"
        "          Variable [root]\n"
        "        Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    bool changed = false;
    do {
        changed = false;
        if (PathFusion{env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    y\n"
        "  RefBlock: \n"
        "    Variable [y]\n"
        "  Evaluation [{y}]\n"
        "    EvalPath []\n"
        "      PathGet [z]\n"
        "        PathTraverse [inf]\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "      Variable [root]\n"
        "    Scan [test, {root}]\n",
        tree);
}

TEST(Path, Fuse4) {
    auto scanNode = make<ScanNode>("root", "test");

    auto project0 = make<EvaluationNode>(
        "z",
        make<EvalPath>(make<PathGet>("z", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    auto project01 = make<EvaluationNode>(
        "z1",
        make<EvalPath>(make<PathGet>("z1", make<PathIdentity>()), make<Variable>("root")),
        std::move(project0));

    auto project02 = make<EvaluationNode>(
        "z2",
        make<EvalPath>(make<PathGet>("z2", make<PathIdentity>()), make<Variable>("root")),
        std::move(project01));

    // Field "a" Const Var "z" * Field "b" Const Var "z1"
    auto field = make<EvalPath>(
        make<PathComposeM>(
            make<PathField>("c", make<PathConstant>(make<Variable>("z2"))),
            make<PathComposeM>(make<PathField>("a", make<PathConstant>(make<Variable>("z"))),
                               make<PathField>("b", make<PathConstant>(make<Variable>("z1"))))),
        make<Variable>("root"));
    auto project1 = make<EvaluationNode>("x", std::move(field), std::move(project02));

    // Get "a" Traverse Const 2
    auto get =
        make<EvalPath>(make<PathGet>("a",
                                     make<PathTraverse>(PathTraverse::kUnlimited,
                                                        make<PathConstant>(Constant::int64(2)))),
                       make<Variable>("x"));
    auto project2 = make<EvaluationNode>("y", std::move(get), std::move(project1));

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"x", "y"}},
                               std::move(project2));

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    x\n"
        "    y\n"
        "  RefBlock: \n"
        "    Variable [x]\n"
        "    Variable [y]\n"
        "  Evaluation [{y}]\n"
        "    EvalPath []\n"
        "      PathGet [a]\n"
        "        PathTraverse [inf]\n"
        "          PathConstant []\n"
        "            Const [2]\n"
        "      Variable [x]\n"
        "    Evaluation [{x}]\n"
        "      EvalPath []\n"
        "        PathComposeM []\n"
        "          PathField [c]\n"
        "            PathConstant []\n"
        "              Variable [z2]\n"
        "          PathComposeM []\n"
        "            PathField [a]\n"
        "              PathConstant []\n"
        "                Variable [z]\n"
        "            PathField [b]\n"
        "              PathConstant []\n"
        "                Variable [z1]\n"
        "        Variable [root]\n"
        "      Evaluation [{z2}]\n"
        "        EvalPath []\n"
        "          PathGet [z2]\n"
        "            PathIdentity []\n"
        "          Variable [root]\n"
        "        Evaluation [{z1}]\n"
        "          EvalPath []\n"
        "            PathGet [z1]\n"
        "              PathIdentity []\n"
        "            Variable [root]\n"
        "          Evaluation [{z}]\n"
        "            EvalPath []\n"
        "              PathGet [z]\n"
        "                PathIdentity []\n"
        "              Variable [root]\n"
        "            Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    bool changed = false;
    do {
        changed = false;
        if (PathFusion{env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    x\n"
        "    y\n"
        "  RefBlock: \n"
        "    Variable [x]\n"
        "    Variable [y]\n"
        "  Evaluation [{y}]\n"
        "    EvalPath []\n"
        "      PathTraverse [inf]\n"
        "        PathConstant []\n"
        "          Const [2]\n"
        "      Variable [z]\n"
        "    Evaluation [{x}]\n"
        "      EvalPath []\n"
        "        PathComposeM []\n"
        "          PathField [c]\n"
        "            PathConstant []\n"
        "              EvalPath []\n"
        "                PathGet [z2]\n"
        "                  PathIdentity []\n"
        "                Variable [root]\n"
        "          PathComposeM []\n"
        "            PathField [a]\n"
        "              PathConstant []\n"
        "                Variable [z]\n"
        "            PathField [b]\n"
        "              PathConstant []\n"
        "                EvalPath []\n"
        "                  PathGet [z1]\n"
        "                    PathIdentity []\n"
        "                  Variable [root]\n"
        "        Variable [root]\n"
        "      Evaluation [{z}]\n"
        "        EvalPath []\n"
        "          PathGet [z]\n"
        "            PathIdentity []\n"
        "          Variable [root]\n"
        "        Scan [test, {root}]\n",
        tree);
}

TEST(Path, Fuse5) {
    auto scanNode = make<ScanNode>("root", "test");

    auto project = make<EvaluationNode>(
        "x",
        make<EvalPath>(make<PathKeep>(FieldNameOrderedSet{"a", "b", "c"}), make<Variable>("root")),
        std::move(scanNode));

    // Get "a" Traverse Compare= 2
    auto filter = make<FilterNode>(
        make<EvalFilter>(make<PathGet>("a",
                                       make<PathTraverse>(
                                           PathTraverse::kSingleLevel,
                                           make<PathCompare>(Operations::Eq, Constant::int64(2)))),
                         make<Variable>("x")),
        std::move(project));

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"x"}},
                               std::move(filter));

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    x\n"
        "  RefBlock: \n"
        "    Variable [x]\n"
        "  Filter []\n"
        "    EvalFilter []\n"
        "      PathGet [a]\n"
        "        PathTraverse [1]\n"
        "          PathCompare [Eq]\n"
        "            Const [2]\n"
        "      Variable [x]\n"
        "    Evaluation [{x}]\n"
        "      EvalPath []\n"
        "        PathKeep [a, b, c]\n"
        "        Variable [root]\n"
        "      Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    bool changed = false;
    do {
        changed = false;
        if (PathFusion{env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    // The filter now refers directly to the root projection.
    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    x\n"
        "  RefBlock: \n"
        "    Variable [x]\n"
        "  Filter []\n"
        "    EvalFilter []\n"
        "      PathGet [a]\n"
        "        PathTraverse [1]\n"
        "          PathCompare [Eq]\n"
        "            Const [2]\n"
        "      Variable [root]\n"
        "    Evaluation [{x}]\n"
        "      EvalPath []\n"
        "        PathKeep [a, b, c]\n"
        "        Variable [root]\n"
        "      Scan [test, {root}]\n",
        tree);
}

TEST(Path, Fuse6) {
    auto scanNode = make<ScanNode>("root", "test");

    auto project = make<EvaluationNode>(
        "x",
        make<EvalPath>(make<PathComposeM>(
                           make<PathComposeM>(make<PathObj>(),
                                              make<PathKeep>(FieldNameOrderedSet{"a", "b", "c"})),
                           make<PathField>("a", make<PathConstant>(Constant::emptyObject()))),
                       make<Variable>("root")),
        std::move(scanNode));

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"x"}},
                               std::move(project));

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    x\n"
        "  RefBlock: \n"
        "    Variable [x]\n"
        "  Evaluation [{x}]\n"
        "    EvalPath []\n"
        "      PathComposeM []\n"
        "        PathComposeM []\n"
        "          PathObj []\n"
        "          PathKeep [a, b, c]\n"
        "        PathField [a]\n"
        "          PathConstant []\n"
        "            Const [{}]\n"
        "      Variable [root]\n"
        "    Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    bool changed = false;
    do {
        changed = false;
        if (PathFusion{env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    // PathObj is removed.
    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    x\n"
        "  RefBlock: \n"
        "    Variable [x]\n"
        "  Evaluation [{x}]\n"
        "    EvalPath []\n"
        "      PathComposeM []\n"
        "        PathKeep [a, b, c]\n"
        "        PathField [a]\n"
        "          PathConstant []\n"
        "            Const [{}]\n"
        "      Variable [root]\n"
        "    Scan [test, {root}]\n",
        tree);
}

TEST(Path, Fuse7) {
    auto scanNode = make<ScanNode>("root", "test");

    auto project1 = make<EvaluationNode>(
        "px",
        make<EvalPath>(make<PathGet>("x", make<PathIdentity>()), make<Variable>("root")),
        std::move(scanNode));

    auto project2 = make<EvaluationNode>(
        "py",
        make<EvalPath>(
            make<PathComposeM>(
                make<PathComposeM>(make<PathKeep>(FieldNameOrderedSet{"a"}),
                                   make<PathField>("a", make<PathConstant>(make<Variable>("px")))),
                make<PathDefault>(Constant::emptyObject())),
            make<Variable>("root")),
        std::move(project1));

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"py"}},
                               std::move(project2));

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    py\n"
        "  RefBlock: \n"
        "    Variable [py]\n"
        "  Evaluation [{py}]\n"
        "    EvalPath []\n"
        "      PathComposeM []\n"
        "        PathComposeM []\n"
        "          PathKeep [a]\n"
        "          PathField [a]\n"
        "            PathConstant []\n"
        "              Variable [px]\n"
        "        PathDefault []\n"
        "          Const [{}]\n"
        "      Variable [root]\n"
        "    Evaluation [{px}]\n"
        "      EvalPath []\n"
        "        PathGet [x]\n"
        "          PathIdentity []\n"
        "        Variable [root]\n"
        "      Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    bool changed = false;
    do {
        changed = false;
        if (PathFusion{env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    // Obtain "x" and directly assign at "a".
    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    py\n"
        "  RefBlock: \n"
        "    Variable [py]\n"
        "  Evaluation [{py}]\n"
        "    EvalPath []\n"
        "      PathField [a]\n"
        "        PathConstant []\n"
        "          EvalPath []\n"
        "            PathGet [x]\n"
        "              PathIdentity []\n"
        "            Variable [root]\n"
        "      Const [{}]\n"
        "    Scan [test, {root}]\n",
        tree);
}

void runPathLowering(VariableEnvironment& env, PrefixId& prefixId, ABT& tree) {
    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (PathLowering{prefixId, env}.optimize(tree)) {
            changed = true;
        }
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);
}

TEST(Path, LowerPathIdentity) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(make<PathIdentity>(), make<Variable>("foo"));
    auto env = VariableEnvironment::build(tree);
    runPathLowering(env, prefixId, tree);

    ASSERT(tree.is<Variable>());
    ASSERT_EQ(tree.cast<Variable>()->name(), "foo");
}

TEST(Path, LowerPathConstant) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(make<PathConstant>(Constant::int64(10)), make<Variable>("foo"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT(tree.is<Constant>());
    ASSERT_EQ(tree.cast<Constant>()->getValueInt64(), 10);
}

TEST(Path, LowerPathLambda) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(
        make<PathLambda>(make<LambdaAbstraction>(
            "x", make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(1)))),
        Constant::int64(9));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT(tree.is<Constant>());
    ASSERT_EQ(tree.cast<Constant>()->getValueInt64(), 10);
}

TEST(Path, LowerPathGet) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(
        make<PathGet>(
            "fieldA",
            make<PathGet>("fieldB",
                          /*make<PathIdentity>()*/ make<PathConstant>(Constant::int64(100)))),
        make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT(tree.is<Constant>());
    ASSERT_EQ(tree.cast<Constant>()->getValueInt64(), 100);
}

TEST(Path, LowerPathGetPathLambda) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(
        make<PathGet>(
            "fieldA",
            make<PathGet>(
                "fieldB",
                make<PathLambda>(make<LambdaAbstraction>(
                    "x",
                    make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(1)))))),
        make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN_AUTO(
        "BinaryOp [Add]\n"
        "  FunctionCall [getField]\n"
        "    FunctionCall [getField]\n"
        "      Variable [rootObj]\n"
        "      Const [\"fieldA\"]\n"
        "    Const [\"fieldB\"]\n"
        "  Const [1]\n",
        tree);
}

TEST(Path, ProjElim1) {
    auto prefixId = PrefixId::createForTests();

    auto scanNode = make<ScanNode>("root", "test");

    auto expr1 = make<FunctionCall>("anyFunctionWillDo", makeSeq(make<Variable>("root")));
    auto project1 = make<EvaluationNode>("x", std::move(expr1), std::move(scanNode));

    auto expr2 = make<Variable>("x");
    auto project2 = make<EvaluationNode>("y", std::move(expr2), std::move(project1));

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{"y"}},
                               std::move(project2));

    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    y\n"
        "  RefBlock: \n"
        "    Variable [y]\n"
        "  Evaluation [{y}]\n"
        "    FunctionCall [anyFunctionWillDo]\n"
        "      Variable [root]\n"
        "    Scan [test, {root}]\n",
        tree);
}

TEST(Path, ProjElim2) {
    auto prefixId = PrefixId::createForTests();

    auto scanNode = make<ScanNode>("root", "test");

    auto expr1 = make<FunctionCall>("anyFunctionWillDo", makeSeq(make<Variable>("root")));
    auto project1 = make<EvaluationNode>("x", std::move(expr1), std::move(scanNode));

    auto expr2 = make<Variable>("x");
    auto project2 = make<EvaluationNode>("y", std::move(expr2), std::move(project1));

    auto tree = make<RootNode>(properties::ProjectionRequirement{{}}, std::move(project2));

    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "  RefBlock: \n"
        "  Scan [test, {root}]\n",
        tree);
}

TEST(Path, ProjElim3) {
    auto node = make<ScanNode>("root", "test");
    ProjectionName var{"root"};
    for (int i = 0; i < 100; ++i) {
        ProjectionName newVar{"p" + std::to_string(i)};
        node = make<EvaluationNode>(
            newVar,
            // make<FunctionCall>("anyFunctionWillDo", makeSeq(make<Variable>(var))),
            make<Variable>(var),
            std::move(node));
        var = newVar;
    }

    auto tree = make<RootNode>(properties::ProjectionRequirement{ProjectionNameVector{var}},
                               std::move(node));

    auto env = VariableEnvironment::build(tree);

    // Run rewriters while things change
    bool changed = false;
    do {
        changed = false;
        if (ConstEval{env}.optimize(tree)) {
            changed = true;
        }
    } while (changed);

    ASSERT_EXPLAIN_AUTO(
        "Root []\n"
        "  projections: \n"
        "    p99\n"
        "  RefBlock: \n"
        "    Variable [p99]\n"
        "  Evaluation [{p99}]\n"
        "    Variable [root]\n"
        "    Scan [test, {root}]\n",
        tree);
}

TEST(Path, LowerNestedPathGet) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(
        make<PathGet>("fieldA", make<PathGet>("fieldB", make<PathDefault>(Constant::int64(0)))),
        make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN(
        "Let [valDefault_0]\n"
        "  FunctionCall [getField]\n"
        "    FunctionCall [getField]\n"
        "      Variable [rootObj]\n"
        "      Const [\"fieldA\"]\n"
        "    Const [\"fieldB\"]\n"
        "  If []\n"
        "    FunctionCall [exists]\n"
        "      Variable [valDefault_0]\n"
        "    Variable [valDefault_0]\n"
        "    Const [0]\n",
        tree);
}

TEST(Path, LowerPathTraverse) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(
        make<PathGet>(
            "fieldA",
            make<PathTraverse>(PathTraverse::kUnlimited,
                               make<PathGet>("fieldB", make<PathDefault>(Constant::int64(0))))),
        make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN(
        "FunctionCall [traverseP]\n"
        "  FunctionCall [getField]\n"
        "    Variable [rootObj]\n"
        "    Const [\"fieldA\"]\n"
        "  LambdaAbstraction [inputGet_0]\n"
        "    Let [valDefault_0]\n"
        "      FunctionCall [getField]\n"
        "        Variable [inputGet_0]\n"
        "        Const [\"fieldB\"]\n"
        "      If []\n"
        "        FunctionCall [exists]\n"
        "          Variable [valDefault_0]\n"
        "        Variable [valDefault_0]\n"
        "        Const [0]\n"
        "  Const [Nothing]\n",
        tree);
}

TEST(Path, LowerPathComposeMIdentity) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(
        make<PathComposeM>(make<PathIdentity>(), make<PathConstant>(Constant::int64(100))),
        make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT(tree.is<Constant>());
    ASSERT_EQ(tree.cast<Constant>()->getValueInt64(), 100);
}

TEST(Path, LowerPathComposeMPathGet) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(make<PathComposeM>(make<PathGet>("fieldA", make<PathIdentity>()),
                                                  make<PathConstant>(Constant::int64(100))),
                               make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT(tree.is<Constant>());
    ASSERT_EQ(tree.cast<Constant>()->getValueInt64(), 100);
}

TEST(Path, LowerPathField) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalPath>(
        make<PathField>(
            "fieldA",
            make<PathTraverse>(PathTraverse::kUnlimited,
                               make<PathField>("fieldB", make<PathDefault>(Constant::int64(0))))),
        make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN(
        "Let [valField_1]\n"
        "  FunctionCall [traverseP]\n"
        "    FunctionCall [getField]\n"
        "      Variable [rootObj]\n"
        "      Const [\"fieldA\"]\n"
        "    LambdaAbstraction [inputField_0]\n"
        "      Let [valField_0]\n"
        "        Let [valDefault_0]\n"
        "          FunctionCall [getField]\n"
        "            Variable [inputField_0]\n"
        "            Const [\"fieldB\"]\n"
        "          If []\n"
        "            FunctionCall [exists]\n"
        "              Variable [valDefault_0]\n"
        "            Variable [valDefault_0]\n"
        "            Const [0]\n"
        "        If []\n"
        "          BinaryOp [Or]\n"
        "            FunctionCall [exists]\n"
        "              Variable [valField_0]\n"
        "            FunctionCall [isObject]\n"
        "              Variable [inputField_0]\n"
        "          FunctionCall [setField]\n"
        "            Variable [inputField_0]\n"
        "            Const [\"fieldB\"]\n"
        "            Variable [valField_0]\n"
        "          Variable [inputField_0]\n"
        "    Const [Nothing]\n"
        "  If []\n"
        "    BinaryOp [Or]\n"
        "      FunctionCall [exists]\n"
        "        Variable [valField_1]\n"
        "      FunctionCall [isObject]\n"
        "        Variable [rootObj]\n"
        "    FunctionCall [setField]\n"
        "      Variable [rootObj]\n"
        "      Const [\"fieldA\"]\n"
        "      Variable [valField_1]\n"
        "    Variable [rootObj]\n",
        tree);
}

TEST(Path, LowerPathCompare) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                 make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);
    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN(
        "BinaryOp [FillEmpty]\n"
        "  BinaryOp [Eq]\n"
        "    Variable [root]\n"
        "    Const [1]\n"
        "  Const [false]\n",
        tree);
}

TEST(Path, LowerPathDrop) {
    auto prefixId = PrefixId::createForTests();

    FieldNameOrderedSet names;
    names.insert("a");
    names.insert("b");
    auto tree = make<EvalPath>(make<PathDrop>(std::move(names)), make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);
    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN(
        "If []\n"
        "  FunctionCall [isObject]\n"
        "    Variable [root]\n"
        "  FunctionCall [dropFields]\n"
        "    Variable [root]\n"
        "    Const [\"a\"]\n"
        "    Const [\"b\"]\n"
        "  Variable [root]\n",
        tree);
}

TEST(Path, LowerPathKeep) {
    auto prefixId = PrefixId::createForTests();

    FieldNameOrderedSet names;
    names.insert("a");
    names.insert("b");
    auto tree = make<EvalPath>(make<PathKeep>(std::move(names)), make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);
    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN(
        "If []\n"
        "  FunctionCall [isObject]\n"
        "    Variable [root]\n"
        "  FunctionCall [keepFields]\n"
        "    Variable [root]\n"
        "    Const [\"a\"]\n"
        "    Const [\"b\"]\n"
        "  Variable [root]\n",
        tree);
}

TEST(Path, LowerPathGetWithObj) {
    auto prefixId = PrefixId::createForTests();
    auto tree = make<EvalFilter>(make<PathGet>("a", make<PathObj>()), make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);
    runPathLowering(env, prefixId, tree);
    ASSERT_EXPLAIN(
        "BinaryOp [FillEmpty]\n"
        "  FunctionCall [isObject]\n"
        "    FunctionCall [getField]\n"
        "      Variable [root]\n"
        "      Const [\"a\"]\n"
        "  Const [false]\n",
        tree);
}

TEST(Path, LowerPathGetWithArr) {
    auto prefixId = PrefixId::createForTests();
    auto tree = make<EvalFilter>(make<PathGet>("a", make<PathArr>()), make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);
    runPathLowering(env, prefixId, tree);
    ASSERT_EXPLAIN(
        "BinaryOp [FillEmpty]\n"
        "  FunctionCall [isArray]\n"
        "    FunctionCall [getField]\n"
        "      Variable [root]\n"
        "      Const [\"a\"]\n"
        "  Const [false]\n",
        tree);
}

TEST(Path, LowerPathComposeA) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalFilter>(make<PathComposeA>(make<PathConstant>(Constant::boolean(false)),
                                                    make<PathConstant>(Constant::boolean(true))),
                                 make<Variable>("rootObj"));
    auto env = VariableEnvironment::build(tree);

    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN(
        "BinaryOp [FillEmpty]\n"
        "  If []\n"
        "    BinaryOp [FillEmpty]\n"
        "      Const [false]\n"
        "      Const [false]\n"
        "    Const [true]\n"
        "    Const [true]\n"
        "  Const [false]\n",
        tree);
}

TEST(Path, PathComposeLambdaLHS) {
    auto tree =
        _evalp(_get("a", _id()),
               _evalp(_plambda(_lambda("x", _binary("Add", "x"_var, "1"_cint64))), "9"_cint64))
            ._n;
    auto env = VariableEnvironment::build(tree);

    auto fusor = PathFusion(env);
    fusor.optimize(tree);

    // PathLambda should be the left child.
    ASSERT_EXPLAIN_V2(
        "EvalPath []\n"
        "|   Const [9]\n"
        "PathComposeM []\n"
        "|   PathGet [a]\n"
        "|   PathIdentity []\n"
        "PathLambda []\n"
        "LambdaAbstraction [x]\n"
        "BinaryOp [Add]\n"
        "|   Const [1]\n"
        "Variable [x]\n",
        tree);
}

TEST(Path, NoLambdaPathCompose) {
    auto tree = make<EvalFilter>(
        make<PathGet>("a", make<PathIdentity>()),
        make<EvalPath>(
            make<PathLambda>(make<LambdaAbstraction>(
                "x", make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(1)))),
            Constant::int64(9)));
    auto env = VariableEnvironment::build(tree);

    auto fusor = PathFusion(env);
    fusor.optimize(tree);

    ASSERT_EXPLAIN(
        "EvalFilter []\n"
        "  PathGet [a]\n"
        "    PathIdentity []\n"
        "  EvalPath []\n"
        "    PathLambda []\n"
        "      LambdaAbstraction [x]\n"
        "        BinaryOp [Add]\n"
        "          Variable [x]\n"
        "          Const [1]\n"
        "    Const [9]\n",
        tree);
}

TEST(Path, NoDefaultSimplifyUnderFilter) {
    auto prefixId = PrefixId::createForTests();
    auto nonNothingCompare = make<PathCompare>(Operations::Gt, Constant::int64(70));
    auto pathDefault = make<PathDefault>(Constant::emptyObject());

    {
        auto tree = make<EvalFilter>(make<PathComposeM>(nonNothingCompare, pathDefault),
                                     make<Variable>("root"));
        auto env = VariableEnvironment::build(tree);

        auto fusor = PathFusion(env);
        fusor.optimize(tree);

        ASSERT_EXPLAIN(
            "EvalFilter []\n"
            "  PathComposeM []\n"
            "    PathCompare [Gt]\n"
            "      Const [70]\n"
            "    PathDefault []\n"
            "      Const [{}]\n"
            "  Variable [root]\n",
            tree);
    }

    {
        auto tree = make<EvalFilter>(
            make<PathComposeM>(std::move(pathDefault), std::move(nonNothingCompare)),
            make<Variable>("root"));
        auto env = VariableEnvironment::build(tree);

        auto fusor = PathFusion(env);
        fusor.optimize(tree);

        ASSERT_EXPLAIN(
            "EvalFilter []\n"
            "  PathComposeM []\n"
            "    PathDefault []\n"
            "      Const [{}]\n"
            "    PathCompare [Gt]\n"
            "      Const [70]\n"
            "  Variable [root]\n",
            tree);
    }
}

}  // namespace
}  // namespace mongo::optimizer
