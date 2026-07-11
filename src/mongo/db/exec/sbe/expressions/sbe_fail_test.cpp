// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

using SBEFailTest = GoldenEExpressionTestFixture;

TEST_F(SBEFailTest, SimpleFail) {
    auto& os = gctx->outStream();

    auto expr = sbe::makeE<EFail>(ErrorCodes::Error::BadValue, "test"sv);
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBEFailTest, FailWithAddDecimal) {
    auto& os = gctx->outStream();

    value::ViewOfValueAccessor condAccessor;

    auto expr = sbe::makeE<EPrimBinary>(EPrimBinary::Op::add,
                                        makeC(value::makeCopyDecimal(Decimal128(123))),
                                        sbe::makeE<EFail>(ErrorCodes::Error::BadValue, "test"sv));
    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}

TEST_F(SBEFailTest, FailWithLocalBind) {
    auto& os = gctx->outStream();

    FrameId frame = 10;
    auto expr = sbe::makeE<ELocalBind>(frame,
                                       makeEs(makeC(value::makeCopyDecimal(Decimal128(123)))),
                                       sbe::makeE<EFail>(ErrorCodes::Error::BadValue, "test"sv));

    printInputExpression(os, *expr);

    auto compiledExpr = compileExpression(*expr);
    printCompiledExpression(os, *compiledExpr);

    executeAndPrintVariation(os, *compiledExpr);
}


}  // namespace mongo::sbe
