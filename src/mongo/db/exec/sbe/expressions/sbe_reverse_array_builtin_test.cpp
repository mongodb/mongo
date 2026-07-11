// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

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

        auto reverseArrayExpr = makeE<EFunction>(EFn::kReverseArray, makeEs(std::move(arrayExpr)));
        auto compiledExpr = compileExpression(*reverseArrayExpr);

        auto actual = runCompiledExpression(compiledExpr.get());
        value::TagValueOwned actualOwned = value::TagValueOwned::fromRaw(actual);

        auto [compareTag, compareValue] = value::compareValue(
            actualOwned.tag(), actualOwned.value(), expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }
};

TEST_F(SBEBuiltinReverseArrayTest, Array) {
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        auto testArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(testArray);

        auto expectedResult = makeArray(BSON_ARRAY(3 << 2 << 1));
        value::TagValueOwned expectedResultOwned = value::TagValueOwned::fromRaw(expectedResult);

        runAndAssertExpression(testArray, expectedResult);
    }
}

TEST_F(SBEBuiltinReverseArrayTest, ArraySet) {
    // This test needs to do a bit more work to find the correct reversed order since ArraySet's
    // internal order is determined by it's hash function and not the order that elements are added
    // to it.
    auto testArray = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(testArray);
    value::ArrayEnumerator testEnumerator{testArrayOwned.tag(), testArrayOwned.value()};

    std::vector<value::TagValueView> testArrayContents;
    auto expectedResult = value::makeNewArray();
    value::TagValueOwned expectedResultOwned = value::TagValueOwned::fromRaw(expectedResult);
    auto expectedResultView = value::getArrayView(expectedResultOwned.value());

    while (!testEnumerator.atEnd()) {
        testArrayContents.push_back(testEnumerator.getViewOfValue());
        testEnumerator.advance();
    }

    for (auto it = testArrayContents.rbegin(); it != testArrayContents.rend(); ++it) {
        auto [copyTag, copyVal] = copyValue(it->tag, it->value);
        expectedResultView->push_back_raw(copyTag, copyVal);
    }

    runAndAssertExpression(testArray, expectedResult);
}

TEST_F(SBEBuiltinReverseArrayTest, NotArray) {
    runAndAssertExpression(makeNothing(), makeNothing());
    runAndAssertExpression(makeInt32(123), makeNothing());
}

}  // namespace mongo::sbe
