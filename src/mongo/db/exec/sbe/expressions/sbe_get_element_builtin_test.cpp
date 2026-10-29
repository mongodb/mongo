// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {

class SBEBuiltinGetElementTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    struct TestCase {
        BSONArray array;
        TypedValue index;
        TypedValue expected;
    };

    void setUp() override {
        decimalValue = value::makeCopyDecimal(Decimal128("1.2345"));
        testCases = {
            // Positive indexes.
            {BSON_ARRAY(1 << 2 << 3), makeInt32(0), makeInt32(1)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(1), makeInt32(2)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(2), makeInt32(3)},

            // Negative indexes.
            {BSON_ARRAY(1 << 2 << 3), makeInt32(-1), makeInt32(3)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(-2), makeInt32(2)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(-3), makeInt32(1)},

            // Index out of bounds.
            {BSON_ARRAY(1 << 2), makeInt32(2), makeNothing()},
            {BSON_ARRAY(1 << 2), makeInt32(-3), makeNothing()},
            {BSONArray(), makeInt32(0), makeNothing()},
            {BSONArray(), makeInt32(-1), makeNothing()},

            // Invalid index type.
            {BSON_ARRAY(1 << 2), makeNothing(), makeNothing()},
            {BSON_ARRAY(1 << 2), decimalValue, makeNothing()},
            {BSON_ARRAY(1 << 2), makeDouble(1.2345), makeNothing()},
            {BSON_ARRAY(1 << 2), makeInt64(0), makeNothing()},
        };
    }

    void tearDown() override {
        value::releaseValue(decimalValue.first, decimalValue.second);
    }

    /**
     * Compile and run expression 'getElement(array, index)' and return its result.
     * NOTE: Values behind arguments and the return value of this function are owned by the caller.
     */
    TypedValue runExpression(TypedValue array, TypedValue index) {
        // We do not copy array value on purpose. During the copy, order of elements in ArraySet
        // may change. 'getElement' return value depends on the order of elements in the input
        // array. After copy 'getElement' may return different element from the expected by the
        // caller.
        value::ViewOfValueAccessor arraySlotAccessor;
        auto arraySlot = bindAccessor(&arraySlotAccessor);
        arraySlotAccessor.reset(array.first, array.second);
        auto arrayExpr = makeE<EVariable>(arraySlot);

        auto indexCopy = value::copyValue(index.first, index.second);
        auto indexExpr = makeE<EConstant>(indexCopy.first, indexCopy.second);

        auto getElementExpr =
            makeE<EFunction>(EFn::kGetElement, makeEs(std::move(arrayExpr), std::move(indexExpr)));
        auto compiledExpr = compileExpression(*getElementExpr);

        return runCompiledExpression(compiledExpr.get());
    }

    /**
     * Assert that result of 'getElement(array, index)' is equal to 'expectedRes'.
     */
    void runAndAssertExpression(TypedValue array, TypedValue index, TypedValue expectedRes) {
        auto actualValue = runExpression(array, index);
        value::TagValueOwned actualValueOwner = value::TagValueOwned::fromRaw(actualValue);

        auto [compareTag, compareValue] = value::compareValue(
            actualValue.first, actualValue.second, expectedRes.first, expectedRes.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    TypedValue decimalValue;
    std::vector<TestCase> testCases;
};

TEST_F(SBEBuiltinGetElementTest, GetElementBSONArray) {
    for (const auto& testCase : testCases) {
        auto bsonArray = makeBsonArray(testCase.array);
        value::TagValueOwned bsonArrayOwner = value::TagValueOwned::fromRaw(bsonArray);
        runAndAssertExpression(bsonArray, testCase.index, testCase.expected);
    }
}

TEST_F(SBEBuiltinGetElementTest, GetElementArray) {
    for (const auto& testCase : testCases) {
        auto array = makeArray(testCase.array);
        value::TagValueOwned arrayOwner = value::TagValueOwned::fromRaw(array);
        runAndAssertExpression(array, testCase.index, testCase.expected);
    }
}

TEST_F(SBEBuiltinGetElementTest, GetElementArraySetNothing) {
    // Run test cases when 'getElement' returns Nothing.
    for (const auto& testCase : testCases) {
        if (testCase.expected.first != value::TypeTags::Nothing) {
            continue;
        }

        auto array = makeArraySet(testCase.array);
        value::TagValueOwned arrayOwner = value::TagValueOwned::fromRaw(array);
        runAndAssertExpression(array, testCase.index, testCase.expected);
    }
}

TEST_F(SBEBuiltinGetElementTest, GetElementArraySetElements) {
    auto array = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::TagValueOwned arrayOwner = value::TagValueOwned::fromRaw(array);

    const std::vector<std::pair<int32_t, int32_t>> indices = {{-3, -1}, {0, 2}};
    for (const auto& [begin, end] : indices) {
        std::vector<int32_t> elements;
        for (int32_t i = begin; i <= end; ++i) {
            auto result = runExpression(array, makeInt32(i));
            value::TagValueOwned resultOwner = value::TagValueOwned::fromRaw(result);
            ASSERT_EQ(result.first, value::TypeTags::NumberInt32);
            elements.push_back(value::bitcastTo<int32_t>(result.second));
        }

        std::sort(elements.begin(), elements.end());
        ASSERT_EQ(elements[0], 1);
        ASSERT_EQ(elements[1], 2);
        ASSERT_EQ(elements[2], 3);
    }
}

TEST_F(SBEBuiltinGetElementTest, GetElementNotArray) {
    runAndAssertExpression(makeNothing(), makeInt32(1), makeNothing());
    runAndAssertExpression(makeInt32(123), makeInt32(1), makeNothing());
}

}  // namespace mongo::sbe
