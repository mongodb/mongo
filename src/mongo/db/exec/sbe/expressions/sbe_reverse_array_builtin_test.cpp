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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/bson.h"

namespace mongo::sbe {

class SBEBuiltinReverseArrayTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Assert that result of 'reverseArray(array)' is equal to 'expected'.
     * NOTE: Values behind arguments and the return value of this function are owned by the caller.
     */
    void runAndAssertExpression(TypedValue array, TypedValue expected) {
        // We do not copy array value on purpose. During the copy, order of elements in ArraySet
        // may change. 'reverseArray' return value depends on the order of elements in the input
        // array. After copy 'reverse' may return elements in a different order from the one
        // expected by the caller.
        value::ViewOfValueAccessor arraySlotAccessor;
        auto arraySlot = bindAccessor(&arraySlotAccessor);
        arraySlotAccessor.reset(array.first, array.second);
        auto arrayExpr = makeE<EVariable>(arraySlot);

        auto reverseArrayExpr = makeE<EFunction>("reverseArray", makeEs(std::move(arrayExpr)));
        auto compiledExpr = compileExpression(*reverseArrayExpr);

        auto actual = runCompiledExpression(compiledExpr.get());
        value::ValueGuard actualGuard{actual};

        auto [compareTag, compareValue] =
            value::compareValue(actual.first, actual.second, expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }
};

TEST_F(SBEBuiltinReverseArrayTest, Array) {
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        auto testArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::ValueGuard testArrayGuard{testArray};

        auto expectedResult = makeArray(BSON_ARRAY(3 << 2 << 1));
        value::ValueGuard expectedResultGuard{expectedResult};

        runAndAssertExpression(testArray, expectedResult);
    }
}

TEST_F(SBEBuiltinReverseArrayTest, ArraySet) {
    // This test needs to do a bit more work to find the correct reversed order since ArraySet's
    // internal order is determined by it's hash function and not the order that elements are added
    // to it.
    auto testArray = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};
    value::ArrayEnumerator testEnumerator{testArray.first, testArray.second};

    std::vector<std::pair<value::TypeTags, value::Value>> testArrayContents;
    auto expectedResult = value::makeNewArray();
    value::ValueGuard expectedResultGuard{expectedResult};
    auto expectedResultView = value::getArrayView(expectedResult.second);

    while (!testEnumerator.atEnd()) {
        testArrayContents.push_back(testEnumerator.getViewOfValue());
        testEnumerator.advance();
    }

    for (auto it = testArrayContents.rbegin(); it != testArrayContents.rend(); ++it) {
        auto [copyTag, copyVal] = copyValue(it->first, it->second);
        expectedResultView->push_back(copyTag, copyVal);
    }

    runAndAssertExpression(testArray, expectedResult);
}

TEST_F(SBEBuiltinReverseArrayTest, NotArray) {
    runAndAssertExpression(makeNothing(), makeNothing());
    runAndAssertExpression(makeInt32(123), makeNothing());
}

}  // namespace mongo::sbe
