/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
            makeE<EFunction>("newArrayFromRange",
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
