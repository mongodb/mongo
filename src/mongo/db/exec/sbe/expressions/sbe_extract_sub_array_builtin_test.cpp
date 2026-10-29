// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

class SBEBuiltinExtractSubArrayTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    struct TestCase {
        BSONArray array;
        TypedValue limit;
        boost::optional<TypedValue> skip;
        boost::optional<BSONArray> expectedResult;
    };

    void setUp() override {
        testCases = {
            // Single argument usage.
            {BSON_ARRAY(1 << 2 << 3), makeInt32(1), boost::none, BSON_ARRAY(1)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(2), boost::none, BSON_ARRAY(1 << 2)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(3), boost::none, BSON_ARRAY(1 << 2 << 3)},

            {BSON_ARRAY(1 << 2 << 3), makeInt32(-1), boost::none, BSON_ARRAY(3)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(-2), boost::none, BSON_ARRAY(2 << 3)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(-3), boost::none, BSON_ARRAY(1 << 2 << 3)},

            // Two arguments usage.
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5), makeInt32(1), makeInt32(2), BSON_ARRAY(3)},
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5), makeInt32(2), makeInt32(2), BSON_ARRAY(3 << 4)},
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5),
             makeInt32(3),
             makeInt32(2),
             BSON_ARRAY(3 << 4 << 5)},

            {BSON_ARRAY(1 << 2 << 3 << 4 << 5), makeInt32(1), makeInt32(-3), BSON_ARRAY(3)},
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5), makeInt32(2), makeInt32(-3), BSON_ARRAY(3 << 4)},
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5),
             makeInt32(3),
             makeInt32(-3),
             BSON_ARRAY(3 << 4 << 5)},

            // Zero and larger than array size skip/limit values.
            {BSON_ARRAY(1 << 2 << 3), makeInt32(0), boost::none, BSONArray()},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(4), boost::none, BSON_ARRAY(1 << 2 << 3)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(-4), boost::none, BSON_ARRAY(1 << 2 << 3)},

            {BSON_ARRAY(1 << 2 << 3 << 4 << 5), makeInt32(0), makeInt32(2), BSONArray()},
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5),
             makeInt32(4),
             makeInt32(2),
             BSON_ARRAY(3 << 4 << 5)},
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5), makeInt32(1), makeInt32(5), BSONArray()},
            {BSON_ARRAY(1 << 2 << 3 << 4 << 5), makeInt32(1), makeInt32(-6), BSON_ARRAY(1)},

            {BSON_ARRAY(1 << 2 << 3), makeInt32(10), makeInt32(0), BSON_ARRAY(1 << 2 << 3)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(10), makeInt32(-3), BSON_ARRAY(1 << 2 << 3)},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(10), makeInt32(-10), BSON_ARRAY(1 << 2 << 3)},

            // Skip and limit validation.
            {BSON_ARRAY(1 << 2 << 3), makeInt64(0), boost::none, boost::none},
            {BSON_ARRAY(1 << 2 << 3), makeInt64(1), makeInt32(0), boost::none},
            {BSON_ARRAY(1 << 2 << 3), makeInt32(-1), makeInt32(0), boost::none},
        };
    }

    /**
     * Compile and run expression 'extractSubArray(array, limit, skip)' and return its result.
     * NOTE: Values behind arguments and the return value of this function are owned by the caller.
     */
    TypedValue runExpression(TypedValue array, TypedValue limit, boost::optional<TypedValue> skip) {
        auto arguments = makeEs();

        auto arrayCopy = value::copyValue(array.first, array.second);
        arguments.push_back(makeE<EConstant>(arrayCopy.first, arrayCopy.second));

        auto limitCopy = value::copyValue(limit.first, limit.second);
        arguments.push_back(makeE<EConstant>(limitCopy.first, limitCopy.second));

        if (skip) {
            auto skipCopy = value::copyValue(skip->first, skip->second);
            arguments.push_back(makeE<EConstant>(skipCopy.first, skipCopy.second));
        }

        auto extractSubArrayExpr = makeE<EFunction>(EFn::kExtractSubArray, std::move(arguments));
        auto compiledExpr = compileExpression(*extractSubArrayExpr);

        return runCompiledExpression(compiledExpr.get());
    }

    /**
     * Assert that result of 'extractSubArray(array, limit, skip)' is equal to the expected result.
     * NOTE: Values behind arguments of this function are owned by the caller.
     */
    template <typename T>
    void runAndAssertExpression(const TestCase& testCase, T makeArrayFn) {
        auto array = makeArrayFn(testCase.array);
        value::TagValueOwned arrayOwned = value::TagValueOwned::fromRaw(array);

        auto expectedResult =
            testCase.expectedResult ? makeArrayFn(*testCase.expectedResult) : makeNothing();
        value::TagValueOwned expectedResultOwned = value::TagValueOwned::fromRaw(expectedResult);

        auto actualResult = runExpression(array, testCase.limit, testCase.skip);
        value::TagValueOwned actualResultOwned = value::TagValueOwned::fromRaw(actualResult);

        auto [compareTag, compareValue] = value::compareValue(
            actualResult.first, actualResult.second, expectedResult.first, expectedResult.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    std::vector<TestCase> testCases;
};

TEST_F(SBEBuiltinExtractSubArrayTest, Array) {
    for (const auto& testCase : testCases) {
        runAndAssertExpression(testCase, makeArray);
    }
}

TEST_F(SBEBuiltinExtractSubArrayTest, BSONArray) {
    for (const auto& testCase : testCases) {
        runAndAssertExpression(testCase, makeBsonArray);
    }
}

TEST_F(SBEBuiltinExtractSubArrayTest, ArraySetNothing) {
    for (const auto& testCase : testCases) {
        if (testCase.expectedResult) {
            continue;
        }
        runAndAssertExpression(testCase, makeArraySet);
    }
}

TEST_F(SBEBuiltinExtractSubArrayTest, ArraySet) {
    auto array = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::TagValueOwned arrayOwned = value::TagValueOwned::fromRaw(array);

    const std::vector<std::pair<TypedValue, boost::optional<TypedValue>>> limitAndSkip = {
        {makeInt32(3), boost::none},
        {makeInt32(-3), boost::none},
        {makeInt32(3), makeInt32(0)},
        {makeInt32(3), makeInt32(-3)},
        {makeInt32(10), makeInt32(0)},
        {makeInt32(10), makeInt32(-3)},
        {makeInt32(10), makeInt32(-10)},
    };
    for (const auto& [limit, skip] : limitAndSkip) {
        value::TagValueOwned result =
            value::TagValueOwned::fromRaw(runExpression(array, limit, skip));

        std::vector<int32_t> elements;
        value::ArrayEnumerator enumerator{result.tag(), result.value()};
        while (!enumerator.atEnd()) {
            auto [tag, value] = enumerator.getViewOfValue();
            ASSERT_EQ(tag, value::TypeTags::NumberInt32);
            elements.push_back(value::bitcastTo<int32_t>(value));
            enumerator.advance();
        }

        std::sort(elements.begin(), elements.end());
        ASSERT_EQ(elements[0], 1);
        ASSERT_EQ(elements[1], 2);
        ASSERT_EQ(elements[2], 3);
    }
}

TEST_F(SBEBuiltinExtractSubArrayTest, NotArray) {
    std::vector<TypedValue> notArrayTestCases = {
        makeNothing(),
        makeInt32(123),
    };
    for (const auto& testCase : notArrayTestCases) {
        auto [tag, value] = runExpression(testCase, makeInt32(1), boost::none);
        ASSERT_EQ(tag, value::TypeTags::Nothing);
        ASSERT_EQ(value, value::bitcastFrom<int64_t>(0));
    }
}

TEST_F(SBEBuiltinExtractSubArrayTest, MemoryManagement) {
    {
        auto array = makeArray(BSON_ARRAY("Item#1" << "Item#2"
                                                   << "Item#3"
                                                   << "Item#4"));

        // Use 'extractSubArray' to create a stack owned array and extract object from it, then test
        // if 'getElement' can return the value with correct memory management.
        auto extractFromSubArrayExpr = makeE<EFunction>(
            EFn::kGetElement,
            makeEs(makeE<EFunction>(EFn::kExtractSubArray,
                                    makeEs(makeC(array),
                                           makeC(value::TypeTags::NumberInt32, 1),
                                           makeC(value::TypeTags::NumberInt32, 2))),
                   makeC(value::TypeTags::NumberInt32, 0)));

        auto compiledExpr = compileExpression(*extractFromSubArrayExpr);

        value::TagValueOwned actual =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
        ASSERT_TRUE(value::isString(actual.tag()));
        ASSERT_EQ("Item#3", value::getStringView(actual.tag(), actual.value()));
    }
    {
        const auto [objTag, objVal] = value::makeNewObject();
        auto obj = value::getObjectView(objVal);

        const auto [fieldTag, fieldVal] = value::makeNewString("not so small string"sv);
        ASSERT_EQ(value::TypeTags::StringBig, fieldTag);
        obj->push_back_raw("field"sv, fieldTag, fieldVal);
        const auto [arrTag, arrVal] = value::makeNewArray();
        auto arr = value::getArrayView(arrVal);
        arr->push_back_raw(objTag, objVal);

        // Use 'extractSubArray' to create a stack owned array and extract object from it, then test
        // if 'getField' can return the value with correct memory management.
        auto extractFromSubArrayExpr = makeE<EFunction>(
            EFn::kGetField,
            makeEs(makeE<EFunction>(
                       EFn::kGetElement,
                       makeEs(makeE<EFunction>(EFn::kExtractSubArray,
                                               makeEs(makeC(arrTag, arrVal),
                                                      makeC(value::TypeTags::NumberInt32, 1))),
                              makeC(value::TypeTags::NumberInt32, 0))),
                   makeC(value::makeNewString("field"sv))));

        auto compiledExpr = compileExpression(*extractFromSubArrayExpr);

        value::TagValueOwned actual =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
        ASSERT_TRUE(value::isString(actual.tag()));
        ASSERT_EQ("not so small string"sv, value::getStringView(actual.tag(), actual.value()));
    }
}
}  // namespace mongo::sbe
