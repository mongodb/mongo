// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
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

}  // namespace mongo::sbe
