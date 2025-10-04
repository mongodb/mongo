/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

class SBEFillTypeTest : public EExpressionTestFixture {};

TEST_F(SBEFillTypeTest, FillType) {
    value::ViewOfValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    value::ViewOfValueAccessor typeMaskAccessor;
    auto typeMaskSlot = bindAccessor(&typeMaskAccessor);
    value::ViewOfValueAccessor fillAccessor;
    auto fillSlot = bindAccessor(&fillAccessor);

    auto fillTypeExpr = sbe::makeE<sbe::EFunction>("fillType",
                                                   sbe::makeEs(makeE<EVariable>(inputSlot),
                                                               makeE<EVariable>(typeMaskSlot),
                                                               makeE<EVariable>(fillSlot)));
    auto compiledExpr = compileExpression(*fillTypeExpr);

    auto [fillTag, fillVal] = value::makeNewString("hello world!"_sd);
    value::ValueGuard fillGuard{fillTag, fillVal};

    {
        // Test invalid type mask.
        auto [typeMaskTag, typeMaskVal] = makeDouble(1.0);
        auto [inputTag, inputVal] = makeInt32(5);


        typeMaskAccessor.reset(typeMaskTag, typeMaskVal);
        inputAccessor.reset(inputTag, inputVal);
        fillAccessor.reset(fillTag, fillVal);

        auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
        ASSERT_EQ(runTag, value::TypeTags::Nothing);
    }

    {
        // Test valid type mask.
        auto [typeMaskTag, typeMaskVal] =
            makeInt32(getBSONTypeMask(BSONType::null) | getBSONTypeMask(BSONType::undefined));
        typeMaskAccessor.reset(typeMaskTag, typeMaskVal);

        {
            // Test with Null input.
            auto [inputTag, inputVal] = makeNull();
            inputAccessor.reset(inputTag, inputVal);

            auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard runGuard{runTag, runVal};
            ASSERT_THAT((std::pair{runTag, runVal}), ValueEq(std::pair{fillTag, fillVal}));
        }

        {
            // Test bsonUndefined input.
            value::TypeTags inputTag = value::TypeTags::bsonUndefined;
            value::Value inputVal = value::Value{0u};
            inputAccessor.reset(inputTag, inputVal);

            auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard runGuard{runTag, runVal};
            ASSERT_THAT((std::pair{runTag, runVal}), ValueEq(std::pair{fillTag, fillVal}));
        }

        {
            // Test with Nothing input.
            auto [inputTag, inputVal] = makeNothing();
            inputAccessor.reset(inputTag, inputVal);

            auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
            ASSERT_EQ(runTag, value::TypeTags::Nothing);
        }

        {
            // Test with non-Nothing input that won't match the type mask.
            auto [inputTag, inputVal] = makeArray(BSON_ARRAY(1 << 2 << 3));
            value::ValueGuard inputGuard{inputTag, inputVal};
            inputAccessor.reset(inputTag, inputVal);

            auto [runTag, runVal] = runCompiledExpression(compiledExpr.get());
            value::ValueGuard runGuard{runTag, runVal};
            ASSERT_THAT((std::pair{runTag, runVal}), ValueEq(std::pair{inputTag, inputVal}));
        }
    }
}
}  // namespace mongo::sbe
