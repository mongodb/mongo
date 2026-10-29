// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <memory>
#include <utility>

namespace mongo::sbe {

class SBEBuiltinUnwindArrayTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Assert that the result of 'unwindArray(array)' is equal to 'expectedRes'.
     * NOTE: Values behind arguments of this function are owned by the caller.
     */
    void runAndAssertExpression(TypedValue array, TypedValue expectedRes) {
        auto [arrayCopyType, arrayCopyValue] = value::copyValue(array.first, array.second);
        std::unique_ptr<EExpression> arrayExpr = makeE<EConstant>(arrayCopyType, arrayCopyValue);

        std::unique_ptr<EExpression> unwoundExpr =
            makeE<EFunction>(EFn::kUnwindArray, makeEs(std::move(arrayExpr)));
        std::unique_ptr<vm::CodeFragment> compiledExpr = compileExpression(*unwoundExpr);

        value::TagValueOwned actualValue =
            value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));

        auto [compareTag, compareValue] = value::compareValue(
            actualValue.tag(), actualValue.value(), expectedRes.first, expectedRes.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0)
            << TypedValue{actualValue.tag(), actualValue.value()} << " != " << expectedRes;
    }
};

/**
 * Test the behaviour of 'unwindArray' with an array containing both scalar values, empty array and
 * nested arrays. Test all the variants of arrays that SBE supports.
 */
TEST_F(SBEBuiltinUnwindArrayTest, Array) {
    BSONArray source = BSON_ARRAY(1 << BSON_ARRAY("long long string" << 900) << 3 << BSONArray()
                                    << BSON_ARRAY(34.3 << BSON_ARRAY(12)));
    value::TagValueOwned expected = value::TagValueOwned::fromRaw(
        makeArray(BSON_ARRAY(1 << "long long string" << 900 << 3 << 34.3 << BSON_ARRAY(12))));
    // Testing ArraySet gives unpredictable ordering of the result, so we only test for stable
    // arrays.
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        value::TagValueOwned someArray = value::TagValueOwned::fromRaw(makeArrayFn(source));

        runAndAssertExpression({someArray.tag(), someArray.value()},
                               {expected.tag(), expected.value()});
    }
}

/**
 * Test the behaviour of 'unwindArray' with an empty array.
 */
TEST_F(SBEBuiltinUnwindArrayTest, EmptyArray) {
    for (auto makeArrayFn : {makeBsonArray, makeArray, makeArraySet}) {
        value::TagValueOwned emptyArray = value::TagValueOwned::fromRaw(makeArrayFn(BSONArray()));

        runAndAssertExpression({emptyArray.tag(), emptyArray.value()}, makeNothing());
    }
}

/**
 * Test the behaviour of 'unwindArray' with an invalid argument.
 */
TEST_F(SBEBuiltinUnwindArrayTest, NotArray) {
    runAndAssertExpression(makeNothing(), makeNothing());
    runAndAssertExpression(makeInt32(123), makeNothing());
}

}  // namespace mongo::sbe
