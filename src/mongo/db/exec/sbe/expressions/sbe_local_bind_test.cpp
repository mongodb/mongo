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

#include "mongo/base/string_data.h"
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
