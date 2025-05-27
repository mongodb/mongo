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

#include "mongo/db/query/stage_builder/sbe/abt/reference_tracker.h"

#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/unittest/unittest.h"

#include <utility>

#include <absl/container/node_hash_map.h>


namespace mongo::abt {
namespace {

TEST(ReferenceTrackerTest, GetDefinitionsForLet) {
    // ABT is a let expression.
    auto letOp =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(100)));
    auto letRef = letOp.ref();

    // The let resolves references to its local variable.
    auto env = VariableEnvironment::build(letOp);
    ASSERT(!env.hasFreeVariables());

    // But, the environment keeps the info about the definitions for all variables in the ABT. Check
    // that the local variable is defined by the Let.
    const Variable* xVar = nullptr;
    VariableEnvironment::walkVariables(letOp, [&](const Variable& var) {
        if (var.name() == "x") {
            xVar = &var;
        }
    });
    ASSERT(env.isLastRef(*xVar));
    ASSERT(env.getDefinition(*xVar).definedBy == letRef);
}

TEST(ReferenceTrackerTest, BuildLetWithFreeVariable) {
    // Again, ABT is a let expression. This time, the let expression also refers to a projection not
    // defined in the ABT.
    auto letOp =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("free")));

    // The "free" variable referenced by the let correctly cannot be resolved.
    auto env = VariableEnvironment::build(letOp);
    ASSERT(env.hasFreeVariables());
}

TEST(ReferenceTrackerTest, NoFreeVariables) {
    ABT noFreeVar = make<UnaryOp>(Operations::Neg, Constant::int64(1));

    auto env = VariableEnvironment::build(noFreeVar);
    ASSERT(!env.hasFreeVariables());
}

TEST(ReferenceTrackerTest, FreeVariablesNoMatchingDef) {
    // There are free variables when referencing a variable not defined previously.
    ABT freeVar = make<UnaryOp>(Operations::Neg, make<Variable>("free"));
    auto env = VariableEnvironment::build(freeVar);
    // "free" must still be a free variable.
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("free"), 1);
}

TEST(ReferenceTrackerTest, MultiFreeVariablesRecorded) {
    // There are free variables when referencing multiple variables that are not defined previously,
    // and both free variables are recorded.
    ABT freeVarMultipleTimes = make<BinaryOp>(
        Operations::Add,
        make<Variable>("free1"),
        make<BinaryOp>(Operations::Add, make<Variable>("free1"), make<Variable>("free2")));
    auto env = VariableEnvironment::build(freeVarMultipleTimes);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("free1"), 2);
    ASSERT_EQ(env.freeOccurences("free2"), 1);
}

TEST(ReferenceTrackerTest, FreeVariablesReferenceLetMaskedVar) {
    // There are free variables when referencing a variable masked by a Let.
    auto letNode =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Sub, make<Variable>("x"), Constant::int64(10)));
    auto sumNode = make<BinaryOp>(Operations::Add, std::move(letNode), make<Variable>("x"));
    auto env = VariableEnvironment::build(sumNode);
    ASSERT(env.hasFreeVariables());
    ASSERT_EQ(env.freeOccurences("x"), 1);
}

TEST(ReferenceTrackerTest, FreeVariablesMultiResolvedAtOnce) {
    // There are no free variables when multiple variables can be resolved by a single definition.
    auto letNode =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("x")));
    auto env = VariableEnvironment::build(letNode);
    ASSERT(!env.hasFreeVariables());
}

TEST(ReferenceTrackerTest, SingleVarNotLastRef) {
    // There are no last refs in an ABT that doesn't "finalize" any last refs.
    ABT justVar = make<Variable>("var");
    auto justVarEnv = VariableEnvironment::build(justVar);
    ASSERT_FALSE(justVarEnv.isLastRef(*justVar.cast<Variable>()));
}

TEST(ReferenceTrackerTest, LambdaMarksVarLastRef) {
    // There is a last ref in a basic lambda expression.
    ABT basicLambdaVar = make<Variable>("x");
    auto basicLambdaVarRef = basicLambdaVar.ref();
    auto basicLambda = make<LambdaAbstraction>(
        "x", make<BinaryOp>(Operations::Add, std::move(basicLambdaVar), Constant::int64(10)));
    auto basicLambdaEnv = VariableEnvironment::build(basicLambda);
    ASSERT(basicLambdaEnv.isLastRef(*basicLambdaVarRef.cast<Variable>()));
}

TEST(ReferenceTrackerTest, InMarksOnlyLocalVarLastRef) {
    // An 'in' result with multiple variables only marks the local variable as a last ref.
    ABT localVar = make<Variable>("localVar");
    auto localVarRef = localVar.ref();
    ABT otherVar = make<Variable>("otherProj");
    auto otherVarRef = otherVar.ref();
    ABT innerLet =
        make<Let>("localVar",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, std::move(localVar), std::move(otherVar)));
    ABT outerLet = make<Let>(
        "otherProj",
        Constant::int64(200),
        make<BinaryOp>(Operations::Add, std::move(innerLet), make<Variable>("otherProj")));

    auto inEnv = VariableEnvironment::build(outerLet);
    ASSERT(!inEnv.hasFreeVariables());
    ASSERT(inEnv.isLastRef(*localVarRef.cast<Variable>()));
    ASSERT_FALSE(inEnv.isLastRef(*otherVarRef.cast<Variable>()));
}

TEST(ReferenceTrackerTest, MultiLetDefinition) {
    auto multiLetOp = ABT::make<MultiLet>(
        std::vector<std::pair<ProjectionName, ABT>>{{"x", Constant::int64(100)},
                                                    {"y", Constant::int64(200)}},
        make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("y")));
    auto multiLetRef = multiLetOp.ref();

    // The multiLet resolves references to its local variable.
    auto env = VariableEnvironment::build(multiLetOp);
    ASSERT_FALSE(env.hasFreeVariables());

    // The environment keeps the info about the definitions for all variables in the ABT.
    // Check that the local variables are defined by the MultiLet.
    const Variable* xVar = nullptr;
    const Variable* yVar = nullptr;
    VariableEnvironment::walkVariables(multiLetOp, [&](const Variable& var) {
        if (var.name() == "x") {
            xVar = &var;
        } else if (var.name() == "y") {
            yVar = &var;
        }
    });
    ASSERT(env.isLastRef(*xVar));
    ASSERT(env.isLastRef(*yVar));
    ASSERT(env.getDefinition(*xVar).definedBy == multiLetRef);
    ASSERT(env.getDefinition(*yVar).definedBy == multiLetRef);
    ASSERT(env.getDefinition(*xVar).definition == multiLetOp.cast<MultiLet>()->bind(0).ref());
    ASSERT(env.getDefinition(*yVar).definition == multiLetOp.cast<MultiLet>()->bind(1).ref());
}

TEST(ReferenceTrackerTest, MultiLetFreeVariables) {
    {
        // ABT is a MultiLet expression. This time, it also refers to a projection not
        // defined in the ABT.
        auto multiLetOp = ABT::make<MultiLet>(
            std::vector<std::pair<ProjectionName, ABT>>{{"x", Constant::int64(100)},
                                                        {"y", Constant::int64(200)}},
            make<BinaryOp>(
                Operations::Add,
                make<Variable>("x"),
                make<BinaryOp>(Operations::Add, make<Variable>("y"), make<Variable>("free"))));

        // The "free" variable referenced by the multiLet correctly cannot be resolved.
        auto env = VariableEnvironment::build(multiLetOp);
        ASSERT(env.hasFreeVariables());
    }
    {
        // There are free variables when referencing variables masked by MultiLet.
        auto multiLetNode = make<MultiLet>(
            std::vector<std::pair<ProjectionName, ABT>>{{"x", Constant::int64(100)},
                                                        {"y", Constant::int64(200)}},
            make<BinaryOp>(Operations::Sub, make<Variable>("x"), make<Variable>("y")));
        auto sumNode = make<BinaryOp>(
            Operations::Add,
            std::move(multiLetNode),
            make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("y")));
        auto env = VariableEnvironment::build(sumNode);
        ASSERT(env.hasFreeVariables());
        ASSERT_EQ(env.freeOccurences("x"), 1);
        ASSERT_EQ(env.freeOccurences("y"), 1);
    }
    {
        // There are no free variables when multiple variables can be resolved by single definition.
        auto multiLetNode = make<MultiLet>(
            std::vector<std::pair<ProjectionName, ABT>>{{"x", Constant::int64(100)},
                                                        {"y", Constant::int64(200)}},
            make<BinaryOp>(
                Operations::Sub,
                make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("x")),
                make<BinaryOp>(Operations::Add, make<Variable>("y"), make<Variable>("y"))));
        auto env = VariableEnvironment::build(multiLetNode);
        ASSERT_FALSE(env.hasFreeVariables());
    }
}

TEST(ReferenceTrackerTest, MultiLetLastRefs) {
    {
        //  Let [z = 1] in
        //      z + (MultiLet [x = 1, y = 1] in
        //          x /*lastRef*/ + y /*lastRef*/) + z)
        //

        auto xVar = make<Variable>("x");
        auto xVarRef = xVar.ref();

        auto yVar = make<Variable>("y");
        auto yVarRef = yVar.ref();

        auto zVar = make<Variable>("z");
        auto zVarRef = zVar.ref();

        auto zVar2 = make<Variable>("z");
        auto zVarRef2 = zVar2.ref();

        auto multiLetOp = ABT::make<MultiLet>(
            std::vector<std::pair<ProjectionName, ABT>>{{"x", Constant::int64(1)},
                                                        {"y", Constant::int64(1)}},
            make<BinaryOp>(Operations::Add,
                           std::move(xVar),
                           make<BinaryOp>(Operations::Add, std::move(yVar), std::move(zVar2))));

        ABT tree =
            make<Let>("z",
                      Constant::int64(1),
                      make<BinaryOp>(Operations::Add, std::move(multiLetOp), std::move(zVar)));

        auto env = VariableEnvironment::build(tree);
        ASSERT_FALSE(env.hasFreeVariables());
        ASSERT(env.isLastRef(*xVarRef.cast<Variable>()));
        ASSERT(env.isLastRef(*yVarRef.cast<Variable>()));
        ASSERT_FALSE(env.isLastRef(*zVarRef.cast<Variable>()));
        ASSERT_FALSE(env.isLastRef(*zVarRef2.cast<Variable>()));
    }
    {
        //
        //  Let [u = 1] in
        //      Let [v = 1] in
        //          Let [w = 1] in
        //              MultiLet [x = u /*lastRef*/ + v, y = v /*lastRef*/ + w] in
        //                  x /*lastRef*/ + y /*lastRef*/ + w /*lastRef*/
        //
        auto uVar = make<Variable>("u");
        auto uVarRef = uVar.ref();

        auto vVar = make<Variable>("v");
        auto vVarRef = vVar.ref();

        auto vVar2 = make<Variable>("v");
        auto vVarRef2 = vVar2.ref();

        auto wVar = make<Variable>("w");
        auto wVarRef = wVar.ref();

        auto wVar2 = make<Variable>("w");
        auto wVarRef2 = wVar2.ref();

        auto xVar = make<Variable>("x");
        auto xVarRef = xVar.ref();

        auto yVar = make<Variable>("y");
        auto yVarRef = yVar.ref();

        std::vector<ProjectionName> varNames = {"x", "y"};
        auto multiLetNode = make<MultiLet>(
            std::move(varNames),
            makeSeq(
                make<BinaryOp>(Operations::Add, std::move(uVar), std::move(vVar)),
                make<BinaryOp>(Operations::Add, std::move(vVar2), std::move(wVar)),
                make<BinaryOp>(Operations::Add,
                               std::move(wVar2),
                               make<BinaryOp>(Operations::Add, std::move(xVar), std::move(yVar)))));

        auto tree =
            make<Let>("u",
                      Constant::int64(1),
                      make<Let>("v",
                                Constant::int64(1),
                                make<Let>("w", Constant::int64(1), std::move(multiLetNode))));

        auto env = VariableEnvironment::build(tree);

        ASSERT_FALSE(env.hasFreeVariables());
        ASSERT(env.isLastRef(*uVarRef.cast<Variable>()));
        ASSERT_FALSE(env.isLastRef(*vVarRef.cast<Variable>()));
        ASSERT(env.isLastRef(*vVarRef2.cast<Variable>()));
        ASSERT_FALSE(env.isLastRef(*wVarRef.cast<Variable>()));
        ASSERT(env.isLastRef(*wVarRef2.cast<Variable>()));
        ASSERT(env.isLastRef(*xVarRef.cast<Variable>()));
        ASSERT(env.isLastRef(*yVarRef.cast<Variable>()));
    }
    {
        // [MultiLet x = 1, y = x + 2, z = x + y + 3] in
        //      x /*lastRef*/ + y /*lastRef*/ + z /*lastRef*/

        auto xVar = make<Variable>("x");
        auto xVarRef = xVar.ref();

        auto xVar2 = make<Variable>("x");
        auto xVarRef2 = xVar2.ref();

        auto xVar3 = make<Variable>("x");
        auto xVarRef3 = xVar3.ref();

        auto yVar = make<Variable>("y");
        auto yVarRef = yVar.ref();

        auto yVar2 = make<Variable>("y");
        auto yVarRef2 = yVar2.ref();

        auto zVar = make<Variable>("z");
        auto zVarRef = zVar.ref();

        std::vector<ProjectionName> varNames = {"x", "y", "z"};
        auto tree = make<MultiLet>(
            std::move(varNames),
            makeSeq(Constant::int64(1),
                    make<BinaryOp>(Operations::Add, std::move(xVar), Constant::int64(2)),
                    make<BinaryOp>(
                        Operations::Add,
                        std::move(xVar2),
                        make<BinaryOp>(Operations::Add, std::move(yVar), Constant::int64(3))),
                    make<BinaryOp>(
                        Operations::Add,
                        std::move(xVar3),
                        make<BinaryOp>(Operations::Add, std::move(yVar2), std::move(zVar)))));

        auto env = VariableEnvironment::build(tree);

        ASSERT_FALSE(env.hasFreeVariables());
        ASSERT_FALSE(env.isLastRef(*xVarRef.cast<Variable>()));
        ASSERT_FALSE(env.isLastRef(*xVarRef2.cast<Variable>()));
        ASSERT(env.isLastRef(*xVarRef3.cast<Variable>()));
        ASSERT_FALSE(env.isLastRef(*yVarRef.cast<Variable>()));
        ASSERT(env.isLastRef(*yVarRef2.cast<Variable>()));
        ASSERT(env.isLastRef(*zVarRef.cast<Variable>()));
    }
}
}  // namespace
}  // namespace mongo::abt
