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
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {

class SBEIfTest : public GoldenEExpressionTestFixture {
protected:
    std::vector<TypedValue> boolTestValues = {makeNothing(), makeBool(false), makeBool(true)};
    ValueVectorGuard boolTestValuesGuard{boolTestValues};
};

TEST_F(SBEIfTest, SimpleIf) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;

    auto condSlot = bindAccessor(&condAccessor);

    auto expr = sbe::makeE<EIf>(makeE<EVariable>(condSlot),
                                makeC(value::makeNewString("then")),
                                makeC(value::makeNewString("else")));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues) {
        condAccessor.reset(cond.first, cond.second);
        executeAndPrintVariation(os, *compiledExpr);
    }
}

TEST_F(SBEIfTest, NestedIfCond) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;

    auto condSlot = bindAccessor(&condAccessor);


    auto ifExpr =
        sbe::makeE<EIf>(makeE<EVariable>(condSlot), makeC(makeBool(false)), makeC(makeBool(true)));

    auto expr = sbe::makeE<EIf>(std::move(ifExpr),
                                makeC(value::makeNewString("then")),
                                makeC(value::makeNewString("else")));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues) {
        condAccessor.reset(cond.first, cond.second);
        executeAndPrintVariation(os, *compiledExpr);
    }
}

TEST_F(SBEIfTest, NestedIfThen) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;
    value::ViewOfValueAccessor cond2Accessor;

    auto condSlot = bindAccessor(&condAccessor);
    auto cond2Slot = bindAccessor(&cond2Accessor);


    auto ifExpr = sbe::makeE<EIf>(makeE<EVariable>(condSlot),
                                  makeC(value::makeNewString("then")),
                                  makeC(value::makeNewString("else")));

    auto expr = sbe::makeE<EIf>(
        makeE<EVariable>(cond2Slot), std::move(ifExpr), makeC(value::makeNewString("else2")));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues)
        for (auto cond2 : boolTestValues) {
            condAccessor.reset(cond.first, cond.second);
            cond2Accessor.reset(cond2.first, cond2.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
}


TEST_F(SBEIfTest, NestedIfElse) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;
    value::ViewOfValueAccessor cond2Accessor;

    auto condSlot = bindAccessor(&condAccessor);
    auto cond2Slot = bindAccessor(&cond2Accessor);

    auto ifExpr = sbe::makeE<EIf>(makeE<EVariable>(condSlot),
                                  makeC(value::makeNewString("then")),
                                  makeC(value::makeNewString("else")));

    auto expr = sbe::makeE<EIf>(
        makeE<EVariable>(cond2Slot), makeC(value::makeNewString("then2")), std::move(ifExpr));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues)
        for (auto cond2 : boolTestValues) {
            condAccessor.reset(cond.first, cond.second);
            cond2Accessor.reset(cond2.first, cond2.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
}


TEST_F(SBEIfTest, IfWithLogicAnd) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;
    value::ViewOfValueAccessor cond2Accessor;

    auto condSlot = bindAccessor(&condAccessor);
    auto cond2Slot = bindAccessor(&cond2Accessor);


    auto expr = sbe::makeE<EIf>(makeE<EPrimBinary>(EPrimBinary::Op::logicAnd,
                                                   makeE<EVariable>(condSlot),
                                                   makeE<EVariable>(cond2Slot)),
                                makeC(value::makeNewString("then")),
                                makeC(value::makeNewString("else")));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues)
        for (auto cond2 : boolTestValues) {
            condAccessor.reset(cond.first, cond.second);
            cond2Accessor.reset(cond2.first, cond2.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
}

TEST_F(SBEIfTest, IfWithLogicOr) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;
    value::ViewOfValueAccessor cond2Accessor;

    auto condSlot = bindAccessor(&condAccessor);
    auto cond2Slot = bindAccessor(&cond2Accessor);


    auto expr = sbe::makeE<EIf>(makeE<EPrimBinary>(EPrimBinary::Op::logicOr,
                                                   makeE<EVariable>(condSlot),
                                                   makeE<EVariable>(cond2Slot)),
                                makeC(value::makeNewString("then")),
                                makeC(value::makeNewString("else")));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues)
        for (auto cond2 : boolTestValues) {
            condAccessor.reset(cond.first, cond.second);
            cond2Accessor.reset(cond2.first, cond2.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
}

TEST_F(SBEIfTest, IfWithLogicNot) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;

    auto condSlot = bindAccessor(&condAccessor);

    auto expr =
        sbe::makeE<EIf>(makeE<EPrimUnary>(EPrimUnary::Op::logicNot, makeE<EVariable>(condSlot)),
                        makeC(value::makeNewString("then")),
                        makeC(value::makeNewString("else")));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues) {
        condAccessor.reset(cond.first, cond.second);
        executeAndPrintVariation(os, *compiledExpr);
    }
}


TEST_F(SBEIfTest, IfWithFillEmpty) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;
    value::ViewOfValueAccessor cond2Accessor;

    auto condSlot = bindAccessor(&condAccessor);
    auto cond2Slot = bindAccessor(&cond2Accessor);

    auto expr = sbe::makeE<EIf>(makeE<EPrimBinary>(EPrimBinary::Op::fillEmpty,
                                                   makeE<EVariable>(condSlot),
                                                   makeE<EVariable>(cond2Slot)),
                                makeC(value::makeNewString("then")),
                                makeC(value::makeNewString("else")));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    // Verify input variations.
    for (auto cond : boolTestValues)
        for (auto cond2 : boolTestValues) {
            condAccessor.reset(cond.first, cond.second);
            cond2Accessor.reset(cond2.first, cond2.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
}

}  // namespace mongo::sbe
