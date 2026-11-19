// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/expressions/sbe_test_interruptible.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <utility>

namespace mongo::sbe {
using SBELambdaTest = GoldenEExpressionTestFixture;

TEST_F(SBELambdaTest, TraverseP_AddOneToArray) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame = 10;
    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kTraverseP,
        sbe::makeEs(makeE<EVariable>(argSlot),
                    makeE<ELocalLambda>(frame,
                                        makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                           makeE<EVariable>(frame, 0),
                                                           makeC(makeInt32(1)))),
                    makeC(makeNothing())));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    auto bsonArr = BSON_ARRAY(1 << 2 << 3);

    slotAccessor.reset(value::TypeTags::bsonArray,
                       value::bitcastFrom<const char*>(bsonArr.objdata()));
    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELambdaTest, TraverseP_AddOneToFirstArrayItem) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame = 10;
    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kTraverseP,
        sbe::makeEs(makeE<EVariable>(argSlot),
                    makeE<ELocalLambda>(frame,
                                        makeE<EIf>(makeE<EPrimBinary>(EPrimBinary::Op::eq,
                                                                      makeE<EVariable>(frame, 1),
                                                                      makeC(makeInt64(0))),
                                                   makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                                      makeE<EVariable>(frame, 0),
                                                                      makeC(makeInt32(1))),
                                                   makeE<EVariable>(frame, 0)),
                                        2),
                    makeC(makeNothing())));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    auto bsonArr = BSON_ARRAY(1 << 2 << 3);

    slotAccessor.reset(value::TypeTags::bsonArray,
                       value::bitcastFrom<const char*>(bsonArr.objdata()));
    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELambdaTest, TraverseF_OpEq) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame = 10;
    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kTraverseF,
        sbe::makeEs(makeE<EVariable>(argSlot),
                    makeE<ELocalLambda>(frame,
                                        makeE<EPrimBinary>(EPrimBinary::Op::eq,
                                                           makeE<EVariable>(frame, 0),
                                                           makeC(makeInt32(3)))),
                    makeC(makeNothing())));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    auto bsonArr = BSON_ARRAY(1 << 2 << 3 << 4);

    slotAccessor.reset(value::TypeTags::bsonArray,
                       value::bitcastFrom<const char*>(bsonArr.objdata()));
    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELambdaTest, TraverseF_OpEqFirstArrayItem) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame = 10;
    auto expr = sbe::makeE<sbe::EFunction>(
        EFn::kTraverseF,
        sbe::makeEs(
            makeE<EVariable>(argSlot),
            makeE<ELocalLambda>(frame,
                                makeE<EPrimBinary>(EPrimBinary::Op::logicAnd,
                                                   makeE<EPrimBinary>(EPrimBinary::Op::eq,
                                                                      makeE<EVariable>(frame, 1),
                                                                      makeC(makeInt64(0))),
                                                   makeE<EPrimBinary>(EPrimBinary::Op::eq,
                                                                      makeE<EVariable>(frame, 0),
                                                                      makeC(makeInt32(3)))),
                                2),
            makeC(makeNothing())));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    auto bsonArr = BSON_ARRAY(1 << 2 << 3 << 4);

    slotAccessor.reset(value::TypeTags::bsonArray,
                       value::bitcastFrom<const char*>(bsonArr.objdata()));
    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELambdaTest, TraverseF_WithLocalBind) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame1 = 10;
    FrameId frame2 = 20;
    auto traverseExpr = sbe::makeE<sbe::EFunction>(
        EFn::kTraverseF,
        sbe::makeEs(makeE<EVariable>(frame2, 0),
                    makeE<ELocalLambda>(frame1,
                                        makeE<EPrimBinary>(EPrimBinary::Op::eq,
                                                           makeE<EVariable>(frame1, 0),
                                                           makeC(makeInt32(3)))),
                    makeC(makeNothing())));

    auto ifExpr = sbe::makeE<sbe::EIf>(std::move(traverseExpr),
                                       sbe::makeE<EVariable>(frame2, 1),
                                       sbe::makeE<EVariable>(frame2, 2));

    auto expr = sbe::makeE<ELocalBind>(
        frame2,
        makeEs(makeE<EVariable>(argSlot), makeC(makeInt32(10)), makeC(makeInt32(20))),
        std::move(ifExpr));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    auto bsonArr = BSON_ARRAY(1 << 2 << 3 << 4);

    slotAccessor.reset(value::TypeTags::bsonArray,
                       value::bitcastFrom<const char*>(bsonArr.objdata()));
    executeAndPrintVariation(os, *compiledExpr);
}

using SBEInterruptibleTraversalTest = EExpressionTestFixture;

TEST_F(SBEInterruptibleTraversalTest, InterruptsAfterFirstArrayBatch) {
    BSONArrayBuilder array;
    for (int i = 0; i < 16384; ++i) {
        array.append(false);
    }
    const auto input = array.arr();
    for (bool immediate : {false, true}) {
        value::ViewOfValueAccessor compareArray;
        compareArray.reset(value::TypeTags::Boolean, value::bitcastFrom<bool>(false));
        const auto compareSlot = bindAccessor(&compareArray);
        auto [tag, val] = makeArray(input);
        auto expr =
            makeE<EFunction>(EFn::kTraverseF,
                             makeEs(makeE<EConstant>(tag, val),
                                    makeE<ELocalLambda>(10, makeE<EVariable>(10, 0)),
                                    immediate ? std::unique_ptr<EExpression>(makeC(makeBool(false)))
                                              : makeE<EVariable>(compareSlot)));
        auto code = compileExpression(*expr);
        CountingInterruptible interruptible(2);
        ASSERT_THROWS_CODE(
            _vm.run(code.get(), &interruptible), DBException, ErrorCodes::Interrupted);
        ASSERT_EQ(interruptible.checks(), 2);
    }
}

TEST_F(SBEInterruptibleTraversalTest, NestedMembershipSharesInterruptible) {
    BSONArrayBuilder array;
    for (int i = 0; i < 16384; ++i) {
        array.append(false);
    }
    auto [innerTag, innerVal] = makeArray(array.arr());
    auto outer = value::TagValueOwned::fromRaw(value::makeNewArray());
    value::getArrayView(outer.value())->push_back_raw(innerTag, innerVal);
    auto [outerTag, outerVal] = outer.releaseToRaw();
    auto expr = makeE<EFunction>(
        EFn::kTraverseF,
        makeEs(makeE<EConstant>(outerTag, outerVal),
               makeE<ELocalLambda>(
                   10,
                   makeE<EFunction>(EFn::kIsMember,
                                    makeEs(makeC(makeBool(true)), makeE<EVariable>(10, 0)))),
               makeC(makeBool(false))));
    auto code = compileExpression(*expr);
    CountingInterruptible interruptible(2);
    ASSERT_THROWS_CODE(_vm.run(code.get(), &interruptible), DBException, ErrorCodes::Interrupted);
    ASSERT_EQ(interruptible.checks(), 2);
}

TEST_F(SBEInterruptibleTraversalTest, ShortNestedMembershipScansShareInterruptBudget) {
    BSONArrayBuilder array;
    for (int i = 0; i < 511; ++i) {
        array.append(BSON_ARRAY(false));
    }
    auto [tag, val] = makeArray(array.arr());
    auto expr = makeE<EFunction>(
        EFn::kTraverseF,
        makeEs(makeE<EConstant>(tag, val),
               makeE<ELocalLambda>(
                   10,
                   makeE<EFunction>(EFn::kIsMember,
                                    makeEs(makeC(makeBool(true)), makeE<EVariable>(10, 0)))),
               makeC(makeBool(false))));
    auto code = compileExpression(*expr);
    CountingInterruptible interruptible;
    ASSERT_FALSE(_vm.runPredicate(code.get(), &interruptible));
    ASSERT_EQ(interruptible.checks(), 0);
    ASSERT_FALSE(_vm.runPredicate(code.get(), &interruptible));
    ASSERT_EQ(interruptible.checks(), 1);
}

}  // namespace mongo::sbe
