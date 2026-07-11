// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {
class SBEBuiltinNewArrayFromRangeTest : public EExpressionTestFixture {
protected:
    using TypedValue = std::pair<value::TypeTags, value::Value>;

    void runAndAssertExpression(TypedValue start,
                                TypedValue end,
                                TypedValue step,
                                TypedValue expectedRes) {
        auto actualValue = runExpression(start, end, step);

        auto [compareTag, compareValue] = value::compareValue(
            actualValue.first, actualValue.second, expectedRes.first, expectedRes.second);

        ASSERT_EQ(compareTag, value::TypeTags::NumberInt32);
        ASSERT_EQ(compareValue, 0);

        value::releaseValue(actualValue.first, actualValue.second);
        value::releaseValue(expectedRes.first, expectedRes.second);
    }

    TypedValue runExpression(TypedValue start, TypedValue end, TypedValue step) {
        auto startCopy = value::copyValue(start.first, start.second);
        auto startExpr = makeE<EConstant>(startCopy.first, startCopy.second);

        auto endCopy = value::copyValue(end.first, end.second);
        auto endExpr = makeE<EConstant>(endCopy.first, endCopy.second);

        auto stepCopy = value::copyValue(step.first, step.second);
        auto stepExpr = makeE<EConstant>(stepCopy.first, stepCopy.second);

        auto getElementExpr =
            makeE<EFunction>(EFn::kNewArrayFromRange,
                             makeEs(std::move(startExpr), std::move(endExpr), std::move(stepExpr)));
        auto compiledExpr = compileExpression(*getElementExpr);

        return runCompiledExpression(compiledExpr.get());
    }
};

TEST_F(SBEBuiltinNewArrayFromRangeTest, PositiveRange) {
    auto expectedRes = makeArray(BSON_ARRAY(1 << 2 << 3));

    auto start = makeInt32(1);
    auto end = makeInt32(4);
    auto step = makeInt32(1);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, PositiveRangeBigStep) {
    auto expectedRes = makeArray(BSON_ARRAY(1 << 4 << 7 << 10));

    auto start = makeInt32(1);
    auto end = makeInt32(11);
    auto step = makeInt32(3);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, NegativeRange) {
    auto expectedRes = makeArray(BSON_ARRAY(-1 << -2 << -3));

    auto start = makeInt32(-1);
    auto end = makeInt32(-4);
    auto step = makeInt32(-1);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, NegativeRangeBigStep) {
    auto expectedRes = makeArray(BSON_ARRAY(-1 << -4 << -7 << -10));

    auto start = makeInt32(-1);
    auto end = makeInt32(-11);
    auto step = makeInt32(-3);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, StartGreaterThanEnd) {
    auto expectedRes = makeArray(BSONArray());

    auto start = makeInt32(4);
    auto end = makeInt32(1);
    auto step = makeInt32(1);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, StartEqualEnd) {
    auto expectedRes = makeArray(BSONArray());

    auto start = makeInt32(4);
    auto end = makeInt32(4);
    auto step = makeInt32(1);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, NonInt32Start) {
    auto expectedRes = std::pair<value::TypeTags, value::Value>{value::TypeTags::Nothing, 0};

    auto start = makeInt64(12147483647);
    auto end = makeInt32(4);
    auto step = makeInt32(1);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, NonInt32End) {
    auto expectedRes = std::pair<value::TypeTags, value::Value>{value::TypeTags::Nothing, 0};

    auto start = makeInt32(4);
    auto end = makeInt64(12147483647);
    auto step = makeInt32(1);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, NonInt32Step) {
    auto expectedRes = std::pair<value::TypeTags, value::Value>{value::TypeTags::Nothing, 0};

    auto start = makeInt32(1);
    auto end = makeInt32(4);
    auto step = makeInt64(12147483647);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}

TEST_F(SBEBuiltinNewArrayFromRangeTest, ZeroStep) {
    auto expectedRes = std::pair<value::TypeTags, value::Value>{value::TypeTags::Nothing, 0};

    auto start = makeInt32(1);
    auto end = makeInt32(4);
    auto step = makeInt32(0);

    runAndAssertExpression(
        std::move(start), std::move(end), std::move(step), std::move(expectedRes));
}
}  // namespace mongo::sbe
