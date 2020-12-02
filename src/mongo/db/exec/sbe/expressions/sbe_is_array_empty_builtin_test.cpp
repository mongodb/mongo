/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <cmath>
#include <limits>

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/bson.h"

namespace mongo::sbe {

class SBEBuiltinIsArrayEmptyTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Assert that result of 'isArrayEmpty(array)' is equal to 'expectedRes'.
     * NOTE: Values behind arguments of this function are owned by the caller.
     */
    void runAndAssertExpression(TypedValue array, TypedValue expectedRes) {
        auto [arrayCopyType, arrayCopyValue] = value::copyValue(array.first, array.second);
        auto arrayExpr = makeE<EConstant>(arrayCopyType, arrayCopyValue);

        auto isArrayEmptyExpr = makeE<EFunction>("isArrayEmpty", makeEs(std::move(arrayExpr)));
        auto compiledExpr = compileExpression(*isArrayEmptyExpr);

        auto actualValue = runCompiledExpression(compiledExpr.get());
        value::ValueGuard actualValueGuard{actualValue};

        auto [compareTag, compareValue] = value::compareValue(
            actualValue.first, actualValue.second, expectedRes.first, expectedRes.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }
};

TEST_F(SBEBuiltinIsArrayEmptyTest, Array) {
    for (auto makeArrayFn : {makeBsonArray, makeArray, makeArraySet}) {
        auto emptyArray = makeArrayFn(BSONArray());
        value::ValueGuard emptyArrayGuard{emptyArray};

        runAndAssertExpression(emptyArray, makeBool(true));

        auto notEmptyArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::ValueGuard notEmptyArrayGuard{notEmptyArray};

        runAndAssertExpression(notEmptyArray, makeBool(false));
    }
}

TEST_F(SBEBuiltinIsArrayEmptyTest, NotArray) {
    runAndAssertExpression(makeNothing(), makeNothing());
    runAndAssertExpression(makeInt32(123), makeNothing());
}

}  // namespace mongo::sbe
