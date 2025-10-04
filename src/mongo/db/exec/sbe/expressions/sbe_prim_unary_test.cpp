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
#include <utility>
#include <vector>

namespace mongo::sbe {

class SBEPrimUnaryTest : public GoldenEExpressionTestFixture {
public:
    void runUnaryOpTest(std::ostream& os, EPrimUnary::Op op, std::vector<TypedValue>& testValues) {
        value::ViewOfValueAccessor lhsAccessor;

        auto lhsSlot = bindAccessor(&lhsAccessor);

        auto expr = sbe::makeE<EPrimUnary>(op, makeE<EVariable>(lhsSlot));
        printInputExpression(os, *expr);

        auto compiledExpr = compileExpression(*expr);
        printCompiledExpression(os, *compiledExpr);

        // Verify the operator table
        for (auto lhs : testValues) {
            lhsAccessor.reset(lhs.first, lhs.second);
            executeAndPrintVariation(os, *compiledExpr);
        }
    }

protected:
    std::vector<TypedValue> boolTestValues = {makeNothing(), makeBool(false), makeBool(true)};
    ValueVectorGuard boolTestValuesGuard{boolTestValues};

    std::vector<TypedValue> numericTestValues = {makeNothing(),
                                                 makeInt32(0),
                                                 makeInt32(12),
                                                 makeInt32(23),
                                                 makeInt64(123),
                                                 makeDouble(123.5),
                                                 value::makeCopyDecimal(Decimal128(223.5))};
    ValueVectorGuard numericTestValuesGuard{numericTestValues};

    std::vector<TypedValue> mixedTestValues = {makeNothing(),
                                               makeNull(),
                                               makeBool(false),
                                               makeBool(true),
                                               makeInt32(12),
                                               value::makeCopyDecimal(Decimal128(223.5)),
                                               value::makeNewString("abc"_sd),
                                               makeTimestamp(Timestamp(1668792433))};
    ValueVectorGuard mixedTestValuesGuard{mixedTestValues};
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
