// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_unittest.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {
using namespace std::literals::string_view_literals;

class SBEFillTypeTest : public EExpressionTestFixture {};

TEST_F(SBEFillTypeTest, FillType) {
    value::ViewOfValueAccessor inputAccessor;
    auto inputSlot = bindAccessor(&inputAccessor);
    value::ViewOfValueAccessor typeMaskAccessor;
    auto typeMaskSlot = bindAccessor(&typeMaskAccessor);
    value::ViewOfValueAccessor fillAccessor;
    auto fillSlot = bindAccessor(&fillAccessor);

    auto fillTypeExpr = sbe::makeE<sbe::EFunction>(EFn::kFillType,
                                                   sbe::makeEs(makeE<EVariable>(inputSlot),
                                                               makeE<EVariable>(typeMaskSlot),
                                                               makeE<EVariable>(fillSlot)));
    auto compiledExpr = compileExpression(*fillTypeExpr);

    value::TagValueOwned fill =
        value::TagValueOwned::fromRaw(value::makeNewString("hello world!"sv));

    {
        // Test invalid type mask.
        auto [typeMaskTag, typeMaskVal] = makeDouble(1.0);
        auto [inputTag, inputVal] = makeInt32(5);


        typeMaskAccessor.reset(typeMaskTag, typeMaskVal);
        inputAccessor.reset(inputTag, inputVal);
        fillAccessor.reset(fill.tag(), fill.value());

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

            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
            ASSERT_THAT((std::pair{result.tag(), result.value()}),
                        ValueEq(std::pair{fill.tag(), fill.value()}));
        }

        {
            // Test bsonUndefined input.
            value::TypeTags inputTag = value::TypeTags::bsonUndefined;
            value::Value inputVal = value::Value{0u};
            inputAccessor.reset(inputTag, inputVal);

            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
            ASSERT_THAT((std::pair{result.tag(), result.value()}),
                        ValueEq(std::pair{fill.tag(), fill.value()}));
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
            value::TagValueOwned input =
                value::TagValueOwned::fromRaw(makeArray(BSON_ARRAY(1 << 2 << 3)));
            inputAccessor.reset(input.tag(), input.value());

            value::TagValueOwned result =
                value::TagValueOwned::fromRaw(runCompiledExpression(compiledExpr.get()));
            ASSERT_THAT((std::pair{result.tag(), result.value()}),
                        ValueEq(std::pair{input.tag(), input.value()}));
        }
    }
}
}  // namespace mongo::sbe
