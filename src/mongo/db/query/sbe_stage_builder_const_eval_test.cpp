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

#include <cstdint>

#include <absl/container/node_hash_map.h>

#include "mongo/base/string_data.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/sbe_stage_builder_const_eval.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::stage_builder {
namespace {

using namespace optimizer;
using namespace unit_test_abt_literals;

Constant* constEval(ABT& tree, const CollatorInterface* collator = nullptr) {
    ExpressionConstEval evaluator{collator};
    evaluator.optimize(tree);

    // The result must be Constant.
    Constant* result = tree.cast<Constant>();
    ASSERT(result != nullptr);

    ASSERT_NE(ABT::tagOf<Constant>(), ABT::tagOf<BinaryOp>());
    ASSERT_EQ(tree.tagOf(), ABT::tagOf<Constant>());
    return result;
}

TEST(SbeStageBuilderConstEvalTest, ConstEval) {
    // 1 + 2
    ABT tree = _binary("Add", "1"_cint64, "2"_cint64)._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueInt64(), 3);
}


TEST(SbeStageBuilderConstEvalTest, ConstEvalCompose) {
    // (1 + 2) + 3
    ABT tree = _binary("Add", _binary("Add", "1"_cint64, "2"_cint64), "3"_cint64)._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueInt64(), 6);
}


TEST(SbeStageBuilderConstEvalTest, ConstEvalCompose2) {
    // 3 - (5 - 4)
    auto tree = _binary("Sub", "3"_cint64, _binary("Sub", "5"_cint64, "4"_cint64))._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueInt64(), 2);
}

TEST(SbeStageBuilderConstEvalTest, ConstEval3) {
    // 1.5 + 0.5
    auto tree = _binary("Add", "1.5"_cdouble, "0.5"_cdouble)._n;
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueDouble(), 2.0);
}

TEST(SbeStageBuilderConstEvalTest, ConstEval4) {
    // INT32_MAX (as int) + 0 (as double) => INT32_MAX (as double)
    auto tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MAX), Constant::fromDouble(0));
    Constant* result = constEval(tree);
    ASSERT_EQ(result->getValueDouble(), INT32_MAX);
}

TEST(SbeStageBuilderConstEvalTest, ConstEval5) {
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

TEST(SbeStageBuilderConstEvalTest, ConstEval6) {
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

TEST(SbeStageBuilderConstEvalTest, IntegerOverflow) {
    auto int32tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MAX), Constant::int32(1));
    ASSERT_EQ(constEval(int32tree)->getValueInt64(), 2147483648);
}

TEST(SbeStageBuilderConstEvalTest, IntegerUnderflow) {
    auto int32tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MIN), Constant::int32(-1));
    ASSERT_EQ(constEval(int32tree)->getValueInt64(), -2147483649);

    auto tree =
        make<BinaryOp>(Operations::Add, Constant::int32(INT32_MAX), Constant::int64(INT64_MIN));
    ASSERT_EQ(constEval(tree)->getValueInt64(), -9223372034707292161);
}

TEST(SbeStageBuilderConstEvalTest, ConstVariableInlining) {
    ABT tree = make<Let>("x",
                         Constant::int32(4),
                         make<BinaryOp>(Operations::Add, make<Variable>("x"), make<Variable>("x")));
    ASSERT_EQ(constEval(tree)->getValueInt32(), 8);
}

TEST(SbeStageBuilderConstEvalTest, IfSimplification) {
    ABT trueTree = make<If>(Constant::boolean(true), Constant::int32(1), Constant::int32(2));
    ABT falseTree = make<If>(Constant::boolean(false), Constant::int32(1), Constant::int32(2));
    ASSERT_EQ(constEval(trueTree)->getValueInt32(), 1);
    ASSERT_EQ(constEval(falseTree)->getValueInt32(), 2);
}

TEST(SbeStageBuilderConstEvalTest, EqualSimplification) {
    ABT falseTree = _binary("Eq", "A"_cstr, "B"_cstr)._n;
    ASSERT_FALSE(constEval(falseTree)->getValueBool());
    ABT trueTree = _binary("Eq", "A"_cstr, "A"_cstr)._n;
    ASSERT_TRUE(constEval(trueTree)->getValueBool());
}

TEST(SbeStageBuilderConstEvalTest, EqualSimplificationCollation) {
    const CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    ABT falseTree = _binary("Eq", "A"_cstr, "b"_cstr)._n;
    ASSERT_FALSE(constEval(falseTree, &collator)->getValueBool());
    ABT trueTree = _binary("Eq", "A"_cstr, "a"_cstr)._n;
    ASSERT_TRUE(constEval(trueTree, &collator)->getValueBool());
}

TEST(SbeStageBuilderConstEvalTest, CompareSimplification) {
    ABT tree = _binary("Lt", "A"_cstr, "B"_cstr)._n;
    ASSERT_TRUE(constEval(tree)->getValueBool());
}

TEST(SbeStageBuilderConstEvalTest, CompareSimplificationCollation) {
    const CollatorInterfaceMock collator{CollatorInterfaceMock::MockType::kToLowerString};
    ABT tree = _binary("Lt", "a"_cstr, "B"_cstr)._n;
    ASSERT_TRUE(constEval(tree, &collator)->getValueBool());
}

}  // namespace
}  // namespace mongo::stage_builder
