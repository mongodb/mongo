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
#include "mongo/db/exec/sbe/accumulator_sum_value_enum.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

namespace mongo::sbe {

enum class DerivativeOp { kAdd, kRemove };

class SBEDerivativeTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(boost::optional<int64_t> unitMillis,
                                std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                                std::vector<std::pair<value::TypeTags, value::Value>>& sortByValues,
                                std::vector<DerivativeOp>& operations,
                                std::vector<std::pair<value::TypeTags, value::Value>>& expValues) {
        value::ViewOfValueAccessor inputAccessorFirst;
        auto inputFirstSlot = bindAccessor(&inputAccessorFirst);

        value::ViewOfValueAccessor sortByAccessorFirst;
        auto sortByFirstSlot = bindAccessor(&sortByAccessorFirst);

        value::ViewOfValueAccessor inputAccessorLast;
        auto inputLastSlot = bindAccessor(&inputAccessorLast);

        value::ViewOfValueAccessor sortByAccessorLast;
        auto sortByLastSlot = bindAccessor(&sortByAccessorLast);

        auto unitMillisConst = unitMillis
            ? sbe::makeE<sbe::EConstant>(value::TypeTags::NumberInt64,
                                         value::bitcastFrom<int64_t>(*unitMillis))
            : sbe::makeE<sbe::EConstant>(value::TypeTags::Null, 0);

        auto aggDerivativeFinalize =
            sbe::makeE<sbe::EFunction>("aggDerivativeFinalize",
                                       sbe::makeEs(std::move(unitMillisConst),
                                                   makeE<EVariable>(inputFirstSlot),
                                                   makeE<EVariable>(sortByFirstSlot),
                                                   makeE<EVariable>(inputLastSlot),
                                                   makeE<EVariable>(sortByLastSlot)));
        auto compiledDerivativeFinalize = compileExpression(*aggDerivativeFinalize);

        // call DerivativeOp (derivativeAdd/Remove) on the inputs and call finalize() method after
        // each DerivativeOp
        size_t firstIdx = 0;
        size_t lastIdx = -1;
        for (size_t i = 0; i < operations.size(); ++i) {
            if (operations[i] == DerivativeOp::kAdd) {
                lastIdx++;
            } else {
                firstIdx++;
            }
            if (firstIdx <= lastIdx) {
                inputAccessorFirst.reset(inputValues[firstIdx].first, inputValues[firstIdx].second);
                sortByAccessorFirst.reset(sortByValues[firstIdx].first,
                                          sortByValues[firstIdx].second);
                inputAccessorLast.reset(inputValues[lastIdx].first, inputValues[lastIdx].second);
                sortByAccessorLast.reset(sortByValues[lastIdx].first, sortByValues[lastIdx].second);
            } else {
                inputAccessorFirst.reset();
                sortByAccessorFirst.reset();
                inputAccessorLast.reset();
                sortByAccessorLast.reset();
            }

            auto out = runCompiledExpression(compiledDerivativeFinalize.get());

            ASSERT_EQ(out.first, expValues[i].first);
            ASSERT_THAT(out, ValueEq(expValues[i]));

            value::releaseValue(out.first, out.second);
            value::releaseValue(expValues[i].first, expValues[i].second);
        }
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
            value::releaseValue(sortByValues[i].first, sortByValues[i].second);
        }
    }

    void runAndAssertErrorCode(boost::optional<int64_t> unitMillis,
                               std::vector<std::pair<value::TypeTags, value::Value>>& inputValues,
                               std::vector<std::pair<value::TypeTags, value::Value>>& sortByValues,
                               std::vector<DerivativeOp>& operations,
                               int expErrCode) {
        value::ViewOfValueAccessor inputAccessorFirst;
        auto inputFirstSlot = bindAccessor(&inputAccessorFirst);

        value::ViewOfValueAccessor sortByAccessorFirst;
        auto sortByFirstSlot = bindAccessor(&sortByAccessorFirst);

        value::ViewOfValueAccessor inputAccessorLast;
        auto inputLastSlot = bindAccessor(&inputAccessorLast);

        value::ViewOfValueAccessor sortByAccessorLast;
        auto sortByLastSlot = bindAccessor(&sortByAccessorLast);

        auto unitMillisConst = unitMillis
            ? sbe::makeE<sbe::EConstant>(value::TypeTags::NumberInt64,
                                         value::bitcastFrom<int64_t>(*unitMillis))
            : sbe::makeE<sbe::EConstant>(value::TypeTags::Null, 0);

        auto aggDerivativeFinalize =
            sbe::makeE<sbe::EFunction>("aggDerivativeFinalize",
                                       sbe::makeEs(std::move(unitMillisConst),
                                                   makeE<EVariable>(inputFirstSlot),
                                                   makeE<EVariable>(sortByFirstSlot),
                                                   makeE<EVariable>(inputLastSlot),
                                                   makeE<EVariable>(sortByLastSlot)));
        auto compiledDerivativeFinalize = compileExpression(*aggDerivativeFinalize);

        Status status = [&]() {
            try {
                size_t firstIdx = 0;
                size_t lastIdx = -1;
                for (size_t i = 0; i < operations.size(); ++i) {
                    if (operations[i] == DerivativeOp::kAdd) {
                        lastIdx++;
                    } else {
                        firstIdx++;
                    }
                    if (firstIdx <= lastIdx) {
                        inputAccessorFirst.reset(inputValues[firstIdx].first,
                                                 inputValues[firstIdx].second);
                        sortByAccessorFirst.reset(sortByValues[firstIdx].first,
                                                  sortByValues[firstIdx].second);
                        inputAccessorLast.reset(inputValues[lastIdx].first,
                                                inputValues[lastIdx].second);
                        sortByAccessorLast.reset(sortByValues[lastIdx].first,
                                                 sortByValues[lastIdx].second);
                    } else {
                        inputAccessorFirst.reset();
                        sortByAccessorFirst.reset();
                        inputAccessorLast.reset();
                        sortByAccessorLast.reset();
                    }

                    auto out = runCompiledExpression(compiledDerivativeFinalize.get());
                    value::releaseValue(out.first, out.second);
                }
                return Status::OK();
            } catch (AssertionException& ex) {
                return ex.toStatus();
            }
        }();
        ASSERT_FALSE(status.isOK());
        ASSERT_EQ(status.code(), expErrCode);
        for (size_t i = 0; i < inputValues.size(); ++i) {
            value::releaseValue(inputValues[i].first, inputValues[i].second);
            value::releaseValue(sortByValues[i].first, sortByValues[i].second);
        }
    }
};

TEST_F(SBEDerivativeTest, DerivatedSortedByDate) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(8)}};
    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::Date, 1589811030000LL},
        {value::TypeTags::Date, 1589811060000LL},
        {value::TypeTags::Date, 1589811090000LL},
        {value::TypeTags::Date, 1589811120000LL}};

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove};
    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(120.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(180.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(280.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(360.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(480.0)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0}};

    boost::optional<int64_t> unitMillis = 60LL * 60LL * 1000LL;  // hour unit
    runAndAssertExpression(unitMillis, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivativeWithMixedNumericTypes) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-20ll)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-40.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-50.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-60ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-70)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{4.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(6ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(7)},
    };

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-10.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-10.0)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(boost::none, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivatedWithDateInputType) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::Date, 1589811030000LL},
        {value::TypeTags::Date, 1589811060000LL},
        {value::TypeTags::Date, 1589811090000LL}};
    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)}};

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove};
    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(30000.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20000.0)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(15000.0)},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0}};

    runAndAssertExpression(boost::none, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivativeWithNaNAndInfinityValues) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt64, 10},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kNegativeNaN).second},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberDecimal,
         value::makeCopyDecimal(Decimal128::kNegativeInfinity).second},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(4)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(5)},
    };

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove};

    std::vector<std::pair<value::TypeTags, value::Value>> expValues = {
        {value::TypeTags::Null, 0},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kNegativeNaN).second},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberDecimal,
         value::makeCopyDecimal(Decimal128::kNegativeInfinity).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kPositiveNaN).second},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kPositiveNaN).second},
        {value::TypeTags::NumberDecimal,
         value::makeCopyDecimal(Decimal128::kNegativeInfinity).second},
        {value::TypeTags::Null, 0},
        {value::TypeTags::Null, 0},
    };

    runAndAssertExpression(boost::none, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivativeWithMixOfNumericAndDateTypeInput) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::Date, 1589811030000LL},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
    };

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd, DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7821012);
}

TEST_F(SBEDerivativeTest, DerivativeWithSortByDatesAndNoUnit) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.95)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.98)}};
    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::Date, 1589811030000LL}, {value::TypeTags::Date, 1589811060000LL}};

    std::vector<DerivativeOp> derivativeOps = {
        DerivativeOp::kAdd, DerivativeOp::kAdd, DerivativeOp::kRemove, DerivativeOp::kRemove};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7993410);
}

TEST_F(SBEDerivativeTest, DerivativeWithSortByNumbersAndDateUnit) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
    };

    std::vector<DerivativeOp> derivativeOps = {
        DerivativeOp::kAdd, DerivativeOp::kAdd, DerivativeOp::kRemove, DerivativeOp::kRemove};

    boost::optional<int64_t> unitMillis = 60LL * 60LL * 1000LL;
    runAndAssertErrorCode(unitMillis, inputValues, sortByValues, derivativeOps, 7993409);
}

TEST_F(SBEDerivativeTest, DerivativeWithIncorrectTypes1) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
    };

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7821012);
}

TEST_F(SBEDerivativeTest, DerivativeWithIncorrectTypes2) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
    };

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7993410);
}

TEST_F(SBEDerivativeTest, DerivativeWithIncorrectTypes3) {
    std::vector<std::pair<value::TypeTags, value::Value>> inputValues = {
        {value::TypeTags::Null, 0},
    };

    std::vector<std::pair<value::TypeTags, value::Value>> sortByValues = {
        {value::TypeTags::NumberInt32, 0},
    };

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7821012);
}
}  // namespace mongo::sbe
