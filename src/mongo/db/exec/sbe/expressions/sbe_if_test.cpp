// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
    std::vector<value::TagValueOwned> boolTestValues =
        makeOwnedVector({makeNothing(), makeBool(false), makeBool(true)});
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
    for (const auto& cond : boolTestValues) {
        condAccessor.reset(cond.tag(), cond.value());
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
    for (const auto& cond : boolTestValues) {
        condAccessor.reset(cond.tag(), cond.value());
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
    for (const auto& cond : boolTestValues)
        for (const auto& cond2 : boolTestValues) {
            condAccessor.reset(cond.tag(), cond.value());
            cond2Accessor.reset(cond2.tag(), cond2.value());
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
    for (const auto& cond : boolTestValues)
        for (const auto& cond2 : boolTestValues) {
            condAccessor.reset(cond.tag(), cond.value());
            cond2Accessor.reset(cond2.tag(), cond2.value());
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
    for (const auto& cond : boolTestValues)
        for (const auto& cond2 : boolTestValues) {
            condAccessor.reset(cond.tag(), cond.value());
            cond2Accessor.reset(cond2.tag(), cond2.value());
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
    for (const auto& cond : boolTestValues)
        for (const auto& cond2 : boolTestValues) {
            condAccessor.reset(cond.tag(), cond.value());
            cond2Accessor.reset(cond2.tag(), cond2.value());
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
    for (const auto& cond : boolTestValues) {
        condAccessor.reset(cond.tag(), cond.value());
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
    for (const auto& cond : boolTestValues)
        for (const auto& cond2 : boolTestValues) {
            condAccessor.reset(cond.tag(), cond.value());
            cond2Accessor.reset(cond2.tag(), cond2.value());
            executeAndPrintVariation(os, *compiledExpr);
        }
}

}  // namespace mongo::sbe
