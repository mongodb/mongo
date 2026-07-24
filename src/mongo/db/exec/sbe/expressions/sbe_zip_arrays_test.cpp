// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo::sbe {

/**
 * Tests for the zipArrays builtin, which backs $zip. The calling convention is
 * zipArrays(numInputs, useLongestLength, input0..inputN-1[, defaultsArray]) where the optional
 * trailing argument evaluates to the whole defaults array (SERVER-109615).
 */
class SBEBuiltinZipArraysTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    /**
     * Compiles and runs 'zipArrays(inputs.size(), useLongestLength, inputs..., defaults?)'.
     * Values behind the arguments and the return value are owned by the caller.
     */
    TypedValue runExpression(const std::vector<TypedValue>& inputs,
                             bool useLongestLength,
                             boost::optional<TypedValue> defaults) {
        auto arguments = makeEs();
        arguments.push_back(makeE<EConstant>(value::TypeTags::NumberInt32,
                                             value::bitcastFrom<int32_t>(inputs.size())));
        arguments.push_back(
            makeE<EConstant>(value::TypeTags::Boolean, value::bitcastFrom<bool>(useLongestLength)));
        for (const auto& input : inputs) {
            auto inputCopy = value::copyValue(input.first, input.second);
            arguments.push_back(makeE<EConstant>(inputCopy.first, inputCopy.second));
        }
        if (defaults) {
            auto defaultsCopy = value::copyValue(defaults->first, defaults->second);
            arguments.push_back(makeE<EConstant>(defaultsCopy.first, defaultsCopy.second));
        }

        auto zipExpr = makeE<EFunction>(EFn::kZipArrays, std::move(arguments));
        auto compiledExpr = compileExpression(*zipExpr);

        return runCompiledExpression(compiledExpr.get());
    }

    /** Asserts that the zip of 'inputs' with 'defaults' produces 'expected'. */
    void runAndAssertExpression(const std::vector<BSONArray>& inputs,
                                bool useLongestLength,
                                boost::optional<TypedValue> defaults,
                                const BSONArray& expected) {
        std::vector<TypedValue> inputValues;
        std::vector<value::TagValueOwned> inputGuards;
        for (const auto& input : inputs) {
            inputValues.push_back(makeArray(input));
            inputGuards.push_back(value::TagValueOwned::fromRaw(inputValues.back()));
        }
        boost::optional<value::TagValueOwned> defaultsGuard;
        if (defaults) {
            defaultsGuard.emplace(value::TagValueOwned::fromRaw(*defaults));
        }

        auto expectedResult = makeArray(expected);
        value::TagValueOwned expectedGuard = value::TagValueOwned::fromRaw(expectedResult);

        auto actualResult = runExpression(inputValues, useLongestLength, defaults);
        value::TagValueOwned actualGuard = value::TagValueOwned::fromRaw(actualResult);

        auto [compareTag, compareValue] = value::compareValue(
            actualResult.first, actualResult.second, expectedResult.first, expectedResult.second);
        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);
    }
};

TEST_F(SBEBuiltinZipArraysTest, ZipWithoutDefaults) {
    runAndAssertExpression({BSON_ARRAY(1 << 2 << 3), BSON_ARRAY("A" << "B")},
                           false /* useLongestLength */,
                           boost::none,
                           BSON_ARRAY(BSON_ARRAY(1 << "A") << BSON_ARRAY(2 << "B")));
}

TEST_F(SBEBuiltinZipArraysTest, ZipLongestLengthWithoutDefaultsPadsWithNulls) {
    runAndAssertExpression(
        {BSON_ARRAY(1 << 2 << 3), BSON_ARRAY("A" << "B")},
        true /* useLongestLength */,
        boost::none,
        BSON_ARRAY(BSON_ARRAY(1 << "A") << BSON_ARRAY(2 << "B") << BSON_ARRAY(3 << BSONNULL)));
}

TEST_F(SBEBuiltinZipArraysTest, ZipWithWholeArrayDefaults) {
    runAndAssertExpression(
        {BSON_ARRAY(1 << 2 << 3), BSON_ARRAY("A" << "B")},
        true /* useLongestLength */,
        makeArray(BSON_ARRAY("C" << "D")),
        BSON_ARRAY(BSON_ARRAY(1 << "A") << BSON_ARRAY(2 << "B") << BSON_ARRAY(3 << "D")));
}

TEST_F(SBEBuiltinZipArraysTest, ZipWithBsonArrayDefaults) {
    runAndAssertExpression(
        {BSON_ARRAY(1 << 2 << 3), BSON_ARRAY("A" << "B")},
        true /* useLongestLength */,
        makeBsonArray(BSON_ARRAY("C" << "D")),
        BSON_ARRAY(BSON_ARRAY(1 << "A") << BSON_ARRAY(2 << "B") << BSON_ARRAY(3 << "D")));
}

TEST_F(SBEBuiltinZipArraysTest, NullishDefaultsBehaveAsAbsent) {
    runAndAssertExpression(
        {BSON_ARRAY(1 << 2 << 3), BSON_ARRAY("A" << "B")},
        true /* useLongestLength */,
        makeNothing(),
        BSON_ARRAY(BSON_ARRAY(1 << "A") << BSON_ARRAY(2 << "B") << BSON_ARRAY(3 << BSONNULL)));
    runAndAssertExpression(
        {BSON_ARRAY(1 << 2 << 3), BSON_ARRAY("A" << "B")},
        true /* useLongestLength */,
        TypedValue{value::TypeTags::Null, 0},
        BSON_ARRAY(BSON_ARRAY(1 << "A") << BSON_ARRAY(2 << "B") << BSON_ARRAY(3 << BSONNULL)));
}

TEST_F(SBEBuiltinZipArraysTest, NonArrayDefaultsThrows) {
    auto input1 = makeArray(BSON_ARRAY(1 << 2));
    value::TagValueOwned input1Guard = value::TagValueOwned::fromRaw(input1);
    auto input2 = makeArray(BSON_ARRAY(1));
    value::TagValueOwned input2Guard = value::TagValueOwned::fromRaw(input2);

    ASSERT_THROWS_CODE(
        runExpression({input1, input2},
                      true /* useLongestLength */,
                      TypedValue{value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(42)}),
        DBException,
        10961502);
}

TEST_F(SBEBuiltinZipArraysTest, WrongLengthDefaultsThrows) {
    auto input1 = makeArray(BSON_ARRAY(1 << 2));
    value::TagValueOwned input1Guard = value::TagValueOwned::fromRaw(input1);
    auto input2 = makeArray(BSON_ARRAY(1));
    value::TagValueOwned input2Guard = value::TagValueOwned::fromRaw(input2);
    auto defaults = makeArray(BSON_ARRAY("C"));
    value::TagValueOwned defaultsGuard = value::TagValueOwned::fromRaw(defaults);

    ASSERT_THROWS_CODE(runExpression({input1, input2}, true /* useLongestLength */, defaults),
                       DBException,
                       10961503);
}

TEST_F(SBEBuiltinZipArraysTest, NonArrayInputReturnsNothing) {
    auto input1 = makeArray(BSON_ARRAY(1 << 2));
    value::TagValueOwned input1Guard = value::TagValueOwned::fromRaw(input1);

    auto result = runExpression(
        {input1, TypedValue{value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)}},
        false /* useLongestLength */,
        boost::none);
    value::TagValueOwned resultGuard = value::TagValueOwned::fromRaw(result);

    ASSERT_EQ(result.first, value::TypeTags::Nothing);
}

}  // namespace mongo::sbe
