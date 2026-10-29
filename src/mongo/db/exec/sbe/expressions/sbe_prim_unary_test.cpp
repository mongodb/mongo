// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"

#include <iosfwd>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

class SBEPrimUnaryTest : public GoldenEExpressionTestFixture {
public:
    void runUnaryOpTest(std::ostream& os,
                        EPrimUnary::Op op,
                        const std::vector<value::TagValueOwned>& testValues) {
        value::ViewOfValueAccessor lhsAccessor;

        auto lhsSlot = bindAccessor(&lhsAccessor);

        auto expr = sbe::makeE<EPrimUnary>(op, makeE<EVariable>(lhsSlot));
        printInputExpression(os, *expr);

        auto compiledExpr = compileExpression(*expr);
        printCompiledExpression(os, *compiledExpr);

        // Verify the operator table
        for (const auto& lhs : testValues) {
            lhsAccessor.reset(lhs.tag(), lhs.value());
            executeAndPrintVariation(os, *compiledExpr);
        }
    }

protected:
    std::vector<value::TagValueOwned> boolTestValues =
        makeOwnedVector({makeNothing(), makeBool(false), makeBool(true)});

    std::vector<value::TagValueOwned> numericTestValues =
        makeOwnedVector({makeNothing(),
                         makeInt32(0),
                         makeInt32(12),
                         makeInt32(23),
                         makeInt64(123),
                         makeDouble(123.5),
                         value::makeCopyDecimal(Decimal128(223.5))});

    std::vector<value::TagValueOwned> mixedTestValues =
        makeOwnedVector({makeNothing(),
                         makeNull(),
                         makeBool(false),
                         makeBool(true),
                         makeInt32(12),
                         value::makeCopyDecimal(Decimal128(223.5)),
                         value::makeNewString("abc"sv),
                         makeTimestamp(Timestamp(1668792433))});
};

TEST_F(SBEPrimUnaryTest, LogicNotBool) {
    auto& os = gctx->outStream();
    runUnaryOpTest(os, EPrimUnary::Op::logicNot, boolTestValues);
}

TEST_F(SBEPrimUnaryTest, LogicNotMixed) {
    auto& os = gctx->outStream();
    runUnaryOpTest(os, EPrimUnary::Op::logicNot, mixedTestValues);
}


TEST_F(SBEPrimUnaryTest, NegateNumeric) {
    auto& os = gctx->outStream();
    runUnaryOpTest(os, EPrimUnary::Op::negate, numericTestValues);
}

TEST_F(SBEPrimUnaryTest, NegateMixed) {
    auto& os = gctx->outStream();
    runUnaryOpTest(os, EPrimUnary::Op::negate, mixedTestValues);
}

}  // namespace mongo::sbe
