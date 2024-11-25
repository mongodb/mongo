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

#include <string>

#include <absl/container/node_hash_map.h>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/optimizer/algebra/operator.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/query/stage_builder/sbe/type_checker.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::stage_builder {
namespace {

using namespace optimizer;

TEST(TypeCheckerTest, FoldFunctionCallTypeMatch) {
    // Run typeMatch on a numeric operation that is guaranteed to never be Nothing.
    auto tree1 = make<FunctionCall>(
        "typeMatch",
        makeSeq(make<BinaryOp>(Operations::Mult,
                               Constant::int32(9),
                               make<BinaryOp>(Operations::FillEmpty,
                                              make<Variable>("inputVar"),
                                              Constant::int32(0))),
                Constant::int32(getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDecimal))));

    TypeChecker{}.typeCheck(tree1);

    ASSERT(tree1.is<Constant>() && tree1.cast<Constant>()->isValueBool() &&
           tree1.cast<Constant>()->getValueBool());

    // Run typeMatch on a numeric operation that can be Nothing.
    auto tree2 = make<FunctionCall>(
        "typeMatch",
        makeSeq(make<BinaryOp>(Operations::Mult, Constant::int32(9), make<Variable>("inputVar")),
                Constant::int32(getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDecimal))));

    TypeChecker{}.typeCheck(tree2);

    ASSERT(tree2.is<FunctionCall>());

    // Run typeMatch on a numeric operation that can be Numeric or a Date.
    auto tree3 = make<FunctionCall>(
        "typeMatch",
        makeSeq(make<BinaryOp>(Operations::Add,
                               Constant::int32(9),
                               make<BinaryOp>(Operations::FillEmpty,
                                              make<Variable>("inputVar"),
                                              Constant::int32(0))),
                Constant::int32(getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDecimal))));

    TypeChecker{}.typeCheck(tree3);
    ASSERT(tree3.is<FunctionCall>());

    // Run typeMatch on a numeric operation that is guaranteed to never be a Date.
    auto tree4 = make<FunctionCall>(
        "typeMatch",
        makeSeq(make<BinaryOp>(
                    Operations::Add,
                    Constant::int32(9),
                    make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                             Constant::int32(-100),
                             Constant::int32(+100))),
                Constant::int32(getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDecimal))));

    TypeChecker{}.typeCheck(tree4);

    ASSERT(tree4.is<Constant>() && tree4.cast<Constant>()->isValueBool() &&
           tree4.cast<Constant>()->getValueBool());
}

TEST(TypeCheckerTest, FoldFunctionCallCoerceToBool) {
    // Run coerceToBool on a boolean operation.
    auto tree1 = make<FunctionCall>(
        "coerceToBool", makeSeq(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar")))));

    TypeChecker{}.typeCheck(tree1);

    ASSERT(tree1.is<FunctionCall>() && tree1.cast<FunctionCall>()->name() == "exists");
}

TEST(TypeCheckerTest, FoldFillEmpty) {
    // Run fillEmpty on a test expression that can never be Nothing.
    auto tree1 =
        make<BinaryOp>(Operations::FillEmpty,
                       make<If>(make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                                Constant::int64(9),
                                Constant::int64(18)),
                       Constant::int64(0));

    TypeChecker{}.typeCheck(tree1);

    ASSERT(tree1.is<If>());
}

TEST(TypeCheckerTest, FoldFillEmptyInComplexCheck) {
    // Run both exists and typeMatch as part of an And, and expect that the resulting expression is
    // guaranteeded to never be Nothing (hence, FillEmpty can be safely removed).
    auto tree = make<BinaryOp>(
        Operations::FillEmpty,
        make<BinaryOp>(Operations::And,
                       make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                       make<FunctionCall>("typeMatch",
                                          makeSeq(make<Variable>("inputVar"),
                                                  Constant::int32(getBSONTypeMask(
                                                      sbe::value::TypeTags::NumberInt32))))),
        Constant::str("impossible"));

    TypeChecker{}.typeCheck(tree);

    ASSERT(tree.is<BinaryOp>() && tree.cast<BinaryOp>()->op() == Operations::And);
}

TEST(TypeCheckerTest, TypeCheckIf) {
    auto tree = make<If>(
        make<BinaryOp>(Operations::And,
                       make<FunctionCall>("exists", makeSeq(make<Variable>("inputVar"))),
                       make<FunctionCall>("isNumber", makeSeq(make<Variable>("inputVar")))),
        make<BinaryOp>(Operations::Mult, Constant::int32(9), make<Variable>("inputVar")),
        Constant::int32(0));

    TypeSignature sign = TypeChecker{}.typeCheck(tree);

    ASSERT(!TypeSignature::kNothingType.isSubset(sign));
}

TEST(TypeCheckerTest, TypeCheckIsString) {
    // isString on an if() statement that would return a string in any case is always true.
    auto tree = make<FunctionCall>("isString",
                                   makeSeq(make<If>(make<BinaryOp>(Operations::FillEmpty,
                                                                   make<Variable>("inputVar"),
                                                                   Constant::boolean(true)),
                                                    Constant::str("true"),
                                                    Constant::str("false"))));
    TypeChecker{}.typeCheck(tree);

    ASSERT(tree.is<Constant>() && tree.cast<Constant>()->isValueBool() &&
           tree.cast<Constant>()->getValueBool());

    // Rewrite doesn't occur if one of the branches can throw an error.
    auto tree1 = make<FunctionCall>(
        "isString",
        makeSeq(make<If>(
            make<BinaryOp>(
                Operations::FillEmpty, make<Variable>("inputVar"), Constant::boolean(true)),
            Constant::str("true"),
            make<FunctionCall>(
                "fail", makeSeq(Constant::int32(7654301), Constant::str("unexpected value"))))));
    TypeChecker{}.typeCheck(tree1);

    ASSERT(tree1.is<FunctionCall>());

    // Rewrite doesn't occur if there is a chance that the if() returns Nothing because of the
    // test condition.
    auto tree2 = make<FunctionCall>(
        "isString",
        makeSeq(
            make<If>(make<Variable>("inputVar"), Constant::str("true"), Constant::str("false"))));
    TypeChecker{}.typeCheck(tree2);

    ASSERT(tree2.is<FunctionCall>());
}

TEST(TypeCheckerTest, FoldComparisonBetweenBools) {
    // A comparison between a boolean operation and a constant 'true' is just the boolean operation.
    auto tree1 =
        make<BinaryOp>(Operations::Eq,
                       make<FunctionCall>("coerceToBool", makeSeq(make<Variable>("inputVar"))),
                       Constant::boolean(true));

    TypeChecker{}.typeCheck(tree1);

    ASSERT(tree1.is<FunctionCall>() && tree1.cast<FunctionCall>()->name() == "coerceToBool");

    // A comparison between a boolean operation and a constant 'false' is the negation of the
    // boolean operation.
    auto tree2 =
        make<BinaryOp>(Operations::Eq,
                       make<FunctionCall>("coerceToBool", makeSeq(make<Variable>("inputVar"))),
                       Constant::boolean(false));

    TypeChecker{}.typeCheck(tree2);

    ASSERT(tree2.is<UnaryOp>() && tree2.cast<UnaryOp>()->op() == Operations::Not);

    // Same results if the order is reversed.
    auto tree3 =
        make<BinaryOp>(Operations::Eq,
                       Constant::boolean(true),
                       make<FunctionCall>("coerceToBool", makeSeq(make<Variable>("inputVar"))));

    TypeChecker{}.typeCheck(tree3);

    ASSERT(tree3.is<FunctionCall>() && tree3.cast<FunctionCall>()->name() == "coerceToBool");

    auto tree4 =
        make<BinaryOp>(Operations::Eq,
                       Constant::boolean(false),
                       make<FunctionCall>("coerceToBool", makeSeq(make<Variable>("inputVar"))));

    TypeChecker{}.typeCheck(tree4);

    ASSERT(tree4.is<UnaryOp>() && tree4.cast<UnaryOp>()->op() == Operations::Not);
}

TEST(TypeCheckerTest, FoldFillEmptyVariable) {
    // Run fillEmpty on a test expression based on a slot variable.
    auto tree1 = make<BinaryOp>(Operations::FillEmpty,
                                make<If>(make<BinaryOp>(Operations::Eq,
                                                        make<Variable>(getABTVariableName(1)),
                                                        Constant::int32(34)),
                                         Constant::int64(9),
                                         Constant::int64(18)),
                                Constant::null());
    TypeSignature signature = TypeChecker{}.typeCheck(tree1);
    ASSERT_EQ(
        signature.typesMask,
        getTypeSignature(sbe::value::TypeTags::NumberInt64, sbe::value::TypeTags::Null).typesMask);
    // Inject the information that the slot contains a number that cannot be Nothing.
    TypeChecker checker;
    checker.bind(getABTVariableName(1), TypeSignature::kNumericType);
    signature = checker.typeCheck(tree1);
    ASSERT_EQ(signature.typesMask, getTypeSignature(sbe::value::TypeTags::NumberInt64).typesMask);
}

TEST(TypeCheckerTest, FoldTraverseF) {
    // Run traverseF on a slot variable.
    auto tree1 =
        make<FunctionCall>("traverseF",
                           makeSeq(make<Variable>(getABTVariableName(1)),
                                   make<LambdaAbstraction>(
                                       getABTLocalVariableName(2, 0),
                                       make<BinaryOp>(Operations::Gt,
                                                      make<Variable>(getABTLocalVariableName(2, 0)),
                                                      Constant::int32(8))),
                                   Constant::boolean(false)));
    TypeSignature signature = TypeChecker{}.typeCheck(tree1);
    ASSERT_EQ(signature.typesMask, TypeSignature::kAnyScalarType.typesMask);
    // Inject the information that the slot contains a number (and not an array).
    TypeChecker checker;
    checker.bind(getABTVariableName(1), TypeSignature::kNumericType);
    signature = checker.typeCheck(tree1);

    // The result should be a Let expression having the comparison in its body.
    ASSERT(tree1.is<Let>() && tree1.cast<Let>()->in().is<BinaryOp>());
    ASSERT_EQ(signature.typesMask, getTypeSignature(sbe::value::TypeTags::Boolean).typesMask);

    // Run it on a constant array.
    auto tree2 =
        make<FunctionCall>("traverseF",
                           makeSeq(Constant::array(sbe::value::makeIntOrLong(78)),
                                   make<LambdaAbstraction>(
                                       getABTLocalVariableName(2, 0),
                                       make<BinaryOp>(Operations::Gt,
                                                      make<Variable>(getABTLocalVariableName(2, 0)),
                                                      Constant::int32(8))),
                                   Constant::boolean(false)));
    signature = TypeChecker{}.typeCheck(tree2);

    ASSERT_EQ(signature.typesMask, TypeSignature::kAnyScalarType.typesMask);

    // Run it on a constant number.
    auto tree3 =
        make<FunctionCall>("traverseF",
                           makeSeq(Constant::int32(78),
                                   make<LambdaAbstraction>(
                                       getABTLocalVariableName(2, 0),
                                       make<BinaryOp>(Operations::Gt,
                                                      make<Variable>(getABTLocalVariableName(2, 0)),
                                                      Constant::int32(8))),
                                   Constant::boolean(false)));
    signature = TypeChecker{}.typeCheck(tree3);

    // The result should be a Let expression having the comparison in its body.
    ASSERT(tree3.is<Let>() && tree1.cast<Let>()->in().is<BinaryOp>());
    ASSERT_EQ(signature.typesMask, getTypeSignature(sbe::value::TypeTags::Boolean).typesMask);
}

TEST(TypeCheckerTest, FoldTraverseP) {
    // Run traverseP on a slot variable.
    auto tree1 =
        make<FunctionCall>("traverseP",
                           makeSeq(make<Variable>(getABTVariableName(1)),
                                   make<LambdaAbstraction>(
                                       getABTLocalVariableName(2, 0),
                                       make<BinaryOp>(Operations::Mult,
                                                      make<Variable>(getABTLocalVariableName(2, 0)),
                                                      Constant::int32(90))),
                                   Constant::int32(0)));
    TypeSignature signature = TypeChecker{}.typeCheck(tree1);
    ASSERT_EQ(signature.typesMask, TypeSignature::kAnyScalarType.typesMask);
    // Inject the information that the slot contains a number (and not an array).
    TypeChecker checker;
    checker.bind(getABTVariableName(1), TypeSignature::kNumericType);
    signature = checker.typeCheck(tree1);

    // The result should be a Let expression having the multiplication in its body.
    ASSERT(tree1.is<Let>() && tree1.cast<Let>()->in().is<BinaryOp>());
    ASSERT_EQ(signature.typesMask, TypeSignature::kNumericType.typesMask);

    // Run it on a constant array.
    auto tree2 =
        make<FunctionCall>("traverseP",
                           makeSeq(Constant::array(sbe::value::makeIntOrLong(78)),
                                   make<LambdaAbstraction>(
                                       getABTLocalVariableName(2, 0),
                                       make<BinaryOp>(Operations::Mult,
                                                      make<Variable>(getABTLocalVariableName(2, 0)),
                                                      Constant::int32(90))),
                                   Constant::int32(0)));
    signature = TypeChecker{}.typeCheck(tree2);

    ASSERT_EQ(signature.typesMask, TypeSignature::kAnyScalarType.typesMask);

    // Run it on a constant number.
    auto tree3 =
        make<FunctionCall>("traverseP",
                           makeSeq(Constant::int32(78),
                                   make<LambdaAbstraction>(
                                       getABTLocalVariableName(2, 0),
                                       make<BinaryOp>(Operations::Mult,
                                                      make<Variable>(getABTLocalVariableName(2, 0)),
                                                      Constant::int32(90))),
                                   Constant::int32(0)));
    signature = TypeChecker{}.typeCheck(tree3);

    // The result should be a Let expression having the multiplication in its body.
    ASSERT(tree3.is<Let>() && tree1.cast<Let>()->in().is<BinaryOp>());
    ASSERT_EQ(signature.typesMask, TypeSignature::kNumericType.typesMask);
}

}  // namespace
}  // namespace mongo::stage_builder
