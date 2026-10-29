// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
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
            EFn::kTopN, makeEs(std::move(nExpr), std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*topNExpr);

        value::TagValueOwned actualOwned =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        auto [compareTag, compareValue] = value::compareValue(
            actualOwned.tag(), actualOwned.value(), expected.first, expected.second);
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
            auto [tag, val] = bson::convertToOwned(elem).releaseToRaw();
            objView->push_back_raw(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinTopNTest, Array) {
    // Testing ArraySet gives unpredictable ordering of the result, so we only test for stable
    // arrays.
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        value::TagValueOwned testArrayOwned =
            value::TagValueOwned::fromRaw(makeArrayFn(BSON_ARRAY(1 << 2 << 3)));
        value::TagValueOwned expectedResultOwned =
            value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(3 << 2 << 1)));
        value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

        runAndAssertExpression(
            makeInt64(3), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
    }
}

TEST_F(SBEBuiltinTopNTest, NotArray) {
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(makeInt64(3), makeNothing(), sortSpecOwned.raw(), makeNothing());
    runAndAssertExpression(makeInt64(3), makeInt32(123), sortSpecOwned.raw(), makeNothing());
}

TEST_F(SBEBuiltinTopNTest, NIsZero) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned = value::TagValueOwned::fromRaw(value::makeNewArray());
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(0), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}


TEST_F(SBEBuiltinTopNTest, NegativeN) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(makeInt64(-1), testArrayOwned.raw(), sortSpecOwned.raw(), makeNothing());
}

TEST_F(SBEBuiltinTopNTest, NLargerThanArraySize) {
    // Test with n larger than array size
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(3 << 2 << 1)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(10), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinTopNTest, Int32N) {
    // Test with n as NumberInt32 instead of NumberInt64
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(3 << 2)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt32(2), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinTopNTest, MixedTypes) {
    // BSON type ordering: null < numbers < strings < objects < arrays
    value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << BSONNULL << 2)));
    value::TagValueOwned expectedResultOwned = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON("a" << 1) << "hello" << 5 << 2)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(5), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinTopNTest, ArraySet) {
    // ArraySet has unpredictable internal ordering due to its hash function, but topN should
    // still sort the elements correctly regardless of the input order.
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(3 << 2)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(2), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
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
            EFn::kBottomN, makeEs(std::move(nExpr), std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*bottomNExpr);

        value::TagValueOwned actualOwned =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        auto [compareTag, compareValue] = value::compareValue(
            actualOwned.tag(), actualOwned.value(), expected.first, expected.second);
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
            auto [tag, val] = bson::convertToOwned(elem).releaseToRaw();
            objView->push_back_raw(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinBottomNTest, Array) {
    // bottomN returns the bottom N elements (opposite of topN)
    // With descending sort (-1), bottom N means the smallest elements
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        value::TagValueOwned testArrayOwned =
            value::TagValueOwned::fromRaw(makeArrayFn(BSON_ARRAY(1 << 2 << 3)));
        value::TagValueOwned expectedResultOwned =
            value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(3 << 2 << 1)));
        value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

        runAndAssertExpression(
            makeInt64(3), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
    }
}

TEST_F(SBEBuiltinBottomNTest, NotArray) {
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(makeInt64(3), makeNothing(), sortSpecOwned.raw(), makeNothing());
    runAndAssertExpression(makeInt64(3), makeInt32(123), sortSpecOwned.raw(), makeNothing());
}

TEST_F(SBEBuiltinBottomNTest, NIsZero) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned = value::TagValueOwned::fromRaw(value::makeNewArray());
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(0), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinBottomNTest, NegativeN) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(makeInt64(-1), testArrayOwned.raw(), sortSpecOwned.raw(), makeNothing());
}

TEST_F(SBEBuiltinBottomNTest, NLargerThanArraySize) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(3 << 2 << 1)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(10), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinBottomNTest, Int32N) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(2 << 1)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt32(2), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinBottomNTest, MixedTypes) {
    // bottomN with mixed types - returns bottom 5 elements
    value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << BSONNULL << 2)));
    value::TagValueOwned expectedResultOwned = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(BSON("a" << 1) << "hello" << 5 << 2 << BSONNULL)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(5), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinBottomNTest, ArraySet) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3)));
    value::TagValueOwned expectedResultOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(2 << 1)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(
        makeInt64(2), testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
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

        auto topExpr =
            makeE<EFunction>(EFn::kTop, makeEs(std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*topExpr);

        value::TagValueOwned actualOwned =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        auto [compareTag, compareValue] = value::compareValue(
            actualOwned.tag(), actualOwned.value(), expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    TypedValue makeSortSpec() {
        auto sortSpecBson = BSON("" << -1);
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        for (auto elem : sortSpecBson) {
            auto [tag, val] = bson::convertToOwned(elem).releaseToRaw();
            objView->push_back_raw(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinTopTest, Array) {
    // top returns the first element under the sort order
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        value::TagValueOwned testArrayOwned =
            value::TagValueOwned::fromRaw(makeArrayFn(BSON_ARRAY(1 << 2 << 3)));

        auto expectedResult = makeInt64(3);

        value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

        runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), expectedResult);
    }
}

TEST_F(SBEBuiltinTopTest, NotArray) {
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(makeNothing(), sortSpecOwned.raw(), makeNothing());
    runAndAssertExpression(makeInt32(123), sortSpecOwned.raw(), makeNothing());
}

TEST_F(SBEBuiltinTopTest, EmptyArray) {
    value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(value::makeNewArray());
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), makeNull());
}

TEST_F(SBEBuiltinTopTest, MixedTypes) {
    // top with mixed types - returns the first element (largest in descending order)
    value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(makeArray(
        BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << BSONNULL << 2)));
    value::TagValueOwned expectedResultOwned =
        value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2)));
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), expectedResultOwned.raw());
}

TEST_F(SBEBuiltinTopTest, ArraySet) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3)));

    auto expectedResult = makeInt64(3);

    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), expectedResult);
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
            makeE<EFunction>(EFn::kBottom, makeEs(std::move(arrayExpr), std::move(sortByExpr)));
        auto compiledExpr = compileExpression(*bottomExpr);

        value::TagValueOwned actualOwned =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        auto [compareTag, compareValue] = value::compareValue(
            actualOwned.tag(), actualOwned.value(), expected.first, expected.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }

    TypedValue makeSortSpec() {
        auto sortSpecBson = BSON("" << -1);
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        for (auto elem : sortSpecBson) {
            auto [tag, val] = bson::convertToOwned(elem).releaseToRaw();
            objView->push_back_raw(elem.fieldNameStringData(), tag, val);
        }
        return {objTag, objVal};
    }
};

TEST_F(SBEBuiltinBottomTest, Array) {
    // bottom returns the last element under the sort order
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        value::TagValueOwned testArrayOwned =
            value::TagValueOwned::fromRaw(makeArrayFn(BSON_ARRAY(1 << 2 << 3)));

        auto expectedResult = makeInt64(1);

        value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

        runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), expectedResult);
    }
}

TEST_F(SBEBuiltinBottomTest, NotArray) {
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(makeNothing(), sortSpecOwned.raw(), makeNothing());
    runAndAssertExpression(makeInt32(123), sortSpecOwned.raw(), makeNothing());
}

TEST_F(SBEBuiltinBottomTest, EmptyArray) {
    value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(value::makeNewArray());
    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), makeNull());
}

TEST_F(SBEBuiltinBottomTest, MixedTypes) {
    // bottom with mixed types - returns the last element (smallest in descending order)
    value::TagValueOwned testArrayOwned = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(5 << "hello" << BSON("a" << 1) << BSON_ARRAY(1 << 2) << 2)));

    auto expectedResult = makeInt64(2);

    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), expectedResult);
}

TEST_F(SBEBuiltinBottomTest, ArraySet) {
    value::TagValueOwned testArrayOwned =
        value::TagValueOwned::fromRaw(makeArraySet(BSON_ARRAY(1 << 2 << 3)));

    auto expectedResult = makeInt64(1);

    value::TagValueOwned sortSpecOwned = value::TagValueOwned::fromRaw(makeSortSpec());

    runAndAssertExpression(testArrayOwned.raw(), sortSpecOwned.raw(), expectedResult);
}

}  // namespace mongo::sbe
