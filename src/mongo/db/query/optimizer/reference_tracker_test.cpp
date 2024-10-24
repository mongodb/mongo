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

#include "mongo/db/query/optimizer/reference_tracker.h"

#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>

#include "mongo/base/string_data.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"


namespace mongo::optimizer {
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

    // The let does not pass up its local definitions to ancestor nodes.
    ProjectionNameSet expectedProjSet = {};
    ASSERT(expectedProjSet == env.topLevelProjections());

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
}  // namespace
}  // namespace mongo::optimizer
