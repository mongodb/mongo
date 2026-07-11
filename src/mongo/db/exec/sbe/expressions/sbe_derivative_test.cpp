// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mongo::sbe {

enum class DerivativeOp { kAdd, kRemove };

class SBEDerivativeTest : public EExpressionTestFixture {
public:
    void runAndAssertExpression(boost::optional<int64_t> unitMillis,
                                const std::vector<value::TagValueOwned>& inputValues,
                                const std::vector<value::TagValueOwned>& sortByValues,
                                const std::vector<DerivativeOp>& operations,
                                const std::vector<value::TagValueOwned>& expValues) {
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
            sbe::makeE<sbe::EFunction>(EFn::kAggDerivativeFinalize,
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
                inputAccessorFirst.reset(inputValues[firstIdx].tag(),
                                         inputValues[firstIdx].value());
                sortByAccessorFirst.reset(sortByValues[firstIdx].tag(),
                                          sortByValues[firstIdx].value());
                inputAccessorLast.reset(inputValues[lastIdx].tag(), inputValues[lastIdx].value());
                sortByAccessorLast.reset(sortByValues[lastIdx].tag(),
                                         sortByValues[lastIdx].value());
            } else {
                inputAccessorFirst.reset();
                sortByAccessorFirst.reset();
                inputAccessorLast.reset();
                sortByAccessorLast.reset();
            }

            auto out = runCompiledExpression(compiledDerivativeFinalize.get());
            value::TagValueOwned outOwned = value::TagValueOwned::fromRaw(out);

            ASSERT_EQ(outOwned.tag(), expValues[i].tag());
            ASSERT_THAT(out, ValueEq(expValues[i].view()));
        }
    }

    void runAndAssertErrorCode(boost::optional<int64_t> unitMillis,
                               const std::vector<value::TagValueOwned>& inputValues,
                               const std::vector<value::TagValueOwned>& sortByValues,
                               const std::vector<DerivativeOp>& operations,
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
            sbe::makeE<sbe::EFunction>(EFn::kAggDerivativeFinalize,
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
                        inputAccessorFirst.reset(inputValues[firstIdx].tag(),
                                                 inputValues[firstIdx].value());
                        sortByAccessorFirst.reset(sortByValues[firstIdx].tag(),
                                                  sortByValues[firstIdx].value());
                        inputAccessorLast.reset(inputValues[lastIdx].tag(),
                                                inputValues[lastIdx].value());
                        sortByAccessorLast.reset(sortByValues[lastIdx].tag(),
                                                 sortByValues[lastIdx].value());
                    } else {
                        inputAccessorFirst.reset();
                        sortByAccessorFirst.reset();
                        inputAccessorLast.reset();
                        sortByAccessorLast.reset();
                    }

                    value::TagValueOwned outOwned = value::TagValueOwned::fromRaw(
                        runCompiledExpression(compiledDerivativeFinalize.get()));
                }
                return Status::OK();
            } catch (AssertionException& ex) {
                return ex.toStatus();
            }
        }();
        ASSERT_FALSE(status.isOK());
        ASSERT_EQ(status.code(), expErrCode);
    }
};

TEST_F(SBEDerivativeTest, DerivatedSortedByDate) {
    auto inputValues =
        makeOwnedVector({{value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
                         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
                         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)},
                         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(8)}});
    auto sortByValues = makeOwnedVector({{value::TypeTags::Date, 1589811030000LL},
                                         {value::TypeTags::Date, 1589811060000LL},
                                         {value::TypeTags::Date, 1589811090000LL},
                                         {value::TypeTags::Date, 1589811120000LL}});

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove};
    auto expValues =
        makeOwnedVector({{value::TypeTags::Null, 0},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(120.0)},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(180.0)},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(280.0)},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(360.0)},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(480.0)},
                         {value::TypeTags::Null, 0},
                         {value::TypeTags::Null, 0}});

    boost::optional<int64_t> unitMillis = 60LL * 60LL * 1000LL;  // hour unit
    runAndAssertExpression(unitMillis, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivativeWithMixedNumericTypes) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-20ll)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-30.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{-40.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(-50.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(-60ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-70)},
    });

    auto sortByValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0)},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128{4.0}).second},
        {value::TypeTags::NumberDouble, value::bitcastFrom<double>(5.0)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(6ll)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(7)},
    });

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

    auto expValues = makeOwnedVector({
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
    });

    runAndAssertExpression(boost::none, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivatedWithDateInputType) {
    auto inputValues = makeOwnedVector({{value::TypeTags::Date, 1589811030000LL},
                                        {value::TypeTags::Date, 1589811060000LL},
                                        {value::TypeTags::Date, 1589811090000LL}});
    auto sortByValues =
        makeOwnedVector({{value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
                         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
                         {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(4)}});

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kAdd,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove,
                                               DerivativeOp::kRemove};
    auto expValues =
        makeOwnedVector({{value::TypeTags::Null, 0},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(30000.0)},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(20000.0)},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(15000.0)},
                         {value::TypeTags::Null, 0},
                         {value::TypeTags::Null, 0}});

    runAndAssertExpression(boost::none, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivativeWithNaNAndInfinityValues) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::NumberInt64, 10},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::quiet_NaN())},
        {value::TypeTags::NumberDecimal, value::makeCopyDecimal(Decimal128::kNegativeNaN).second},
        {value::TypeTags::NumberDouble,
         value::bitcastFrom<double>(std::numeric_limits<double>::infinity())},
        {value::TypeTags::NumberDecimal,
         value::makeCopyDecimal(Decimal128::kNegativeInfinity).second},
    });

    auto sortByValues = makeOwnedVector({
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(2)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(3)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(4)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int32_t>(5)},
    });

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

    auto expValues = makeOwnedVector({
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
    });

    runAndAssertExpression(boost::none, inputValues, sortByValues, derivativeOps, expValues);
}

TEST_F(SBEDerivativeTest, DerivativeWithMixOfNumericAndDateTypeInput) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::Date, 1589811030000LL},
    });

    auto sortByValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(2)},
    });

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd, DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7821012);
}

TEST_F(SBEDerivativeTest, DerivativeWithSortByDatesAndNoUnit) {
    auto inputValues =
        makeOwnedVector({{value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.95)},
                         {value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.98)}});
    auto sortByValues = makeOwnedVector(
        {{value::TypeTags::Date, 1589811030000LL}, {value::TypeTags::Date, 1589811060000LL}});

    std::vector<DerivativeOp> derivativeOps = {
        DerivativeOp::kAdd, DerivativeOp::kAdd, DerivativeOp::kRemove, DerivativeOp::kRemove};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7993410);
}

TEST_F(SBEDerivativeTest, DerivativeWithSortByNumbersAndDateUnit) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(10)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10ll)},
    });

    auto sortByValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
        {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2l)},
    });

    std::vector<DerivativeOp> derivativeOps = {
        DerivativeOp::kAdd, DerivativeOp::kAdd, DerivativeOp::kRemove, DerivativeOp::kRemove};

    boost::optional<int64_t> unitMillis = 60LL * 60LL * 1000LL;
    runAndAssertErrorCode(unitMillis, inputValues, sortByValues, derivativeOps, 7993409);
}

TEST_F(SBEDerivativeTest, DerivativeWithIncorrectTypes1) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
    });

    auto sortByValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
    });

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7821012);
}

TEST_F(SBEDerivativeTest, DerivativeWithIncorrectTypes2) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
    });

    auto sortByValues = makeOwnedVector({
        {value::TypeTags::StringSmall, value::makeSmallString("a").second},
    });

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7993410);
}

TEST_F(SBEDerivativeTest, DerivativeWithIncorrectTypes3) {
    auto inputValues = makeOwnedVector({
        {value::TypeTags::Null, 0},
    });

    auto sortByValues = makeOwnedVector({
        {value::TypeTags::NumberInt32, 0},
    });

    std::vector<DerivativeOp> derivativeOps = {DerivativeOp::kAdd};

    runAndAssertErrorCode(boost::none, inputValues, sortByValues, derivativeOps, 7821012);
}
}  // namespace mongo::sbe
