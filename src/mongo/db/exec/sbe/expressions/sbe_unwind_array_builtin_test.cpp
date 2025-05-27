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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
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
            makeE<EFunction>("unwindArray", makeEs(std::move(arrayExpr)));
        std::unique_ptr<vm::CodeFragment> compiledExpr = compileExpression(*unwoundExpr);

        TypedValue actualValue = runCompiledExpression(compiledExpr.get());
        value::ValueGuard actualValueGuard{actualValue};

        auto [compareTag, compareValue] = value::compareValue(
            actualValue.first, actualValue.second, expectedRes.first, expectedRes.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0) << actualValue << " != " << expectedRes;
    }
};

/**
 * Test the behaviour of 'unwindArray' with an array containing both scalar values, empty array and
 * nested arrays. Test all the variants of arrays that SBE supports.
 */
TEST_F(SBEBuiltinUnwindArrayTest, Array) {
    BSONArray source = BSON_ARRAY(1 << BSON_ARRAY("long long string" << 900) << 3 << BSONArray()
                                    << BSON_ARRAY(34.3 << BSON_ARRAY(12)));
    TypedValue expected =
        makeArray(BSON_ARRAY(1 << "long long string" << 900 << 3 << 34.3 << BSON_ARRAY(12)));
    value::ValueGuard guardExpected{expected};
    // Testing ArraySet gives unpredictable ordering of the result, so we only test for stable
    // arrays.
    for (auto makeArrayFn : {makeBsonArray, makeArray}) {
        TypedValue someArray = makeArrayFn(source);
        value::ValueGuard guard{someArray};

        runAndAssertExpression(someArray, expected);
    }
}

/**
 * Test the behaviour of 'unwindArray' with an empty array.
 */
TEST_F(SBEBuiltinUnwindArrayTest, EmptyArray) {
    for (auto makeArrayFn : {makeBsonArray, makeArray, makeArraySet}) {
        TypedValue emptyArray = makeArrayFn(BSONArray());
        value::ValueGuard emptyArrayGuard{emptyArray};

        runAndAssertExpression(emptyArray, makeNothing());
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
