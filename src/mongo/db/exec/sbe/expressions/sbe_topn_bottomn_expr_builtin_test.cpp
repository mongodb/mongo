/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <memory>
#include <utility>

namespace mongo::sbe {

class SBEBuiltinTopNTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Assert that result of 'topN(n, array, sortBy)' is equal to 'expected'.
     * NOTE: Values behind arguments and the return value of this function are owned by the caller.
     */
    void runAndAssertExpression(TypedValue n,
                                TypedValue array,
                                TypedValue sortBy,
                                TypedValue expected) {
        value::ViewOfValueAccessor nSlotAccessor;
        auto nSlot = bindAccessor(&nSlotAccessor);
        nSlotAccessor.reset(n.first, n.second);
        auto nExpr = makeE<EVariable>(nSlot);
        value::ViewOfValueAccessor arraySlotAccessor;
        auto arraySlot = bindAccessor(&arraySlotAccessor);
        arraySlotAccessor.reset(array.first, array.second);
        auto arrayExpr = makeE<EVariable>(arraySlot);
        value::ViewOfValueAccessor sortBySlotAccessor;
        auto sortBySlot = bindAccessor(&sortBySlotAccessor);
        sortBySlotAccessor.reset(sortBy.first, sortBy.second);
        auto sortByExpr = makeE<EVariable>(sortBySlot);

        auto topNExpr = makeE<EFunction>(
            "topN", makeEs(std::move(nExpr), std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*topNExpr);

        auto actual = runCompiledExpression(compiledExpr.get());
        value::ValueGuard actualGuard{actual};

        auto [compareTag, compareValue] =
            value::compareValue(actual.first, actual.second, expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    /**
     * Helper to create a sort specification object for topN/bottomN.
     * Creates a simple descending sort spec (-1) to get the largest elements first.
     * For scalar arrays, the field name should be empty string "".
     */
    TypedValue makeSortSpec() {
        // Create a BSON object with an empty field name and value -1 (descending sort)
        auto sortSpecBson = BSON("" << -1);
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        for (auto elem : sortSpecBson) {
            auto [tag, val] = bson::convertFrom<false>(elem);
            objView->push_back(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinTopNTest, Array) {
    // Testing ArraySet gives unpredictable ordering of the result, so we only test for stable
    // arrays.
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        auto testArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::ValueGuard testArrayGuard{testArray};

        auto expectedResult = makeArray(BSON_ARRAY(3 << 2 << 1));
        value::ValueGuard expectedResultGuard{expectedResult};

        auto sortSpec = makeSortSpec();
        value::ValueGuard sortSpecGuard{sortSpec};

        runAndAssertExpression(makeInt64(3), testArray, sortSpec, expectedResult);
    }
}

TEST_F(SBEBuiltinTopNTest, NotArray) {
    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(3), makeNothing(), sortSpec, makeNothing());
    runAndAssertExpression(makeInt64(3), makeInt32(123), sortSpec, makeNothing());
}

TEST_F(SBEBuiltinTopNTest, NIsZero) {
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};
    auto expectedResult = value::makeNewArray();
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(0), testArray, sortSpec, expectedResult);
}


TEST_F(SBEBuiltinTopNTest, NegativeN) {
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(-1), testArray, sortSpec, makeNothing());
}

TEST_F(SBEBuiltinTopNTest, NLargerThanArraySize) {
    // Test with n larger than array size
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};
    auto expectedResult = makeArray(BSON_ARRAY(3 << 2 << 1));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(10), testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinTopNTest, Int32N) {
    // Test with n as NumberInt32 instead of NumberInt64
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};
    auto expectedResult = makeArray(BSON_ARRAY(3 << 2));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt32(2), testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinTopNTest, MixedTypes) {
    // BSON type ordering: null < numbers < strings < objects < arrays
    auto testArray = makeArray(
        BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << BSONNULL << 2));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult =
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON("a" << 1) << "hello" << 5 << 2));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(5), testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinTopNTest, ArraySet) {
    // ArraySet has unpredictable internal ordering due to its hash function, but topN should
    // still sort the elements correctly regardless of the input order.
    auto testArray = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult = makeArray(BSON_ARRAY(3 << 2));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(2), testArray, sortSpec, expectedResult);
}

class SBEBuiltinBottomNTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Assert that result of 'bottomN(n, array, sortBy)' is equal to 'expected'.
     * NOTE: Values behind arguments and the return value of this function are owned by the caller.
     */
    void runAndAssertExpression(TypedValue n,
                                TypedValue array,
                                TypedValue sortBy,
                                TypedValue expected) {
        value::ViewOfValueAccessor nSlotAccessor;
        auto nSlot = bindAccessor(&nSlotAccessor);
        nSlotAccessor.reset(n.first, n.second);
        auto nExpr = makeE<EVariable>(nSlot);
        value::ViewOfValueAccessor arraySlotAccessor;
        auto arraySlot = bindAccessor(&arraySlotAccessor);
        arraySlotAccessor.reset(array.first, array.second);
        auto arrayExpr = makeE<EVariable>(arraySlot);
        value::ViewOfValueAccessor sortBySlotAccessor;
        auto sortBySlot = bindAccessor(&sortBySlotAccessor);
        sortBySlotAccessor.reset(sortBy.first, sortBy.second);
        auto sortByExpr = makeE<EVariable>(sortBySlot);

        auto bottomNExpr = makeE<EFunction>(
            "bottomN", makeEs(std::move(nExpr), std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*bottomNExpr);

        auto actual = runCompiledExpression(compiledExpr.get());
        value::ValueGuard actualGuard{actual};

        auto [compareTag, compareValue] =
            value::compareValue(actual.first, actual.second, expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    /**
     * Helper to create a sort specification object for topN/bottomN.
     * Creates a simple descending sort spec (-1) to get the largest elements first.
     * For scalar arrays, the field name should be empty string "".
     */
    TypedValue makeSortSpec() {
        // Create a BSON object with an empty field name and value -1 (descending sort)
        auto sortSpecBson = BSON("" << -1);
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        for (auto elem : sortSpecBson) {
            auto [tag, val] = bson::convertFrom<false>(elem);
            objView->push_back(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinBottomNTest, Array) {
    // bottomN returns the bottom N elements (opposite of topN)
    // With descending sort (-1), bottom N means the smallest elements
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        auto testArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::ValueGuard testArrayGuard{testArray};

        auto expectedResult = makeArray(BSON_ARRAY(3 << 2 << 1));
        value::ValueGuard expectedResultGuard{expectedResult};

        auto sortSpec = makeSortSpec();
        value::ValueGuard sortSpecGuard{sortSpec};

        runAndAssertExpression(makeInt64(3), testArray, sortSpec, expectedResult);
    }
}

TEST_F(SBEBuiltinBottomNTest, NotArray) {
    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(3), makeNothing(), sortSpec, makeNothing());
    runAndAssertExpression(makeInt64(3), makeInt32(123), sortSpec, makeNothing());
}

TEST_F(SBEBuiltinBottomNTest, NIsZero) {
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};
    auto expectedResult = value::makeNewArray();
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(0), testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinBottomNTest, NegativeN) {
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(-1), testArray, sortSpec, makeNothing());
}

TEST_F(SBEBuiltinBottomNTest, NLargerThanArraySize) {
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};
    auto expectedResult = makeArray(BSON_ARRAY(3 << 2 << 1));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(10), testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinBottomNTest, Int32N) {
    auto testArray = makeArray(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};
    auto expectedResult = makeArray(BSON_ARRAY(2 << 1));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt32(2), testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinBottomNTest, MixedTypes) {
    // bottomN with mixed types - returns bottom 5 elements
    auto testArray = makeArray(
        BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << BSONNULL << 2));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult = makeArray(BSON_ARRAY(BSON("a" << 1) << "hello" << 5 << 2 << BSONNULL));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(5), testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinBottomNTest, ArraySet) {
    auto testArray = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult = makeArray(BSON_ARRAY(2 << 1));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeInt64(2), testArray, sortSpec, expectedResult);
}

class SBEBuiltinTopTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Assert that result of 'top(array, sortBy)' is equal to 'expected'.
     * NOTE: Values behind arguments and the return value of this function are owned by the caller.
     */
    void runAndAssertExpression(TypedValue array, TypedValue sortBy, TypedValue expected) {
        value::ViewOfValueAccessor arraySlotAccessor;
        auto arraySlot = bindAccessor(&arraySlotAccessor);
        arraySlotAccessor.reset(array.first, array.second);
        auto arrayExpr = makeE<EVariable>(arraySlot);
        value::ViewOfValueAccessor sortBySlotAccessor;
        auto sortBySlot = bindAccessor(&sortBySlotAccessor);
        sortBySlotAccessor.reset(sortBy.first, sortBy.second);
        auto sortByExpr = makeE<EVariable>(sortBySlot);

        auto topExpr = makeE<EFunction>("top", makeEs(std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*topExpr);

        auto actual = runCompiledExpression(compiledExpr.get());
        value::ValueGuard actualGuard{actual};

        auto [compareTag, compareValue] =
            value::compareValue(actual.first, actual.second, expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    TypedValue makeSortSpec() {
        auto sortSpecBson = BSON("" << -1);
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        for (auto elem : sortSpecBson) {
            auto [tag, val] = bson::convertFrom<false>(elem);
            objView->push_back(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinTopTest, Array) {
    // top returns the first element under the sort order
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        auto testArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::ValueGuard testArrayGuard{testArray};

        auto expectedResult = makeInt64(3);

        auto sortSpec = makeSortSpec();
        value::ValueGuard sortSpecGuard{sortSpec};

        runAndAssertExpression(testArray, sortSpec, expectedResult);
    }
}

TEST_F(SBEBuiltinTopTest, NotArray) {
    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeNothing(), sortSpec, makeNothing());
    runAndAssertExpression(makeInt32(123), sortSpec, makeNothing());
}

TEST_F(SBEBuiltinTopTest, EmptyArray) {
    auto testArray = value::makeNewArray();
    value::ValueGuard testArrayGuard{testArray};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(testArray, sortSpec, makeNull());
}

TEST_F(SBEBuiltinTopTest, MixedTypes) {
    // top with mixed types - returns the first element (largest in descending order)
    auto testArray = makeArray(
        BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << BSONNULL << 2));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult = makeArray(BSON_ARRAY(1 << 2));
    value::ValueGuard expectedResultGuard{expectedResult};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinTopTest, ArraySet) {
    auto testArray = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult = makeInt64(3);

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(testArray, sortSpec, expectedResult);
}

class SBEBuiltinBottomTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Assert that result of 'bottom(array, sortBy)' is equal to 'expected'.
     * NOTE: Values behind arguments and the return value of this function are owned by the caller.
     */
    void runAndAssertExpression(TypedValue array, TypedValue sortBy, TypedValue expected) {
        value::ViewOfValueAccessor arraySlotAccessor;
        auto arraySlot = bindAccessor(&arraySlotAccessor);
        arraySlotAccessor.reset(array.first, array.second);
        auto arrayExpr = makeE<EVariable>(arraySlot);
        value::ViewOfValueAccessor sortBySlotAccessor;
        auto sortBySlot = bindAccessor(&sortBySlotAccessor);
        sortBySlotAccessor.reset(sortBy.first, sortBy.second);
        auto sortByExpr = makeE<EVariable>(sortBySlot);

        auto bottomExpr =
            makeE<EFunction>("bottom", makeEs(std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*bottomExpr);

        auto actual = runCompiledExpression(compiledExpr.get());
        value::ValueGuard actualGuard{actual};

        auto [compareTag, compareValue] =
            value::compareValue(actual.first, actual.second, expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    TypedValue makeSortSpec() {
        auto sortSpecBson = BSON("" << -1);
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        for (auto elem : sortSpecBson) {
            auto [tag, val] = bson::convertFrom<false>(elem);
            objView->push_back(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinBottomTest, Array) {
    // bottom returns the last element under the sort order
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        auto testArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::ValueGuard testArrayGuard{testArray};

        auto expectedResult = makeInt64(1);

        auto sortSpec = makeSortSpec();
        value::ValueGuard sortSpecGuard{sortSpec};

        runAndAssertExpression(testArray, sortSpec, expectedResult);
    }
}

TEST_F(SBEBuiltinBottomTest, NotArray) {
    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(makeNothing(), sortSpec, makeNothing());
    runAndAssertExpression(makeInt32(123), sortSpec, makeNothing());
}

TEST_F(SBEBuiltinBottomTest, EmptyArray) {
    auto testArray = value::makeNewArray();
    value::ValueGuard testArrayGuard{testArray};

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(testArray, sortSpec, makeNull());
}

TEST_F(SBEBuiltinBottomTest, MixedTypes) {
    // bottom with mixed types - returns the last element (smallest in descending order)
    auto testArray =
        makeArray(BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << 2));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult = makeInt64(2);

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(testArray, sortSpec, expectedResult);
}

TEST_F(SBEBuiltinBottomTest, ArraySet) {
    auto testArray = makeArraySet(BSON_ARRAY(1 << 2 << 3));
    value::ValueGuard testArrayGuard{testArray};

    auto expectedResult = makeInt64(1);

    auto sortSpec = makeSortSpec();
    value::ValueGuard sortSpecGuard{sortSpec};

    runAndAssertExpression(testArray, sortSpec, expectedResult);
}

}  // namespace mongo::sbe
