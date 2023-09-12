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

#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer {
namespace {
TEST(ConstEvalTest, RIDUnion) {
    using namespace properties;

    ABT leftChild = make<EvaluationNode>(
        "y",
        make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(1)),
        make<EvaluationNode>("x", Constant::int64(1), make<ScanNode>("p0", "test")));

    ABT rightChild = make<EvaluationNode>(
        "y",
        make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(10)),
        make<EvaluationNode>("x", Constant::int64(100), make<ScanNode>("p0", "test")));

    ABT unionNode = make<RIDUnionNode>(
        "p0", ProjectionNameVector{"p0", "x", "y"}, std::move(leftChild), std::move(rightChild));

    ABT rootNode = make<RootNode>(
        ProjectionRequirement{ProjectionNameVector{"z"}},
        make<EvaluationNode>(
            "z",
            make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("y")),
            std::move(unionNode)));

    auto env = VariableEnvironment::build(rootNode);
    ASSERT(!env.hasFreeVariables());

    // Constant folds the ABT.
    ConstEval::constFold(rootNode);

    // Expects the constant folding not to fold below the RIDUnion node in that the left child
    // and the right child may hold different definitions. Expected the reference tracker to use the
    // definitions created from make<Source>() instead of the ones from the left child or the right
    // child.
    ASSERT_EXPLAIN_AUTO(
        "Root [{z}]\n"
        "  Evaluation [{z}]\n"
        "    BinaryOp [Add]\n"
        "      Variable [x]\n"
        "      Variable [y]\n"
        "    RIDUnion [p0]\n"
        "      Evaluation [{y} = Const [2]]\n"
        "        Evaluation [{x} = Const [1]]\n"
        "          Scan [test, {p0}]\n"
        "      Evaluation [{y} = Const [110]]\n"
        "        Evaluation [{x} = Const [100]]\n"
        "          Scan [test, {p0}]\n",
        rootNode);
}

TEST(ConstEvalTest, FoldRedundantExists) {
    ABT exists = make<FunctionCall>("exists", makeSeq(Constant::int32(1)));

    // Eliminates the exists call in favor of a boolean true.
    ConstEval::constFold(exists);

    ASSERT_EXPLAIN_AUTO(  // NOLINT
        "Const [true]\n",
        exists);
}
}  // namespace
}  // namespace mongo::optimizer
