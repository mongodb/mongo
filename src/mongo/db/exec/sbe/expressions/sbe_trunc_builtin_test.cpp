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

#include "mongo/db/exec/sbe/expression_test_base.h"

#include <limits>

namespace mongo::sbe {

class SBETruncBuiltinTest : public EExpressionTestFixture {
protected:
    void runAndAssertExpression(value::TypeTags argumentTag,
                                value::Value argumentValue,
                                value::TypeTags expectedTag,
                                value::Value expectedValue) {
        auto [copyTag, copyValue] = value::copyValue(argumentTag, argumentValue);
        auto truncExpr =
            sbe::makeE<sbe::EFunction>("trunc", sbe::makeEs(makeE<EConstant>(copyTag, copyValue)));
        auto compiledExpr = compileExpression(*truncExpr);

        auto [actualTag, actualValue] = runCompiledExpression(compiledExpr.get());
        value::ValueGuard guard(actualTag, actualValue);

        // This workaround is needed because double NaN values are not equal to themselves.
        if (expectedTag == value::TypeTags::NumberDouble) {
            auto expectedDouble = value::bitcastTo<double>(expectedValue);
            if (std::isnan(expectedDouble)) {
                auto actualDouble = value::bitcastTo<double>(actualValue);
                ASSERT(std::isnan(actualDouble));
                return;
            }
        }

        // This workaround is needed because Decimal128 NaN values are not equal to themselves.
        if (expectedTag == value::TypeTags::NumberDecimal) {
            auto expectedDecimal = value::bitcastTo<Decimal128>(expectedValue);
            if (expectedDecimal.isNaN()) {
                auto actualDecimal = value::bitcastTo<Decimal128>(actualValue);
                ASSERT(actualDecimal.isNaN());
                return;
            }
        }

        auto [compareTag, compareValue] =
            value::compareValue(actualTag, actualValue, expectedTag, expectedValue);
        ASSERT_EQUALS(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQUALS(value::bitcastTo<int32_t>(compareValue), 0);
    }
};

TEST_F(SBETruncBuiltinTest, TestIntegers) {
    std::vector<int64_t> testCases = {1234, 0, -1234};

    for (const auto& argument : testCases) {
        runAndAssertExpression(value::TypeTags::NumberInt32,
                               value::bitcastFrom<int64_t>(argument),
                               value::TypeTags::NumberInt32,
                               value::bitcastFrom<int64_t>(argument));

        runAndAssertExpression(value::TypeTags::NumberInt64,
                               value::bitcastFrom<int64_t>(argument),
                               value::TypeTags::NumberInt64,
                               value::bitcastFrom<int64_t>(argument));
    }
}

TEST_F(SBETruncBuiltinTest, TestDouble) {
    std::vector<std::pair<double, double>> testCases = {
        {0, 0},
        {1.2, 1},
        {1.5, 1},
        {1.7, 1},
        {-1.2, -1},
        {-1.5, -1},
        {-1.7, -1},
        {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()},
        {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()},
        {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()},
        {std::numeric_limits<double>::signaling_NaN(),
         std::numeric_limits<double>::signaling_NaN()},
    };

    for (const auto& [argument, result] : testCases) {
        runAndAssertExpression(value::TypeTags::NumberDouble,
                               value::bitcastFrom<double>(argument),
                               value::TypeTags::NumberDouble,
                               value::bitcastFrom<double>(result));
    }
}

TEST_F(SBETruncBuiltinTest, TestDecimal) {
    std::vector<std::pair<Decimal128, Decimal128>> testCases = {
        {Decimal128("0"), Decimal128("0")},
        {Decimal128("1.2"), Decimal128("1")},
        {Decimal128("1.5"), Decimal128("1")},
        {Decimal128("1.7"), Decimal128("1")},
        {Decimal128("-1.2"), Decimal128("-1")},
        {Decimal128("-1.5"), Decimal128("-1")},
        {Decimal128("-1.7"), Decimal128("-1")},
        {Decimal128::kPositiveInfinity, Decimal128::kPositiveInfinity},
        {Decimal128::kNegativeInfinity, Decimal128::kNegativeInfinity},
        {Decimal128::kPositiveNaN, Decimal128::kPositiveNaN},
        {Decimal128::kNegativeNaN, Decimal128::kNegativeNaN},
    };

    for (const auto& [argument, result] : testCases) {
        auto [argumentTag, argumentValue] = value::makeCopyDecimal(argument);
        value::ValueGuard argumentGuard(argumentTag, argumentValue);

        auto [resultTag, resultValue] = value::makeCopyDecimal(result);
        value::ValueGuard resultGuard(resultTag, resultValue);

        runAndAssertExpression(argumentTag, argumentValue, resultTag, resultValue);
    }
}

}  // namespace mongo::sbe
