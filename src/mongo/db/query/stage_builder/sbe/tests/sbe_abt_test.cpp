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
#include "mongo/db/query/stage_builder/sbe/abt_lower.h"
#include "mongo/db/query/stage_builder/sbe/abt_lower_defs.h"

namespace mongo::stage_builder::abt_lower {
namespace {

using namespace abt;

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
    auto arrResult = sbe::value::getArrayView(resultVal);
    ASSERT_EQ(4, arrResult->size());
    auto arrResult0 = arrResult->values()[0];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult0.first);
    ASSERT_EQ(11, sbe::value::bitcastTo<int64_t>(arrResult0.second));
    auto arrResult1 = arrResult->values()[1];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult1.first);
    ASSERT_EQ(12, sbe::value::bitcastTo<int64_t>(arrResult1.second));
    auto arrResult2 = arrResult->values()[2];
    ASSERT_EQ(sbe::value::TypeTags::Array, arrResult2.first);
    auto arrResult2v = sbe::value::getArrayView(arrResult2.second);
    ASSERT_EQ(2, arrResult2v->size());

    auto arrResult2v0 = arrResult2v->values()[0];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult2v0.first);
    ASSERT_EQ(31, sbe::value::bitcastTo<int64_t>(arrResult2v0.second));
    auto arrResult2v1 = arrResult2v->values()[1];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult2v1.first);
    ASSERT_EQ(32, sbe::value::bitcastTo<int64_t>(arrResult2v1.second));

    auto arrResult3 = arrResult->values()[3];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult3.first);
    ASSERT_EQ(13, sbe::value::bitcastTo<int64_t>(arrResult3.second));
}

TEST_F(AbtToSbeExpression, Lower4TwoArgsOneLevel) {
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
        makeSeq(
            make<Constant>(tagArr, valArr),
            make<LambdaAbstraction>(
                "value",
                "index",
                make<If>(
                    // Comparing the index with a full int64 implies we only process the first
                    // level.
                    make<BinaryOp>(Operations::Eq, make<Variable>("index"), Constant::int64(1)),
                    make<BinaryOp>(Operations::Add, make<Variable>("value"), Constant::int64(10)),
                    Constant::nothing())),
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
    auto arrResult = sbe::value::getArrayView(resultVal);
    ASSERT_EQ(2, arrResult->size());
    auto arrResult0 = arrResult->values()[0];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult0.first);
    ASSERT_EQ(12, sbe::value::bitcastTo<int64_t>(arrResult0.second));
    auto arrResult1 = arrResult->values()[1];
    ASSERT_EQ(sbe::value::TypeTags::Array, arrResult1.first);
    ASSERT_EQ(0, sbe::value::getArrayView(arrResult1.second)->size());
}


TEST_F(AbtToSbeExpression, Lower4TwoArgsAnyLevel) {
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
        makeSeq(
            make<Constant>(tagArr, valArr),
            make<LambdaAbstraction>(
                "value",
                "index",
                make<If>(
                    // Trimming the index to the lowest 32 bits makes the test work on any level.
                    make<BinaryOp>(
                        Operations::Eq,
                        make<FunctionCall>(
                            "mod", makeSeq(make<Variable>("index"), Constant::int64(1LL << 32))),
                        Constant::int64(1)),
                    make<BinaryOp>(Operations::Add, make<Variable>("value"), Constant::int64(10)),
                    Constant::nothing())),
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
    auto arrResult = sbe::value::getArrayView(resultVal);
    ASSERT_EQ(2, arrResult->size());
    auto arrResult0 = arrResult->values()[0];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult0.first);
    ASSERT_EQ(12, sbe::value::bitcastTo<int64_t>(arrResult0.second));
    auto arrResult1 = arrResult->values()[1];
    ASSERT_EQ(sbe::value::TypeTags::Array, arrResult1.first);
    auto arrResult1v = sbe::value::getArrayView(arrResult1.second);
    ASSERT_EQ(1, arrResult1v->size());

    auto arrResult1v0 = arrResult1v->values()[0];
    ASSERT_EQ(sbe::value::TypeTags::NumberInt64, arrResult1v0.first);
    ASSERT_EQ(32, sbe::value::bitcastTo<int64_t>(arrResult1v0.second));
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

TEST_F(AbtToSbeExpression, LowerMultiLet) {
    {
        //  MultiLet [x = 100, y = var] in
        //      y / x
        //
        // var: 500

        auto tree = make<MultiLet>(
            std::vector<std::pair<ProjectionName, ABT>>{{"x", Constant::int64(100)},
                                                        {"y", make<Variable>("var")}},
            make<BinaryOp>(Operations::Div, make<Variable>("y"), make<Variable>("x")));

        boost::optional<
            std::pair<ProjectionName, std::pair<sbe::value::TypeTags, sbe::value::Value>>>
            var = {{ProjectionName{"var"}, std::pair{sbe::value::TypeTags::NumberInt64, 500}}};
        auto [resultTag, resultVal] = evalExpr(tree, var);

        ASSERT_EQ(sbe::value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<double>(resultVal), 5.0);
    }
    {
        //  MultiLet [x = 1, y = x + 2, z = x + y + 3] in
        //      x + y + z
        //

        auto tree = make<MultiLet>(
            std::vector<std::pair<ProjectionName, ABT>>{
                {"x", Constant::int64(1)},
                {"y", make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(2))},
                {"z",
                 make<BinaryOp>(
                     Operations::Add,
                     make<Variable>("x"),
                     make<BinaryOp>(Operations::Add, make<Variable>("y"), Constant::int64(3)))}},
            make<BinaryOp>(
                Operations::Add,
                make<Variable>("x"),
                make<BinaryOp>(Operations::Add, make<Variable>("y"), make<Variable>("z"))));

        auto [resultTag, resultVal] = evalExpr(tree, boost::none);

        ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 11);
    }
    {
        //  Let [var2 = 100] in
        //      MultiLet [x = var2, y = var] in
        //          y / x
        //
        // var: 500

        auto tree = make<Let>(
            "var2",
            Constant::int64(100),
            make<MultiLet>(
                std::vector<std::pair<ProjectionName, ABT>>{{"x", make<Variable>("var2")},
                                                            {"y", make<Variable>("var")}},
                make<BinaryOp>(Operations::Div, make<Variable>("y"), make<Variable>("x"))));

        boost::optional<
            std::pair<ProjectionName, std::pair<sbe::value::TypeTags, sbe::value::Value>>>
            var = {{ProjectionName{"var"}, std::pair{sbe::value::TypeTags::NumberInt64, 500}}};
        auto [resultTag, resultVal] = evalExpr(tree, var);

        ASSERT_EQ(sbe::value::TypeTags::NumberDouble, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<double>(resultVal), 5.0);
    }
    {
        auto tree = make<MultiLet>(
            std::vector<ProjectionName>{"x"},
            makeSeq(Constant::int64(10),
                    make<BinaryOp>(Operations::Add, make<Variable>("x"), Constant::int64(1))));
        auto [resultTag, resultVal] = evalExpr(tree, boost::none);

        ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 11);
    }
}

TEST_F(AbtToSbeExpression, LowerNaryAdd) {
    {
        auto tree = make<NaryOp>(Operations::Add,
                                 ABTVector{Constant::int64(1),
                                           Constant::int64(5),
                                           Constant::int64(10),
                                           Constant::int64(20),
                                           Constant::int64(100)});

        auto [resultTag, resultVal] = evalExpr(tree, boost::none);

        ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 136);
    }
    {
        auto tree = make<NaryOp>(
            Operations::Add,
            ABTVector{make<BinaryOp>(Operations::Sub, Constant::int64(10), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(20), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(30), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(40), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(50), make<Variable>("var"))});

        auto [resultTag, resultVal] =
            evalExpr(tree, std::pair{ProjectionName{"var"}, sbe::value::makeIntOrLong(5)});

        ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 125);
    }
}

TEST_F(AbtToSbeExpression, LowerNaryMult) {
    {
        auto tree = make<NaryOp>(Operations::Mult,
                                 ABTVector{Constant::int64(1),
                                           Constant::int64(5),
                                           Constant::int64(10),
                                           Constant::int64(20),
                                           Constant::int64(100)});

        auto [resultTag, resultVal] = evalExpr(tree, boost::none);

        ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 100000);
    }
    {
        auto tree = make<NaryOp>(
            Operations::Mult,
            ABTVector{make<BinaryOp>(Operations::Sub, Constant::int64(10), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(20), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(30), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(40), make<Variable>("var")),
                      make<BinaryOp>(Operations::Sub, Constant::int64(50), make<Variable>("var"))});

        auto [resultTag, resultVal] =
            evalExpr(tree, std::pair{ProjectionName{"var"}, sbe::value::makeIntOrLong(5)});

        ASSERT_EQ(sbe::value::TypeTags::NumberInt64, resultTag);
        ASSERT_EQ(sbe::value::bitcastTo<int64_t>(resultVal), 2953125);
    }
}

}  // namespace
}  // namespace mongo::stage_builder::abt_lower
