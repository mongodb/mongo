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
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
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
    void validateExpression(const std::string& builtinMethodName,
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
    auto sbeTimestamp = makeTimestamp(timestamp);
    auto expectedSecs = makeInt64(timestamp.getSecs());

    value::ValueGuard guardSbeTimestamp{sbeTimestamp};
    value::ValueGuard guardExpectedSecs{expectedSecs};

    validateExpression("tsSecond", sbeTimestamp, expectedSecs);
}

TEST_F(SBEBuiltinTsSecondTest, HandlesInvalidTimestamp) {
    auto timestamp = 1622731060;
    auto sbeInvalidTimestamp = makeInt64(timestamp);
    auto expectedNothing = makeNothing();

    value::ValueGuard guardSbeInvalidTimestamp{sbeInvalidTimestamp};
    value::ValueGuard guardExpectedNothing{expectedNothing};

    validateExpression("tsSecond", sbeInvalidTimestamp, expectedNothing);
}

TEST_F(SBEBuiltinTsIncrementTest, HandlesTimestamp) {
    auto timestamp = Timestamp(1622731060, 10);
    auto sbeTimestamp = makeTimestamp(timestamp);
    auto expectedInc = makeInt64(timestamp.getInc());

    value::ValueGuard guardSbeTimestamp{sbeTimestamp};
    value::ValueGuard guardExpectedInc{expectedInc};

    validateExpression("tsIncrement", sbeTimestamp, expectedInc);
}

TEST_F(SBEBuiltinTsIncrementTest, HandlesInvalidTimestamp) {
    auto timestamp = 10;
    auto sbeInvalidTimestamp = makeInt64(timestamp);
    auto expectedNothing = makeNothing();

    value::ValueGuard guardSbeInvalidTimestamp{sbeInvalidTimestamp};
    value::ValueGuard guardExpectedNothing{expectedNothing};

    validateExpression("tsIncrement", sbeInvalidTimestamp, expectedNothing);
}

}  // namespace mongo::sbe
