// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

using SBEVariableTest = GoldenEExpressionTestFixture;

TEST_F(SBEVariableTest, SlotVariable) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    auto slotId = bindAccessor(&slotAccessor);
    auto expr = makeE<EVariable>(slotId);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    std::vector<TypedValue> testValues = {makeNothing(), makeBool(true), makeInt32(123)};
    for (auto p : testValues) {
        slotAccessor.reset(p.first, p.second);

        executeAndPrintVariation(os, *compiledExpr);
    }
}

TEST_F(SBEVariableTest, LocalVariable) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(
        frame,
        makeEs(makeC(value::makeNewString("abcdeghijklmnop"sv))),
        makeE<EFunction>(EFn::kNewArray,
                         makeEs(makeE<EVariable>(frame, 0), makeE<EVariable>(frame, 0))));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBEVariableTest, LocalVariableMove) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor slotAccessor;
    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(frame,
                                       makeEs(makeC(value::makeNewString("abcdeghijklmnop"sv))),
                                       makeE<EFunction>(EFn::kNewArray,
                                                        makeEs(makeE<EVariable>(frame, 0, true),
                                                               makeE<EVariable>(frame, 0, true))));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

}  // namespace mongo::sbe
