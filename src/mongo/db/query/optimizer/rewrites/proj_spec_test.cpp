/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/rewrites/proj_spec_lower.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer {
namespace {

using sbe::MakeObjSpec;
using sbe::value::TypeTags;

using FieldAction = MakeObjSpec::FieldAction;
using FieldActions = std::vector<std::pair<StringData, FieldAction>>;
using MakeObj = MakeObjSpec::MakeObj;
using ValueArg = MakeObjSpec::ValueArg;
using LambdaArg = MakeObjSpec::LambdaArg;
using NonObjInputBehavior = MakeObjSpec::NonObjInputBehavior;

// Helpers to shorten path construction.
ABT Obj() {
    return make<PathObj>();
}

ABT Id() {
    return make<PathIdentity>();
}

ABT Arr() {
    return make<PathArr>();
}

ABT Default(ABT inner) {
    return make<PathDefault>(std::move(inner));
}

ABT Const(ABT c) {
    return make<PathConstant>(std::move(c));
}

ABT Traverse(size_t depth, ABT inner) {
    return make<PathTraverse>(depth, std::move(inner));
}

ABT Field(FieldNameType path, ABT inner) {
    return make<PathField>(std::move(path), std::move(inner));
}

ABT Get(FieldNameType path, ABT inner) {
    return make<PathGet>(std::move(path), std::move(inner));
}

ABT Keep(FieldNameOrderedSet paths) {
    return make<PathKeep>(std::move(paths));
}

ABT Drop(FieldNameOrderedSet paths) {
    return make<PathDrop>(std::move(paths));
}

ABT Var(ProjectionName name) {
    return make<Variable>(std::move(name));
}

ABT Lambda(ProjectionName varName, ABT inner) {
    return make<PathLambda>(make<LambdaAbstraction>(std::move(varName), std::move(inner)));
}

ABT operator*(ABT l, ABT r) {
    return make<PathComposeM>(std::move(l), std::move(r));
}

ABT Eval(ABT path, ABT input) {
    return make<EvalPath>(std::move(path), std::move(input));
}

// Macro helper to detect case when we completely fallback to lowering.
#define ASSERT_MAKEOBJSPEC_ARGS_EMPTY(tree) ASSERT_EQ(generateMakeObjArgs(tree), ABTVector{});

// Helper to run one pass of path lowering, which should be enough to generate a MakeObjSpec in the
// test cases below.
bool runPathLower(ABT& tree, bool constFold = false) {
    auto env = VariableEnvironment::build(tree);
    auto prefixId = PrefixId::createForTests();
    if (!PathLowering{prefixId}.optimize(tree)) {
        return false;
    }
    while (constFold && ConstEval{env}.optimize(tree))
        ;
    return true;
}

TEST(ProjSpecTest, SimplePathField) {
    auto tree = Eval(Field("a", Const(Constant::fromDouble(42.0))), Constant::emptyObject());
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Const [{}]\n"
        "PathField [a] PathConstant [] Const [42]\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   |   Const [42]\n"
        "|   |   Const [false]\n"
        "|   Const [{}]\n"
        "Const [MakeObjSpec([a = Arg(0)], Open, NewObj, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimpleNestedPathFieldTraverse) {
    auto tree = Eval(
        Field("a",
              Traverse(PathTraverse::kSingleLevel, Field("b", Const(Constant::fromDouble(42.0))))),
        Constant::emptyObject());
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Const [{}]\n"
        "PathField [a] PathTraverse [1] PathField [b] PathConstant [] Const [42]\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   |   Const [42]\n"
        "|   |   Const [false]\n"
        "|   Const [{}]\n"
        "Const [MakeObjSpec([a = MakeObj([b = Arg(0)], Open, NewObj, 1)], Open, NewObj, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimplePathFieldConsecutiveTraverse) {
    // Traverses are treated additively.
    {
        auto tree =
            Eval(Field("a",
                       Traverse(PathTraverse::kSingleLevel,
                                Traverse(PathTraverse::kSingleLevel,
                                         Traverse(PathTraverse::kSingleLevel,
                                                  Field("b", Const(Constant::fromDouble(42.0))))))),
                 Constant::emptyObject());
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Const [{}]\n"
            "PathField [a] PathTraverse [1] PathTraverse [1] PathTraverse [1] PathField [b] "
            "PathConstant [] Const [42]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [42]\n"
            "|   |   Const [false]\n"
            "|   Const [{}]\n"
            "Const [MakeObjSpec([a = MakeObj([b = Arg(0)], Open, NewObj, 3)], Open, NewObj, 0)]\n",
            tree);
    }
    // Infinite traverses override other depths.
    {
        auto tree =
            Eval(Field("a",
                       Traverse(PathTraverse::kSingleLevel,
                                Traverse(PathTraverse::kSingleLevel,
                                         Traverse(PathTraverse::kUnlimited,
                                                  Field("b", Const(Constant::fromDouble(42.0))))))),
                 Constant::emptyObject());
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Const [{}]\n"
            "PathField [a] PathTraverse [1] PathTraverse [1] PathTraverse [inf] PathField [b] "
            "PathConstant [] Const [42]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [42]\n"
            "|   |   Const [false]\n"
            "|   Const [{}]\n"
            "Const [MakeObjSpec([a = MakeObj([b = Arg(0)], Open)], Open, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree =
            Eval(Field("a",
                       Traverse(PathTraverse::kSingleLevel,
                                Traverse(PathTraverse::kUnlimited,
                                         Traverse(PathTraverse::kSingleLevel,
                                                  Field("b", Const(Constant::fromDouble(42.0))))))),
                 Constant::emptyObject());
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Const [{}]\n"
            "PathField [a] PathTraverse [1] PathTraverse [inf] PathTraverse [1] PathField [b] "
            "PathConstant [] Const [42]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [42]\n"
            "|   |   Const [false]\n"
            "|   Const [{}]\n"
            "Const [MakeObjSpec([a = MakeObj([b = Arg(0)], Open)], Open, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree =
            Eval(Field("a",
                       Traverse(PathTraverse::kUnlimited,
                                Traverse(PathTraverse::kSingleLevel,
                                         Traverse(PathTraverse::kSingleLevel,
                                                  Field("b", Const(Constant::fromDouble(42.0))))))),
                 Constant::emptyObject());
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Const [{}]\n"
            "PathField [a] PathTraverse [inf] PathTraverse [1] PathTraverse [1] PathField [b] "
            "PathConstant [] Const [42]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [42]\n"
            "|   |   Const [false]\n"
            "|   Const [{}]\n"
            "Const [MakeObjSpec([a = MakeObj([b = Arg(0)], Open)], Open, NewObj, 0)]\n",
            tree);
    }
}

TEST(ProjSpecTest, PathFieldTraverse) {
    auto tree =
        Eval(Field("c",
                   Traverse(PathTraverse::kUnlimited,
                            Field("b",
                                  Field("a",
                                        Traverse(PathTraverse::kSingleLevel,
                                                 Field("d", Const(Constant::fromDouble(42.0)))))))),
             Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathField [c] PathTraverse [inf] PathField [b] PathField [a] PathTraverse [1] PathField "
        "[d] PathConstant [] Const [42]\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   |   Const [42]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([c = MakeObj([b = MakeObj([a = MakeObj([d = Arg(0)], Open, NewObj, "
        "1)], Open, NewObj, 0)], Open)], Open, NewObj, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimplePathKeep) {
    auto tree = Eval(Keep({"a", "b", "c"}), Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathKeep [a, b, c]\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([a, b, c], Closed, RetInput, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimplePathDrop) {
    auto tree = Eval(Drop({"a", "b", "c"}), Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathDrop [a, b, c]\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([a, b, c], Open, RetInput, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimplePathIdentity) {
    // Note: we need a PathField here, otherwise we will avoid generating the 'makeBsonObj' function
    // call in this case, since it can be effectively treated as a no-op.
    auto tree = Eval(Field("a", Id()), Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathField [a] PathIdentity []\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([a = MakeObj([], Open, RetInput, 0)], Open, RetInput, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimplePathObj) {
    auto tree = Eval(Obj(), Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathObj []\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([], Open, RetNothing, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimpleFailedToConstructSpec) {
    // These are cases resulting in an "incomplete" MakeObjSpec, therefore returning 0 args/ falling
    // back to path lowering. Here we just want to verify that we fail to generate arguments for
    // makeBsonObj.
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(Eval(Const(Constant::emptyObject()), Var("foo")));
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval(Traverse(PathTraverse::kUnlimited, Const(Constant::emptyObject())), Var("foo")));
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(Eval(Arr(), Var("foo")));
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(Eval(Get("f", Id()), Var("foo")));
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(Eval(Default(Constant::emptyObject()), Var("foo")));

    // This case results in a trivial 'makeObjSpec', so we'd rather fallback to
    // lowering/const-folding it away.
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(Eval(Id(), Var("foo")));

    // Different traverse depths for two sides of a composition fails to translate.
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval(Id() * Traverse(PathTraverse::kSingleLevel, Id()), Var("foo")));
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval(Traverse(PathTraverse::kUnlimited, Id()) * Id(), Var("foo")));
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval(Traverse(PathTraverse::kSingleLevel, Id()) * Traverse(PathTraverse::kUnlimited, Id()),
             Var("foo")));
}

TEST(ProjSpecTest, ComposeConstObj) {
    // The below path means:
    //  1. (Left branch) if input is an object, set field "a" to "abc" unconditionally (and create a
    //  new object if needed).
    //  2. (Right branch) if input is an object, return it, else return {}.
    auto tree = Eval(Field("a", Const(Constant::str("abc"_sd))) *
                         (Obj() * Default(Constant::emptyObject())),
                     Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathComposeM []\n"
        "|   PathComposeM []\n"
        "|   |   PathDefault [] Const [{}]\n"
        "|   PathObj []\n"
        "PathField [a] PathConstant [] Const [\"abc\"]\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   |   Const [\"abc\"]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([a = Arg(0)], Open, NewObj, 0)]\n",
        tree);
}

TEST(ProjSpecTest, SimpleComposition) {
    {
        auto tree = Eval(Field("a", Const(Constant::emptyArray())) *
                             Field("b", Const(Constant::emptyObject())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [b] PathConstant [] Const [{}]\n"
            "PathField [a] PathConstant [] Const [[]]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   |   Const [{}]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([a = Arg(0), b = Arg(1)], Open, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(Field("a", Field("b", Id())) * Field("c", Field("b", Id())), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [c] PathField [b] PathIdentity []\n"
            "PathField [a] PathField [b] PathIdentity []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([a = MakeObj([b = MakeObj([], Open, RetInput, 0)], Open, RetInput, "
            "0), c = MakeObj([b = MakeObj([], Open, RetInput, 0)], Open, RetInput, 0)], Open, "
            "RetInput, 0)]\n",
            tree);
    }
}

TEST(ProjSpecTest, TraversalComposition) {
    // We fallback to lowering when two branches have differing traversal depths.
    // A possible optimization here would be to recognize that after traversing to
    // an unlimited depth, we can't possibly have an array value at the end, hence simplifying this
    // path.
    auto tree = Eval(Field("a", Traverse(PathTraverse::kUnlimited, Id()) * Arr()), Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathField [a] PathComposeM []\n"
        "|   PathArr []\n"
        "PathTraverse [inf] PathIdentity []\n",
        tree);
    ASSERT_TRUE(runPathLower(tree, true /* constFold */));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   |   LambdaAbstraction [inputComposeM_0] If []\n"
        "|   |   |   |   |   Const [Nothing]\n"
        "|   |   |   |   Variable [inputComposeM_0]\n"
        "|   |   |   FunctionCall [isArray] Variable [inputComposeM_0]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([a = LambdaArg(0, false)], Open, NewObj, 0)]\n",
        tree);
}

// Tests that a ValueArg on the right overrides the left branch by ConstCompose.
// Note: all the below tests should set field "same" to [] (result in the same lowered ABT).
// Furthermore, the left branch must result in behavior kNewObj because the right const branch does,
// otherwise we will fallback.
TEST(ProjSpecTest, ConstCompose) {
    // ValueArg.
    {
        auto tree = Eval(Field("same", Const(Constant::emptyObject())) *
                             Field("same", Const(Constant::emptyArray())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathConstant [] Const [[]]\n"
            "PathField [same] PathConstant [] Const [{}]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Open, NewObj, 0)]\n",
            tree);
    }

    // LambdaArg.
    {
        auto tree =
            Eval(Field("same", Arr()) * Field("same", Const(Constant::emptyArray())), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathConstant [] Const [[]]\n"
            "PathField [same] PathArr []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Open, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(Field("same", Default(Constant::null())) *
                             Field("same", Const(Constant::emptyArray())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathConstant [] Const [[]]\n"
            "PathField [same] PathDefault [] Const [null]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Open, NewObj, 0)]\n",
            tree);
    }

    // MakeObj.
    {
        auto tree = Eval(Field("same", Field("inner", Const(Constant::emptyObject()))) *
                             Field("same", Const(Constant::emptyArray())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathConstant [] Const [[]]\n"
            "PathField [same] PathField [inner] PathConstant [] Const [{}]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Open, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(((Obj() * Default(Constant::emptyObject())) * Field("same", Id())) *
                             Field("same", Const(Constant::emptyArray())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathConstant [] Const [[]]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathIdentity []\n"
            "PathComposeM []\n"
            "|   PathDefault [] Const [{}]\n"
            "PathObj []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Open, NewObj, 0)]\n",
            tree);
    }

    // KeepOrDrop.
    {
        auto tree = Eval(((Obj() * Default(Constant::emptyObject())) * Keep({"same"})) *
                             Field("same", Const(Constant::emptyArray())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathConstant [] Const [[]]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathComposeM []\n"
            "|   PathDefault [] Const [{}]\n"
            "PathObj []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Closed, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(((Obj() * Default(Constant::emptyObject())) * Drop({"same"})) *
                             Field("same", Const(Constant::emptyArray())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathConstant [] Const [[]]\n"
            "PathComposeM []\n"
            "|   PathDrop [same]\n"
            "PathComposeM []\n"
            "|   PathDefault [] Const [{}]\n"
            "PathObj []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [[]]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Open, NewObj, 0)]\n",
            tree);
    }
}

TEST(ProjSpecTest, RightLambdaMerge) {
    // Test that a lambda value arg on the right side results in a fallback.
    {
        ABTVector branches = {/* ValueArg. */
                              Field("same", Const(Constant::emptyObject())),
                              /* LambdaArg. */
                              Field("same", Arr()),
                              Field("same", Default(Constant::null())),
                              /* MakeObjSpec. */
                              Field("same", Field("inner", Id())),
                              Field("same", Id()),
                              /* KeepDrop. */
                              Drop({"same"})};
        for (auto&& leftBranch : branches) {
            ASSERT_MAKEOBJSPEC_ARGS_EMPTY(Eval(leftBranch * Field("same", Arr()), Var("foo")));
        }
    }

    // UNLESS we have a Keep! Then we just propagate the lambda. Note that Lmabda sets the behavior
    // to kNewObj, so we need to make sure the Keep branch also generates an object, or this will
    // fallback.
    {
        auto tree = Eval(((Obj() * Default(Constant::emptyObject())) * Keep({"same"})) *
                             Field("same", Lambda("id", Var("id"))),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathLambda [] LambdaAbstraction [id] Variable [id]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathComposeM []\n"
            "|   PathDefault [] Const [{}]\n"
            "PathObj []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree, true /* constFold */));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   LambdaAbstraction [id] Variable [id]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = LambdaArg(0, false)], Closed, NewObj, 0)]\n",
            tree);
    }
}

TEST(ProjSpecTest, RightMakeObjMerge) {
    // Test that a MakeObj value arg on the right side results in a fallback if we have a ValueArg
    // or LambdaArg on the left.
    {
        ABTVector branches = {/* ValueArg. */
                              Field("same", Const(Constant::emptyObject())),
                              /* LambdaArg. */
                              Field("same", Arr()),
                              Field("same", Default(Constant::null()))};
        for (auto&& leftBranch : branches) {
            ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
                Eval(leftBranch * Field("same", Field("inner", Id())), Var("foo")));
        }
    }

    // We recursively combine compositions of MakeObj.
    {
        auto tree = Eval(Field("same", Field("inner", Id())) * Field("same", Field("inner", Id())),
                         Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathField [inner] PathIdentity []\n"
            "PathField [same] PathField [inner] PathIdentity []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = MakeObj([inner = MakeObj([], Open, RetInput, 0)], Open, "
            "RetInput, 0)], Open, RetInput, 0)]\n",
            tree);
    }
    {
        auto tree =
            Eval(Field("same", Field("inner", Id())) * Field("same", Keep({"inner", "other"})),
                 Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathKeep [inner, other]\n"
            "PathField [same] PathField [inner] PathIdentity []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = MakeObj([inner = MakeObj([], Open, RetInput, 0), other], "
            "Closed, RetInput, 0)], Open, RetInput, 0)]\n",
            tree);
    }

    // If we have a Keep, we return the right MakeObj + set the top-level field behavior to closed.
    {
        auto tree = Eval(Keep({"same"}) * Field("same", Field("inner", Id())), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathField [same] PathField [inner] PathIdentity []\n"
            "PathKeep [same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = MakeObj([inner = MakeObj([], Open, RetInput, 0)], Open, "
            "RetInput, 0)], Closed, RetInput, 0)]\n",
            tree);
    }

    // If we have a Drop, we fallback.
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval(Drop({"same"}) * Field("same", Field("inner", Id())), Var("foo")));
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval(Drop({"same"}) * (Keep({"same"}) * Field("same", Field("inner", Id()))), Var("foo")));
    // Recursive absorption case.
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(Eval(
        Field("obj", Traverse(PathTraverse::kUnlimited, Drop({"obj"}))) *
            ((Obj() * Keep({"_id", "obj"})) *
             Field("obj",
                   Traverse(PathTraverse::kUnlimited,
                            ((Obj() * Keep({"obj"})) *
                             Field("obj",
                                   Traverse(PathTraverse::kUnlimited, (Obj() * Keep({"obj"})))))))),
        Var("foo")));
}

TEST(ProjSpecTest, RightKeepMerge) {
    // In the following cases we pick the left FieldAction for "a" and set the field behavior to
    // closed:
    // - ValueArg: Field "a" Const c * Keep "a" = Field "a" Const c
    // - LambdaArg: Field "a" Lambda l * Keep "a" = Field "a" Lambda l
    // - MakeObj: Field "a" Field "b" p * Keep "a" = Field "a" Field "b" p

    // ValueArg.
    {
        auto tree =
            Eval(Field("same", Const(Constant::emptyObject())) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathField [same] PathConstant [] Const [{}]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   Const [{}]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = Arg(0)], Closed, NewObj, 0)]\n",
            tree);
    }

    // LambdaArg.
    {
        auto tree = Eval(Field("same", Arr()) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathField [same] PathArr []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree, true /* constFold */));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   LambdaAbstraction [valArr_0] If []\n"
            "|   |   |   |   |   Const [Nothing]\n"
            "|   |   |   |   Variable [valArr_0]\n"
            "|   |   |   FunctionCall [isArray] Variable [valArr_0]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = LambdaArg(0, false)], Closed, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(Field("same", Default(Constant::null())) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathField [same] PathDefault [] Const [null]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree, true /* constFold */));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   |   LambdaAbstraction [valDefault_0] If []\n"
            "|   |   |   |   |   Const [null]\n"
            "|   |   |   |   Variable [valDefault_0]\n"
            "|   |   |   FunctionCall [exists] Variable [valDefault_0]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = LambdaArg(0, false)], Closed, NewObj, 0)]\n",
            tree);
    }

    // MakeObj.
    {
        auto tree = Eval(Field("same", Field("inner", Id())) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathField [same] PathField [inner] PathIdentity []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = MakeObj([inner = MakeObj([], Open, RetInput, 0)], Open, "
            "RetInput, 0)], Closed, RetInput, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(Field("same", Id()) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathField [same] PathIdentity []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same = MakeObj([], Open, RetInput, 0)], Closed, RetInput, 0)]\n",
            tree);
    }

    // When we have Keeps/Drops in some combination, things get more complicated.

    // (KeepMerge) Keep S * Keep S = Keep S.
    {
        auto tree = Eval(Keep({"same"}) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathKeep [same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Closed, RetInput, 0)]\n",
            tree);
    }
    {
        auto tree =
            Eval(Keep({"same1", "same2", "same3"}) * Keep({"same1", "same2", "same3"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same1, same2, same3]\n"
            "PathKeep [same1, same2, same3]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same1, same2, same3], Closed, RetInput, 0)]\n",
            tree);
    }

    // (KeepMerge) Keep S1 * Keep S2 = Keep S1 ∩ S2
    {
        auto tree = Eval(Keep({"same"}) * Keep({"other", "same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [other, same]\n"
            "PathKeep [same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Closed, RetInput, 0)]\n",
            tree);
    }
    {
        auto tree =
            Eval(Keep({"same1", "same2", "same3"}) * Keep({"other", "same2", "same3"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [other, same2, same3]\n"
            "PathKeep [same1, same2, same3]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same2, same3], Closed, RetInput, 0)]\n",
            tree);
    }
    {
        // Empty intersection means we drop all fields.
        auto tree = Eval(Keep({"a", "b", "c"}) * Keep({"d", "e", "f"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [d, e, f]\n"
            "PathKeep [a, b, c]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([], Closed, RetInput, 0)]\n",
            tree);
    }

    // (DropKeepMerge) Drop S * Keep S = drop all fields, including S.
    {
        auto tree = Eval(Drop({"same"}) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathDrop [same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([], Closed, RetInput, 0)]\n",
            tree);
    }
    {
        auto tree =
            Eval(Drop({"same1", "same2", "same3"}) * Keep({"same1", "same2", "same3"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same1, same2, same3]\n"
            "PathDrop [same1, same2, same3]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([], Closed, RetInput, 0)]\n",
            tree);
    }

    // (DropKeepMerge) Drop S1 * Keep S2 = Keep S2 \ S1
    {
        // Eliminate common field, keep other field.
        auto tree = Eval(Drop({"same"}) * Keep({"other", "same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [other, same]\n"
            "PathDrop [same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([other], Closed, RetInput, 0)]\n",
            tree);
    }
    {
        // Drop all fields.
        auto tree = Eval(Drop({"other", "same"}) * Keep({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathKeep [same]\n"
            "PathDrop [other, same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([], Closed, RetInput, 0)]\n",
            tree);
    }
}

TEST(ProjSpecTest, RightDropMerge) {
    // In the following cases we pick the right FieldAction for "a", as they always evaluate to
    // Drop "a"
    // - ValueArg: Field "a" Const c * Drop "a"
    // - LambdaArg: Field "a" Lambda l * Drop "a"
    // - MakeObj: Field "a" Field "b" p * Drop "a"
    // - Drop: Drop "a" * Drop "a"

    // ValueArg.
    {
        auto tree =
            Eval(Field("same", Const(Constant::emptyObject())) * Drop({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [same]\n"
            "PathField [same] PathConstant [] Const [{}]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Open, NewObj, 0)]\n",
            tree);
    }

    // LambdaArg.
    {
        auto tree = Eval(Field("same", Arr()) * Drop({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [same]\n"
            "PathField [same] PathArr []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Open, NewObj, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(Field("same", Default(Constant::null())) * Drop({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [same]\n"
            "PathField [same] PathDefault [] Const [null]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Open, NewObj, 0)]\n",
            tree);
    }

    // MakeObj.
    {
        auto tree = Eval(Field("same", Field("inner", Id())) * Drop({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [same]\n"
            "PathField [same] PathField [inner] PathIdentity []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Open, RetInput, 0)]\n",
            tree);
    }
    {
        auto tree = Eval(Field("same", Id()) * Drop({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [same]\n"
            "PathField [same] PathIdentity []\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Open, RetInput, 0)]\n",
            tree);
    }
    // (DropMerge) Drop S * Drop S = Drop S.
    {
        auto tree = Eval(Drop({"same"}) * Drop({"same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [same]\n"
            "PathDrop [same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same], Open, RetInput, 0)]\n",
            tree);
    }
    {
        auto tree =
            Eval(Drop({"same1", "same2", "same3"}) * Drop({"same1", "same2", "same3"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [same1, same2, same3]\n"
            "PathDrop [same1, same2, same3]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same1, same2, same3], Open, RetInput, 0)]\n",
            tree);
    }
    // (DropMerge) Drop S1 * Drop S2 = Drop S1 U S2.
    {
        auto tree = Eval(Drop({"a", "b", "c"}) * Drop({"d", "e"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [d, e]\n"
            "PathDrop [a, b, c]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([a, b, c, d, e], Open, RetInput, 0)]\n",
            tree);
    }

    // (KeepDropMerge) Keep S1 * Drop S2 = Keep S1 \ S2
    {
        // Drop everything.
        auto tree = Eval(Keep({"same"}) * Drop({"other", "same"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [other, same]\n"
            "PathKeep [same]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([], Closed, RetInput, 0)]\n",
            tree);
    }
    {
        // Keep only "same1".
        auto tree =
            Eval(Keep({"same1", "same2", "same3"}) * Drop({"other", "same2", "same3"}), Var("foo"));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "EvalPath []\n"
            "|   Variable [foo]\n"
            "PathComposeM []\n"
            "|   PathDrop [other, same2, same3]\n"
            "PathKeep [same1, same2, same3]\n",
            tree);
        ASSERT_TRUE(runPathLower(tree));
        ASSERT_EXPLAIN_V2Compact_AUTO(
            "FunctionCall [makeBsonObj]\n"
            "|   |   Const [false]\n"
            "|   Variable [foo]\n"
            "Const [MakeObjSpec([same1], Closed, RetInput, 0)]\n",
            tree);
    }
}

TEST(ProjSpecTest, ConflictingKeepDropComposition) {
    auto tree = Eval(
        // Drop the inner "conflicting" path.
        Field("outer", Drop({"conflicting"})) *
            // Keep some top-level paths.
            ((Obj() * Keep({"_id", "outer"})) *
             // Keep the inner "conflicting" path.
             Field("outer", (Obj() * Keep({"conflicting"})))),
        Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathComposeM []\n"
        "|   PathComposeM []\n"
        "|   |   PathField [outer] PathComposeM []\n"
        "|   |   |   PathKeep [conflicting]\n"
        "|   |   PathObj []\n"
        "|   PathComposeM []\n"
        "|   |   PathKeep [_id, outer]\n"
        "|   PathObj []\n"
        "PathField [outer] PathDrop [conflicting]\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    // Drop the conflicting path.
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([outer = MakeObj([], Closed, RetNothing, 0), _id], Closed, "
        "RetNothing, 0)]\n",
        tree);
}

TEST(ProjSpecTest, NewObj) {
    // Tests that we detect the Path subtree equivalent to kNewObj and translate it.
    auto tree = Eval(Obj() * Default(Constant::emptyObject()), Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathComposeM []\n"
        "|   PathDefault [] Const [{}]\n"
        "PathObj []\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([], Open, NewObj, 0)]\n",
        tree);
}

TEST(ProjSpecTest, NewObjLeft) {
    auto tree = Eval((Obj() * Default(Constant::emptyObject())) *
                         Field("a", Const(Constant::str("abc"_sd))),
                     Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathComposeM []\n"
        "|   PathField [a] PathConstant [] Const [\"abc\"]\n"
        "PathComposeM []\n"
        "|   PathDefault [] Const [{}]\n"
        "PathObj []\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   |   Const [\"abc\"]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([a = Arg(0)], Open, NewObj, 0)]\n",
        tree);
}

TEST(ProjSpecTest, LClosedROpen) {
    // The below will fail to convert because we have kRetInput on the left and kNewObj on the
    // right.
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval(Keep({}) * Field("a", Const(Constant::fromDouble(42.0))), Var("foo")));

    // However, this tree will convert.
    auto tree = Eval(((Obj() * Default(Constant::emptyObject())) * Keep({})) *
                         Field("a", Const(Constant::fromDouble(42.0))),
                     Var("foo"));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "EvalPath []\n"
        "|   Variable [foo]\n"
        "PathComposeM []\n"
        "|   PathField [a] PathConstant [] Const [42]\n"
        "PathComposeM []\n"
        "|   PathKeep []\n"
        "PathComposeM []\n"
        "|   PathDefault [] Const [{}]\n"
        "PathObj []\n",
        tree);
    ASSERT_TRUE(runPathLower(tree));
    ASSERT_EXPLAIN_V2Compact_AUTO(
        "FunctionCall [makeBsonObj]\n"
        "|   |   |   Const [42]\n"
        "|   |   Const [false]\n"
        "|   Variable [foo]\n"
        "Const [MakeObjSpec([a = Arg(0)], Closed, NewObj, 0)]\n",
        tree);
}

TEST(ProjSpecTest, LRClosed) {
    // Reduced form of {$project: {o1.a: 1}}, {$project: {o1.o2.b: 1}}.
    // We expect to see a fallback here, because when we go to merge the specs for fields "a" and
    // "o2", we have a MakeObj on the right side, which we do not know if we should keep when both
    // specs are Closed.
    ASSERT_MAKEOBJSPEC_ARGS_EMPTY(
        Eval((Keep({"o1"}) * Field("o1", Obj() * Keep({"a"})) *
              ((Obj() * Keep({"o1"})) *
               Field("o1", (Obj() * Keep({"o2"})) * Field("o2", Obj() * Keep({"b"}))))),
             Var("foo")));
}

}  // namespace
}  // namespace mongo::optimizer
