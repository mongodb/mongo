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
        makeEs(makeC(value::makeNewString("abcdeghijklmnop"_sd))),
        makeE<EFunction>("newArray"_sd,
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
                                       makeEs(makeC(value::makeNewString("abcdeghijklmnop"_sd))),
                                       makeE<EFunction>("newArray"_sd,
                                                        makeEs(makeE<EVariable>(frame, 0, true),
                                                               makeE<EVariable>(frame, 0, true))));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

}  // namespace mongo::sbe
