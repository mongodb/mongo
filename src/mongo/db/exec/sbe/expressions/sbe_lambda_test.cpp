/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {
using SBELambdaTest = GoldenEExpressionTestFixture;

TEST_F(SBELambdaTest, TraverseP_AddOneToArray) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame = 10;
    auto expr = sbe::makeE<sbe::EFunction>(
        "traverseP",
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

TEST_F(SBELambdaTest, TraverseF_OpEq) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame = 10;
    auto expr = sbe::makeE<sbe::EFunction>(
        "traverseF",
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


TEST_F(SBELambdaTest, TraverseF_WithLocalBind) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto argSlot = bindAccessor(&slotAccessor);
    FrameId frame1 = 10;
    FrameId frame2 = 20;
    auto traverseExpr = sbe::makeE<sbe::EFunction>(
        "traverseF",
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
