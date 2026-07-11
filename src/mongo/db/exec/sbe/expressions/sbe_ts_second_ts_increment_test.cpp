// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {

/**
 * Provides method that can be used in testing the '$tsSecond' and the '$tsIncrement' operators.
 */
class SBEBuiltinTsExpressionBase : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Compiles and runs the expression over the input TypedValue 'timestampValue' and validates if
     * the computed TypedValue is same as the 'expectedValue'.
     */
    void validateExpression(EFn builtinMethodName,
                            TypedValue timestampValue,
                            TypedValue expectedValue) {
        value::ViewOfValueAccessor valueAccessor;
        auto slotId = bindAccessor(&valueAccessor);
        valueAccessor.reset(timestampValue.first, timestampValue.second);

        auto tsExpressionFn = makeE<EFunction>(builtinMethodName, makeEs(makeE<EVariable>(slotId)));
        auto compiledExpr = compileExpression(*tsExpressionFn);
        auto returnedValue = runCompiledExpression(compiledExpr.get());

        auto [cmpTag, cmpValue] = value::compareValue(
            returnedValue.first, returnedValue.second, expectedValue.first, expectedValue.second);

        // A zero value of 'cmpValue' signifies that the compared values are equal. The 'cmpValue'
        // should be of type 'int32'.
        ASSERT_EQ(cmpTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(cmpValue, 0);
    }
};

using SBEBuiltinTsSecondTest = SBEBuiltinTsExpressionBase;
using SBEBuiltinTsIncrementTest = SBEBuiltinTsExpressionBase;

TEST_F(SBEBuiltinTsSecondTest, HandlesTimestamp) {
    auto timestamp = Timestamp(1622731060, 10);
    value::TagValueOwned sbeTimestamp = value::TagValueOwned::fromRaw(makeTimestamp(timestamp));
    value::TagValueOwned expectedSecs =
        value::TagValueOwned::fromRaw(makeInt64(timestamp.getSecs()));

    validateExpression(EFn::kTsSecond,
                       {sbeTimestamp.tag(), sbeTimestamp.value()},
                       {expectedSecs.tag(), expectedSecs.value()});
}

TEST_F(SBEBuiltinTsSecondTest, HandlesInvalidTimestamp) {
    auto timestamp = 1622731060;
    value::TagValueOwned sbeInvalidTimestamp = value::TagValueOwned::fromRaw(makeInt64(timestamp));
    value::TagValueOwned expectedNothing = value::TagValueOwned::fromRaw(makeNothing());

    validateExpression(EFn::kTsSecond,
                       {sbeInvalidTimestamp.tag(), sbeInvalidTimestamp.value()},
                       {expectedNothing.tag(), expectedNothing.value()});
}

TEST_F(SBEBuiltinTsIncrementTest, HandlesTimestamp) {
    auto timestamp = Timestamp(1622731060, 10);
    value::TagValueOwned sbeTimestamp = value::TagValueOwned::fromRaw(makeTimestamp(timestamp));
    value::TagValueOwned expectedInc = value::TagValueOwned::fromRaw(makeInt64(timestamp.getInc()));

    validateExpression(EFn::kTsIncrement,
                       {sbeTimestamp.tag(), sbeTimestamp.value()},
                       {expectedInc.tag(), expectedInc.value()});
}

TEST_F(SBEBuiltinTsIncrementTest, HandlesInvalidTimestamp) {
    auto timestamp = 10;
    value::TagValueOwned sbeInvalidTimestamp = value::TagValueOwned::fromRaw(makeInt64(timestamp));
    value::TagValueOwned expectedNothing = value::TagValueOwned::fromRaw(makeNothing());

    validateExpression(EFn::kTsIncrement,
                       {sbeInvalidTimestamp.tag(), sbeInvalidTimestamp.value()},
                       {expectedNothing.tag(), expectedNothing.value()});
}

}  // namespace mongo::sbe
