// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {
using SBELocalBindTest = GoldenEExpressionTestFixture;

TEST_F(SBELocalBindTest, OneVariable) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(frame,
                                       makeEs(makeC(makeInt32(10))),
                                       makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                          makeE<EVariable>(frame, 0),
                                                          makeE<EVariable>(frame, 0)));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELocalBindTest, TwoVariables) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(frame,
                                       makeEs(makeC(makeInt32(10)), makeC(makeInt32(20))),
                                       makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                          makeE<EVariable>(frame, 0),
                                                          makeE<EVariable>(frame, 1)));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELocalBindTest, NestedBind1) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame1 = 10;
    FrameId frame2 = 20;
    auto bindExpr = sbe::makeE<ELocalBind>(frame1,
                                           makeEs(makeC(makeInt32(10))),
                                           makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                              makeE<EVariable>(frame1, 0),
                                                              makeE<EVariable>(frame2, 0)));

    auto expr = sbe::makeE<ELocalBind>(
        frame2,
        makeEs(makeC(makeInt32(20))),
        makeE<EPrimBinary>(EPrimBinary::Op::add, std::move(bindExpr), makeE<EVariable>(frame2, 0)));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELocalBindTest, NestedBind2) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame1 = 10;
    FrameId frame2 = 20;
    auto bindExpr = sbe::makeE<ELocalBind>(frame1,
                                           makeEs(makeC(makeInt32(10)), makeC(makeInt32(20))),
                                           makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                              makeE<EVariable>(frame1, 0),
                                                              makeE<EVariable>(frame1, 1)));

    auto expr = sbe::makeE<ELocalBind>(frame2,
                                       makeEs(std::move(bindExpr), makeC(makeInt32(30))),
                                       makeE<EPrimBinary>(EPrimBinary::Op::add,
                                                          makeE<EVariable>(frame2, 0),
                                                          makeE<EVariable>(frame2, 1)));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELocalBindTest, BinaryOperatorLhsVariable) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(
        frame,
        makeEs(makeC(makeInt32(10))),
        makeE<EPrimBinary>(EPrimBinary::Op::sub, makeE<EVariable>(frame, 0), makeC(makeInt32(20))));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELocalBindTest, BinaryOperatorRhsVariable) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(
        frame,
        makeEs(makeC(makeInt32(10))),
        makeE<EPrimBinary>(EPrimBinary::Op::sub, makeC(makeInt32(20)), makeE<EVariable>(frame, 0)));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBELocalBindTest, BinaryOperatorBothVariables) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(frame,
                                       makeEs(makeC(makeInt32(10)), makeC(makeInt32(20))),
                                       makeE<EPrimBinary>(EPrimBinary::Op::sub,
                                                          makeE<EVariable>(frame, 0),
                                                          makeE<EVariable>(frame, 1)));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}


}  // namespace mongo::sbe
