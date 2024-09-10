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

#include "mongo/db/matcher/in_list_data.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::optimizer {
namespace {

using namespace unit_test_abt_literals;
using namespace sbe::value;

TEST(ConstEvalTest, RIDUnion) {
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
        ProjectionNameVector{"z"},
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

TEST(ConstEvalTest, FoldIsInListForConstants) {
    ABT abt = make<FunctionCall>("isInList", makeSeq(Constant::int32(1)));
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _fn("isInList", getParam(TypeTags::Array))._n;
    ConstEval::constFold(abt);
    // Can't simplify this expression because child of FunctionCall[isInList] is not a Constant
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "FunctionCall [isInList]\n"
        "FunctionCall [getParam]\n"
        "|   Const [15]\n"
        "Const [0]\n",
        abt);

    sbe::InList* inList = nullptr;
    abt = make<FunctionCall>(
        "isInList",
        makeSeq(make<Constant>(TypeTags::inList, sbe::value::bitcastFrom<sbe::InList*>(inList))));
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);
}

TEST(ConstEvalTest, GetParamMinKey) {
    ABT abt = _binary("Gt", _cminKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _binary("Gte", _cminKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _binary("Lt", _cminKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Lte", _cminKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Gt", getParam(TypeTags::NumberInt32), _cminKey())._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Gte", getParam(TypeTags::NumberInt32), _cminKey())._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Lt", getParam(TypeTags::NumberInt32), _cminKey())._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _binary("Lt", getParam(TypeTags::NumberInt32), _cminKey())._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _binary("Cmp3w", _cminKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [-1]\n",
        abt);
}

TEST(ConstEvalTest, GetParamMaxKey) {
    ABT abt = _binary("Lt", _cmaxKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _binary("Gt", _cmaxKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Cmp3w", _cmaxKey(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [1]\n",
        abt);
}

TEST(ConstEvalTest, GetParamSameType) {
    ABT abt = _binary("Lt", "5"_cint64, getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    // Can't simplify this expression since getParam might evaluate to any number.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "BinaryOp [Lt]\n"
        "|   FunctionCall [getParam]\n"
        "|   |   Const [1]\n"
        "|   Const [0]\n"
        "Const [5]\n",
        abt);
}

TEST(ConstEvalTest, GetParamDiffType) {
    ABT abt = _binary("Lt", "5"_cint64, getParam(TypeTags::ObjectId))._n;
    ConstEval::constFold(abt);
    // The number 5 is always less than an ObjectId
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);
}

TEST(ConstEvalTest, GetParamDifferentNumberTypes) {
    ABT abt = _binary("Lt", "5"_cint64, getParam(TypeTags::NumberDouble))._n;
    ConstEval::constFold(abt);
    // Can't simplify this expression since getParam(double) is the same canonicalized BSON type as
    // the integer constant.
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "BinaryOp [Lt]\n"
        "|   FunctionCall [getParam]\n"
        "|   |   Const [3]\n"
        "|   Const [0]\n"
        "Const [5]\n",
        abt);
}

TEST(ConstEvalTest, GetParamTwoParams) {
    ABT abt = _binary("Lt", getParam(TypeTags::NumberInt32), getParam(TypeTags::ObjectId))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);
}

TEST(ConstEvalTest, GetParamNaN) {
    ABT abt = _binary("Eq", _cNaN(), getParam(TypeTags::NumberInt32))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _binary("Lt", _cNaN(), getParam(TypeTags::StringSmall))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Lte", _cNaN(), getParam(TypeTags::NumberDouble))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Gt", _cNaN(), getParam(TypeTags::ObjectId))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    abt = _binary("Gte", _cNaN(), getParam(TypeTags::MinKey))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    abt = _binary("Cmp3w", getParam(TypeTags::Boolean), _cNaN())._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [1]\n",
        abt);

    abt = _binary("Cmp3w", getParam(TypeTags::NumberDecimal), _cNaN())._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [1]\n",
        abt);
}

TEST(ConstEvalTest, AndOrFoldNonNothingLhs) {
    /* OR */
    // non-nullable lhs (getParam) || true -> true.
    ABT abt = _binary("Or", getParam(TypeTags::Boolean), _cbool(true))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    // non-nullable lhs (binary op) || true -> true.
    abt = _binary("Or",
                  _binary("Gt", getParam(TypeTags::NumberInt32), getParam(TypeTags::NumberInt32)),
                  _cbool(true))
              ._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    // non-nullable lhs (const) || true -> true.
    abt = _binary("Or", _cNaN(), _cbool(true))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [true]\n",
        abt);

    // non-nullable lhs (getParam) || false -> lhs.
    abt = _binary("Or", getParam(TypeTags::Boolean), _cbool(false))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "FunctionCall [getParam]\n"
        "|   Const [6]\n"
        "Const [0]\n",
        abt);

    // nullable lhs (variable) || false -> lhs.
    abt = _binary("Or", _binary("Lte", "x"_var, "2"_cint64), _cbool(false))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "BinaryOp [Lte]\n"
        "|   Const [2]\n"
        "Variable [x]\n",
        abt);

    // nullable lhs (variable) || true -> no change.
    abt = _binary("Or", _binary("Lte", "x"_var, "2"_cint64), _cbool(true))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "BinaryOp [Or]\n"
        "|   Const [true]\n"
        "BinaryOp [Lte]\n"
        "|   Const [2]\n"
        "Variable [x]\n",
        abt);

    // nothing lhs (const) || true -> no change.
    abt = _binary("Or", _cnothing(), _cbool(true))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "BinaryOp [Or]\n"
        "|   Const [true]\n"
        "Const [Nothing]\n",
        abt);

    /* AND */
    // non-nullable lhs (binary op) && false -> false.
    abt = _binary("And", _binary("Gt", getParam(TypeTags::NumberInt32), "2"_cint64), _cbool(false))
              ._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    // non-nullable lhs (const) && false -> false.
    abt = _binary("And", _cnull(), _cbool(false))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "Const [false]\n",
        abt);

    // non-nullable lhs (getParam) && true -> lhs.
    abt = _binary("And", getParam(TypeTags::Boolean), _cbool(true))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "FunctionCall [getParam]\n"
        "|   Const [6]\n"
        "Const [0]\n",
        abt);

    // non-nullable lhs (binary op) && true -> lhs.
    abt = _binary("And", _binary("Lt", getParam(TypeTags::NumberDecimal), "2"_cint64), _cbool(true))
              ._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "BinaryOp [Lt]\n"
        "|   Const [2]\n"
        "FunctionCall [getParam]\n"
        "|   Const [13]\n"
        "Const [0]\n",
        abt);

    // nullable lhs (if) && true -> lhs.
    abt = _binary("And", _if("x"_var, "y"_var, "z"_var), _cbool(true))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "If []\n"
        "|   |   Variable [z]\n"
        "|   Variable [y]\n"
        "Variable [x]\n",
        abt);

    // nullable lhs (if) && false -> no change.
    abt = _binary("And", _if("x"_var, "y"_var, "z"_var), _cbool(false))._n;
    ConstEval::constFold(abt);
    ASSERT_EXPLAIN_V2_AUTO(  // NOLINT
        "BinaryOp [And]\n"
        "|   Const [false]\n"
        "If []\n"
        "|   |   Variable [z]\n"
        "|   Variable [y]\n"
        "Variable [x]\n",
        abt);
}
}  // namespace
}  // namespace mongo::optimizer
