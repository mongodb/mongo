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

#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/sbe_stage_builder_type_checker.h"
#include "mongo/unittest/unittest.h"

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

    ASSERT(!TypeChecker::kNothingType.isSubset(sign));
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

}  // namespace
}  // namespace mongo::stage_builder
