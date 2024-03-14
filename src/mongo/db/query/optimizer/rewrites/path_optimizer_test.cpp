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

#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "mongo/base/string_data.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
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
        "Root [{y}]\n"
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
        "Root [{y}]\n"
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
        "Root [{x, y}]\n"
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
        "Root [{x, y}]\n"
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
        "Root [{x}]\n"
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
        "Root [{x}]\n"
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
        "Root [{x}]\n"
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
        "Root [{x}]\n"
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
        "Root [{py}]\n"
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
        "Root [{py}]\n"
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
        if (PathLowering{prefixId}.optimize(tree)) {
            changed = true;
            env.rebuild(tree);
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
        "Root [{y}]\n"
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
        "Root [{p99}]\n"
        "  Evaluation [{p99} = Variable [root]]\n"
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

    ASSERT_EXPLAIN_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "  Const [MakeObjSpec([fieldA = MakeObj([fieldB = LambdaArg(0, false)], Open)], Open, "
        "NewObj, 0)]\n"
        "  Variable [rootObj]\n"
        "  Const [false]\n"
        "  LambdaAbstraction [valDefault_0]\n"
        "    If []\n"
        "      FunctionCall [exists]\n"
        "        Variable [valDefault_0]\n"
        "      Variable [valDefault_0]\n"
        "      Const [0]\n",
        tree);
}

TEST(Path, LowerPathCompare) {
    auto prefixId = PrefixId::createForTests();

    auto tree = make<EvalFilter>(make<PathCompare>(Operations::Eq, Constant::int64(1)),
                                 make<Variable>("root"));
    auto env = VariableEnvironment::build(tree);
    runPathLowering(env, prefixId, tree);

    ASSERT_EXPLAIN_AUTO(
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

    ASSERT_EXPLAIN_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "  Const [MakeObjSpec([a, b], Open, RetInput, 0)]\n"
        "  Variable [root]\n"
        "  Const [false]\n",
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

    ASSERT_EXPLAIN_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "  Const [MakeObjSpec([a, b], Closed, RetInput, 0)]\n"
        "  Variable [root]\n"
        "  Const [false]\n",
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

    ASSERT_EXPLAIN_AUTO(  // NOLINT
        "Const [true]\n",
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

TEST(Path, FusePathsWithEqualTraverseDepths) {
    // This ABT sets every element (if its an array) of the field 'a' to 2 (via the PathTraverse) by
    // the bottom-most EvalPath and then retrieves the value of the field 'a' by the top-most
    // PathTraverse.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .eval("x",
                         _evalp(_get("a", _traverseN(_id())),
                                _evalp(_field("a", _traverseN(_pconst("2"_cint64))), "root"_var)))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   EvalPath []\n"
        "|   |   |   Variable [root]\n"
        "|   |   PathField [a] PathTraverse [inf] PathConstant [] Const [2]\n"
        "|   PathGet [a] PathTraverse [inf] PathIdentity []\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrite simplifies the tree such that we simply set every
    // element (if its an array) of the field 'a' to 2 and return the value of the field 'a'.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathGet [a] PathTraverse [inf] PathConstant [] Const [2]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, FuseTraverseWithConstant) {
    // This ABT sets the field 'a' to "hello" and then checks if the value of the field 'a' is equal
    // to 2.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .filter(_evalf(_get("a", _traverse1(_cmp("Eq", "2"_cint64))), "x"_var))
                   .eval("x", _evalp(_field("a", _pconst("hello"_cstr)), "root"_var))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [x]\n"
        "|   PathGet [a] PathTraverse [1] PathCompare [Eq] Const [2]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrite simplifies the tree such that (1) the PathTraverse is
    // eliminated, since the constant that we are setting the field 'a' to is not an array and (2)
    // we directly compare "hello" to 2 instead of retrieving the field 'a' in the Filter. Note that
    // further optimizations with different rewrites (i.e. ConstEval) would simplify the tree
    // further (constant folding could determine that "hello" != 2 and replace the right child of
    // the Filter node with Const [false]). However, this unit test just exercises PathFusion
    // optimizations.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   BinaryOp [Eq]\n"
        "|   |   Const [2]\n"
        "|   Const [\"hello\"]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathConstantArrayBool) {
    // This test tests the rewriting of ABTs with array and boolean constants.
    // This ABT sets the field 'a' to an empty array and the field 'b' to 'true' and then projects
    // those two fields.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .eval("x",
                         _evalp(_composem(_composem(_obj(), _keep("a", "b")),
                                          _composem(_field("a", _pconst(_cemparray())),
                                                    _field("b", _pconst(_cbool(true))))),
                                "root"_var))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathComposeM []\n"
        "|   |   PathComposeM []\n"
        "|   |   |   PathField [b] PathConstant [] Const [true]\n"
        "|   |   PathField [a] PathConstant [] Const [[]]\n"
        "|   PathComposeM []\n"
        "|   |   PathKeep [a, b]\n"
        "|   PathObj []\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrite simplifies the tree such that we directly construct the
    // object {a: [], b: true}
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Const [{}]\n"
        "|   PathComposeM []\n"
        "|   |   PathField [b] PathConstant [] Const [true]\n"
        "|   PathField [a] PathConstant [] Const [[]]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathConstantArrayBool2) {
    // This test tests the rewriting of ABTs with array and boolean constants.
    // This ABT sets the field 'a' to an empty array and the field 'b' to 'true' and then projects
    // those two fields, similar to PathConstantArrayBool. The difference between this test and
    // PathConstantArrayBool is the order of the children under the leftmost PathComposeM (the
    // PathKeep and PathObj). Regardless of this order, we will arrive at the same optimized tree.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .eval("x",
                         _evalp(_composem(_composem(_keep("a", "b"), _obj()),
                                          _composem(_field("a", _pconst(_cemparray())),
                                                    _field("b", _pconst(_cbool(true))))),
                                "root"_var))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathComposeM []\n"
        "|   |   PathComposeM []\n"
        "|   |   |   PathField [b] PathConstant [] Const [true]\n"
        "|   |   PathField [a] PathConstant [] Const [[]]\n"
        "|   PathComposeM []\n"
        "|   |   PathObj []\n"
        "|   PathKeep [a, b]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrite simplifies the tree such that we directly construct the
    // object {a: [], b: true}
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Const [{}]\n"
        "|   PathComposeM []\n"
        "|   |   PathField [b] PathConstant [] Const [true]\n"
        "|   PathField [a] PathConstant [] Const [[]]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathConstantNothing) {
    // This test tests the rewriting of ABTs with a Nothing constant.
    // This ABT sets the field 'a' to Nothing and then checks if the field 'a.b' is equal to 3.
    ABT tree =
        NodeBuilder{}
            .root("x")
            .filter(_evalf(_get("a", _traverse1(_get("b", _traverse1(_cmp("Eq", "3"_cint64))))),
                           "x"_var))
            .eval("x", _evalp(_field("a", _pconst(_cnothing())), "root"_var))
            .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [x]\n"
        "|   PathGet [a] PathTraverse [1] PathGet [b] PathTraverse [1] PathCompare [Eq] Const [3]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [Nothing]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrites will remove the PathTraverse under PathGet [a] in the
    // Filter since we know that a is set to be a non-array by the projection.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [x]\n"
        "|   PathGet [a] PathGet [b] PathTraverse [1] PathCompare [Eq] Const [3]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [Nothing]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, FusionPathComposeMIdentity1) {
    // Observe the path law Id * p2 -> p2 via the PathFusion rewrites in action.
    auto tree = make<EvalPath>(
        make<PathComposeM>(make<PathIdentity>(),
                           make<PathField>("a", make<PathConstant>(Constant::int64(100)))),
        make<Variable>("root"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [root]\n"
        "PathComposeM []\n"
        "|   PathField [a] PathConstant [] Const [100]\n"
        "PathIdentity []\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [root]\n"
        "PathField [a] PathConstant [] Const [100]\n",
        tree);
}

TEST(Path, FusionPathComposeMIdentity2) {
    // Observe the path law p1 * Id -> p1 via the PathFusion rewrites in action.
    auto tree = make<EvalPath>(
        make<PathComposeM>(make<PathField>("a", make<PathConstant>(Constant::int64(100))),
                           make<PathIdentity>()),
        make<Variable>("root"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [root]\n"
        "PathComposeM []\n"
        "|   PathIdentity []\n"
        "PathField [a] PathConstant [] Const [100]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [root]\n"
        "PathField [a] PathConstant [] Const [100]\n",
        tree);
}


TEST(Path, PathDefaultNotNothing1) {
    // The following ABT returns the object {a: 3}. Notice the PathDefault is the first child of the
    // PathComposeM.
    ABT tree =
        NodeBuilder{}
            .root("x")
            .eval("x",
                  _evalp(_composem(_default(_cbool(false)), _field("a", _pconst("3"_cint64))),
                         "root"_var))
            .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathComposeM []\n"
        "|   |   PathField [a] PathConstant [] Const [3]\n"
        "|   PathDefault [] Const [false]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect to see that the PathComposeM and the PathDefault are removed by the PathFusion
    // rewrites since the constant overwrites whatever its input is.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [3]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathDefaultNotNothing2) {
    // The following ABT returns the object {a: 3}. Notice the PathDefault is the second child of
    // the PathComposeM.
    ABT tree =
        NodeBuilder{}
            .root("x")
            .eval("x",
                  _evalp(_composem(_field("a", _pconst("3"_cint64)), _default(_cbool(false))),
                         "root"_var))
            .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathComposeM []\n"
        "|   |   PathDefault [] Const [false]\n"
        "|   PathField [a] PathConstant [] Const [3]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect to see that the PathComposeM and the PathDefault are removed by the PathFusion
    // rewrites since we know that the first child of the PathComposeM will not produce Nothing, as
    // it produces a constant object.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [3]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, TwoPathKeep) {
    // The following ABT does an inclusion projection on the field 'a' and then on the fields 'a',
    // 'b', and 'c'.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .eval("x", _evalp(_composem(_keep("a"), _keep("a", "b", "c")), "root"_var))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathComposeM []\n"
        "|   |   PathKeep [a, b, c]\n"
        "|   PathKeep [a]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect to see that the unused PathKeeps for fields 'b' and 'c' are removed by the
    // PathFusion rewrites.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathKeep [a]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, FieldDefinedAndKeptInComposeM) {
    // The following ABT defines the field 'a' to be 2, then projects 'a' and defines the field 'b'
    // to be 3.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .eval("x",
                         _evalp(_composem(_keep("a"), _field("b", _pconst("3"_cint64))),
                                _evalp(_field("a", _pconst("2"_cint64)), "root"_var)))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   EvalPath []\n"
        "|   |   |   Variable [root]\n"
        "|   |   PathField [a] PathConstant [] Const [2]\n"
        "|   PathComposeM []\n"
        "|   |   PathField [b] PathConstant [] Const [3]\n"
        "|   PathKeep [a]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect to see that the PathFusion rewrites simplify the tree such that we project {a: 2,
    // b: 3} at once.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Const [{}]\n"
        "|   PathComposeM []\n"
        "|   |   PathField [b] PathConstant [] Const [3]\n"
        "|   PathField [a] PathConstant [] Const [2]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathDefaultSimple) {
    // The following ABT defines the field 'a' to be 2, then also projects 'b'.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .eval("x",
                         _evalp(_composem(_default(_cempobj()), _keep("b")),
                                _evalp(_field("a", _pconst("2"_cint64)), "root"_var)))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   EvalPath []\n"
        "|   |   |   Variable [root]\n"
        "|   |   PathField [a] PathConstant [] Const [2]\n"
        "|   PathComposeM []\n"
        "|   |   PathKeep [b]\n"
        "|   PathDefault [] Const [{}]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect to see that the PathFusion rewrites simplify the tree such that we compose the
    // projections.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathComposeM []\n"
        "|   |   PathComposeM []\n"
        "|   |   |   PathKeep [b]\n"
        "|   |   PathDefault [] Const [{}]\n"
        "|   PathField [a] PathConstant [] Const [2]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathDefaultObjectInput) {
    ABT tree = make<EvalPath>(make<PathComposeM>(make<PathDefault>(Constant::emptyObject()),
                                                 make<PathDefault>(Constant::emptyObject())),
                              Constant::emptyObject());

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Const [{}]\n"
        "PathComposeM []\n"
        "|   PathDefault [] Const [{}]\n"
        "PathDefault [] Const [{}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);

    while (PathFusion{env}.optimize(tree))
        ;

    // We expect to see that the PathFusion rewrites remove the unnecessary PathDefaults.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Const [{}]\n"
        "PathIdentity []\n",
        tree);
}

TEST(Path, PathCompareEqMemberArrayLower) {
    // This ABT sets the field 'a' to "hello" and then checks if the value of the field 'a' is a
    // member of the array [1, 2, 3].
    ABT tree =
        NodeBuilder{}
            .root("x")
            .filter(_evalf(
                _get("a",
                     _traverse1(_cmp("EqMember", _carray("1"_cdouble, "2"_cdouble, "3"_cdouble)))),
                "x"_var))
            .eval("x", _evalp(_field("a", _pconst("hello"_cstr)), "root"_var))
            .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [x]\n"
        "|   PathGet [a] PathTraverse [1] PathCompare [EqMember] Const [[1, 2, 3]]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    while (PathFusion{env}.optimize(tree) || ConstEval{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrites removes the PathTraverse and transforms the
    // PathCompare [EqMember] to an expression checking if the value under the PathCompare is an
    // array. If so, we rewrite it as BinaryOp [EqMember] and if not, we rewrite it as BinaryOp [Eq]
    // + BinaryOp [Cmp3w]. ConstEval will simplify the prior expression to just have the BinaryOp
    // [EqMember] branch since the value of the PathCompare is a constant array.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   BinaryOp [EqMember]\n"
        "|   |   Const [[1, 2, 3]]\n"
        "|   Const [\"hello\"]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathCompareEqMemberNonArrayLower) {
    // This ABT sets the field 'a' to "hello" and then checks if the value of the field 'a' "is a
    // member" of the string "world". Note, this is a weird example, but it is one that tests the
    // rewrites in the situation where the value below PathCompare [EqMember] is not an array, but
    // some other constant.
    ABT tree = NodeBuilder{}
                   .root("x")
                   .filter(_evalf(_get("a", _traverse1(_cmp("EqMember", "world"_cstr))), "x"_var))
                   .eval("x", _evalp(_field("a", _pconst("hello"_cstr)), "root"_var))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [x]\n"
        "|   PathGet [a] PathTraverse [1] PathCompare [EqMember] Const [\"world\"]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    while (PathFusion{env}.optimize(tree) || ConstEval{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrites removes the PathTraverse and transforms the
    // PathCompare [EqMember] to an expression checking if the value under the PathCompare is an
    // array. If so, we rewrite it as BinaryOp [EqMember] and if not, we rewrite it as BinaryOp [Eq]
    // + BinaryOp [Cmp3w]. ConstEval will simplify the prior expression to just have the BinaryOp
    // [Eq] + BinaryOp [Cmp3w] branch since the value of the PathCompare is NOT a constant array.
    // ConstEval will further simplify the tree since both "hello" and "world" are constants, so the
    // check of their equality will always be false.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   Const [false]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, PathCompareEqMemberUnknownTypeLower) {
    // This ABT sets the field 'a' to "hello" and then checks if the value of the field 'a' is a
    // member of the value stored by the variable "a".
    ABT tree = NodeBuilder{}
                   .root("x")
                   .filter(_evalf(_get("a", _traverse1(_cmp("EqMember", "a"_var))), "x"_var))
                   .eval("x", _evalp(_field("a", _pconst("hello"_cstr)), "root"_var))
                   .finish(_scan("root", "test"));

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   EvalFilter []\n"
        "|   |   Variable [x]\n"
        "|   PathGet [a] PathTraverse [1] PathCompare [EqMember] Variable [a]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);

    auto env = VariableEnvironment::build(tree);
    while (PathFusion{env}.optimize(tree))
        ;

    // We expect that the PathFusion rewrites removes the PathTraverse and transforms the
    // PathCompare [EqMember] to an expression checking if the value under the PathCompare is an
    // array. If so, we rewrite it as BinaryOp [EqMember] and if not, we rewrite it as BinaryOp [Eq]
    // + BinaryOp [Cmp3w]. There are no further simplifications for ConstEval to do here since we do
    // not know the type of the value stored by the variable "a".
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{x}]\n"
        "Filter []\n"
        "|   If []\n"
        "|   |   |   BinaryOp [Eq]\n"
        "|   |   |   |   Const [0]\n"
        "|   |   |   BinaryOp [Cmp3w]\n"
        "|   |   |   |   Variable [a]\n"
        "|   |   |   Const [\"hello\"]\n"
        "|   |   BinaryOp [EqMember]\n"
        "|   |   |   Variable [a]\n"
        "|   |   Const [\"hello\"]\n"
        "|   BinaryOp [Or]\n"
        "|   |   FunctionCall [isInListData] Variable [a]\n"
        "|   FunctionCall [isArray] Variable [a]\n"
        "Evaluation [{x}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [\"hello\"]\n"
        "Scan [test, {root}]\n",
        tree);
}

TEST(Path, FuseConstantAndCompare) {
    auto tree = NodeBuilder{}
                    .root("p1")
                    .filter(_evalf(_get("a", _cmp("Lt", "2"_cint64)), "p1"_var))
                    .eval("p1", _evalp(_field("a", _pconst("3"_cint64)), "root"_var))
                    .finish(_scan("root", "test"));

    auto env = VariableEnvironment::build(tree);
    while (PathFusion{env}.optimize(tree) || ConstEval{env}.optimize(tree))
        ;

    ASSERT_EXPLAIN_V2Compact_AUTO(
        "Root [{p1}]\n"
        "Filter []\n"
        "|   Const [false]\n"
        "Evaluation [{p1}]\n"
        "|   EvalPath []\n"
        "|   |   Variable [root]\n"
        "|   PathField [a] PathConstant [] Const [3]\n"
        "Scan [test, {root}]\n",
        tree);
}

}  // namespace
}  // namespace mongo::optimizer
