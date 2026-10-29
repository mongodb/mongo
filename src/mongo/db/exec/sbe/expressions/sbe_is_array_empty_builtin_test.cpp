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

        auto isArrayEmptyExpr = makeE<EFunction>(EFn::kIsArrayEmpty, makeEs(std::move(arrayExpr)));
        auto compiledExpr = compileExpression(*isArrayEmptyExpr);

        auto actualValue = runCompiledExpression(compiledExpr.get());
        value::TagValueOwned actualValueOwned = value::TagValueOwned::fromRaw(actualValue);

        auto [compareTag, compareValue] = value::compareValue(
            actualValue.first, actualValue.second, expectedRes.first, expectedRes.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }
};

TEST_F(SBEBuiltinIsArrayEmptyTest, Array) {
    for (auto makeArrayFn : {makeBsonArray, makeArray, makeArraySet}) {
        auto emptyArray = makeArrayFn(BSONArray());
        value::TagValueOwned emptyArrayOwned = value::TagValueOwned::fromRaw(emptyArray);

        runAndAssertExpression(emptyArray, makeBool(true));

        auto notEmptyArray = makeArrayFn(BSON_ARRAY(1 << 2 << 3));
        value::TagValueOwned notEmptyArrayOwned = value::TagValueOwned::fromRaw(notEmptyArray);

        runAndAssertExpression(notEmptyArray, makeBool(false));
    }
}

TEST_F(SBEBuiltinIsArrayEmptyTest, NotArray) {
    runAndAssertExpression(makeNothing(), makeNothing());
    runAndAssertExpression(makeInt32(123), makeNothing());
}

}  // namespace mongo::sbe
