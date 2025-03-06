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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/stage_builder/sbe/abt_lower.h"
#include "mongo/db/query/stage_builder/sbe/abt_lower_defs.h"

namespace mongo::stage_builder::abt {
namespace {

using namespace optimizer;

class AbtToSbeExpression : public sbe::EExpressionTestFixture {
public:
    // Helper that lowers and compiles an ABT expression and returns the evaluated result.
    // If the expression contains a variable, it will be bound to a slot along with its definition
    // before lowering.
    std::pair<sbe::value::TypeTags, sbe::value::Value> evalExpr(
        const ABT& tree,
        boost::optional<
            std::pair<ProjectionName, std::pair<sbe::value::TypeTags, sbe::value::Value>>> var) {
        auto env = VariableEnvironment::build(tree);

        SlotVarMap map;
        sbe::value::OwnedValueAccessor accessor;
        auto slotId = bindAccessor(&accessor);
        if (var) {
            auto& projName = var.get().first;
            map[projName] = slotId;

            auto [tag, val] = var.get().second;
            accessor.reset(tag, val);
        }

        sbe::InputParamToSlotMap inputParamToSlotMap;
        auto expr =
            SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
                .optimize(tree);

        auto compiledExpr = compileExpression(*expr);
        return runCompiledExpression(compiledExpr.get());
    }

    void assertEqualValues(std::pair<sbe::value::TypeTags, sbe::value::Value> res,
                           std::pair<sbe::value::TypeTags, sbe::value::Value> resConstFold) {
        auto [tag, val] = sbe::value::compareValue(
            res.first, res.second, resConstFold.first, resConstFold.second);
        ASSERT_EQ(tag, sbe::value::TypeTags::NumberInt32);
        ASSERT_EQ(val, 0);
    }
};

TEST_F(AbtToSbeExpression, Lower1) {
    auto tree = Constant::int64(100);
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());

    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
    ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 100);
}

TEST_F(AbtToSbeExpression, Lower2) {
    auto tree =
        make<Let>("x",
                  Constant::int64(100),
                  make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(100)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());

    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
    ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 200);
}

TEST_F(AbtToSbeExpression, Lower3) {
    auto tree = make<FunctionCall>("isNumber", makeSeq(Constant::int64(10)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto result = runCompiledExpressionPredicate(compiledExpr.get());

    ASSERT(result);
}

TEST_F(AbtToSbeExpression, Lower4) {
    auto [tagArr, valArr] = sbe::value::makeNewArray();
    auto arr = sbe::value::getArrayView(valArr);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 1);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 2);
    auto [tagArrNest, valArrNest] = sbe::value::makeNewArray();
    auto arrNest = sbe::value::getArrayView(valArrNest);
    arrNest->push_back(sbe::value::TypeTags::NumberInt64, 21);
    arrNest->push_back(sbe::value::TypeTags::NumberInt64, 22);
    arr->push_back(tagArrNest, valArrNest);
    arr->push_back(sbe::value::TypeTags::NumberInt64, 3);

    auto tree = make<FunctionCall>(
        "traverseP",
        makeSeq(make<Constant>(tagArr, valArr),
                make<LambdaAbstraction>(
                    "x", make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(10))),
                Constant::nothing()));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    sbe::value::ValueGuard guard(resultTag, resultVal);

    ASSERT_EQ(sbe::value::TypeTags::Array, resultTag);
}

TEST_F(AbtToSbeExpression, Lower5) {
    auto tree = make<FunctionCall>(
        "setField", makeSeq(Constant::nothing(), Constant::str("fieldA"), Constant::int64(10)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
    sbe::value::ValueGuard guard(resultTag, resultVal);
}

TEST_F(AbtToSbeExpression, LowerFunctionCallFail) {
    std::string errorMessage = "Error: Bad value 123456789!";

    auto tree =
        make<FunctionCall>("fail",
                           makeSeq(Constant::int32(static_cast<int32_t>(ErrorCodes::BadValue)),
                                   Constant::str(errorMessage)));
    auto env = VariableEnvironment::build(tree);
    SlotVarMap map;
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);
    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);
    Status status = Status::OK();

    try {
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        sbe::value::releaseValue(resultTag, resultVal);
    } catch (const DBException& e) {
        status = e.toStatus();
    }

    ASSERT(!status.isOK());
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_EQ(status.reason(), errorMessage);
}

TEST_F(AbtToSbeExpression, LowerFunctionCallConvert) {
    sbe::value::OwnedValueAccessor inputAccessor;
    auto slotId = bindAccessor(&inputAccessor);
    SlotVarMap map;
    map["inputVar"] = slotId;

    auto tree = make<FunctionCall>(
        "convert",
        makeSeq(make<Variable>("inputVar"),
                Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt64))));
    auto env = VariableEnvironment::build(tree);
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);
    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);

    {
        inputAccessor.reset(sbe::value::TypeTags::NumberDouble,
                            sbe::value::bitcastFrom<double>(42.0));
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        sbe::value::ValueGuard guard(resultTag, resultVal);
        ASSERT_EQ(resultTag, sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 42);
    }

    {
        auto [tag, val] = sbe::value::makeCopyDecimal(Decimal128{-73});
        inputAccessor.reset(tag, val);
        auto [resultTag, resultVal] = runCompiledExpression(compiledExpr.get());
        sbe::value::ValueGuard guard(resultTag, resultVal);
        ASSERT_EQ(resultTag, sbe::value::TypeTags::NumberInt64);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), -73);
    }
}

TEST_F(AbtToSbeExpression, LowerFunctionCallTypeMatch) {
    sbe::value::OwnedValueAccessor inputAccessor;
    auto slotId = bindAccessor(&inputAccessor);
    SlotVarMap map;
    map["inputVar"] = slotId;

    auto tree = make<FunctionCall>(
        "typeMatch",
        makeSeq(make<Variable>("inputVar"),
                Constant::int32(getBSONTypeMask(sbe::value::TypeTags::NumberInt32) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberInt64) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDouble) |
                                getBSONTypeMask(sbe::value::TypeTags::NumberDecimal))));

    auto env = VariableEnvironment::build(tree);
    sbe::InputParamToSlotMap inputParamToSlotMap;
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);
    ASSERT(expr);

    auto compiledExpr = compileExpression(*expr);

    {
        inputAccessor.reset(sbe::value::TypeTags::NumberDouble,
                            sbe::value::bitcastFrom<double>(123.0));
        auto result = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT(result);
    }

    {
        auto [tag, val] = sbe::value::makeNewString("123");
        inputAccessor.reset(tag, val);
        auto result = runCompiledExpressionPredicate(compiledExpr.get());
        ASSERT(!result);
    }
}

TEST_F(AbtToSbeExpression, LowerComparisonCollation) {
    sbe::value::OwnedValueAccessor lhsAccessor;
    sbe::value::OwnedValueAccessor rhsAccessor;
    auto lhsSlotId = bindAccessor(&lhsAccessor);
    auto rhsSlotId = bindAccessor(&rhsAccessor);
    SlotVarMap map;
    map["lhs"] = lhsSlotId;
    map["rhs"] = rhsSlotId;

    sbe::InputParamToSlotMap inputParamToSlotMap;

    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    registerSlot("collator"_sd,
                 sbe::value::TypeTags::collator,
                 sbe::value::bitcastFrom<const CollatorInterface*>(&collator),
                 false);

    auto tree = make<BinaryOp>(Operations::Cmp3w, make<Variable>("lhs"), make<Variable>("rhs"));
    auto env = VariableEnvironment::build(tree);
    auto expr =
        SBEExpressionLowering{env, map, *runtimeEnv(), slotIdGenerator(), inputParamToSlotMap}
            .optimize(tree);

    ASSERT(expr);
    auto compiledExpr = compileExpression(*expr);

    auto checkCmp3w = [&](StringData lhs, StringData rhs, int result) {
        auto [lhsTag, lhsValue] = sbe::value::makeNewString(lhs);
        lhsAccessor.reset(true, lhsTag, lhsValue);
        auto [rhsTag, rhsValue] = sbe::value::makeNewString(rhs);
        rhsAccessor.reset(true, rhsTag, rhsValue);

        auto [tag, value] = runCompiledExpression(compiledExpr.get());
        sbe::value::ValueGuard guard(tag, value);

        ASSERT_EQ(sbe::value::TypeTags::NumberInt32, tag);
        ASSERT_EQ(result, sbe::value::bitcastTo<int32_t>(value))
            << "comparing string '" << lhs << "' and '" << rhs << "'";
    };

    checkCmp3w("ABC", "abc", 0);
    checkCmp3w("aCC", "abb", 1);
    checkCmp3w("AbX", "aBy", -1);
}
}  // namespace
}  // namespace mongo::stage_builder::abt
